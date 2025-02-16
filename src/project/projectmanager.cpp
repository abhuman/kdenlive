/*
SPDX-FileCopyrightText: 2014 Till Theato <root@ttill.de>
SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "projectmanager.h"
#include "bin/bin.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "kdenlivesettings.h"
#include "mainwindow.h"
#include "monitor/monitormanager.h"
#include "profiles/profilemodel.hpp"
#include "project/dialogs/archivewidget.h"
#include "project/dialogs/backupwidget.h"
#include "project/dialogs/noteswidget.h"
#include "project/dialogs/projectsettings.h"
#include "utils/thumbnailcache.hpp"
#include "xml/xml.hpp"
#include <audiomixer/mixermanager.hpp>
#include <lib/localeHandling.h>

// Temporary for testing
#include "bin/model/markerlistmodel.hpp"

#include "profiles/profilerepository.hpp"
#include "project/notesplugin.h"
#include "timeline2/model/builders/meltBuilder.hpp"
#include "timeline2/view/timelinecontroller.h"
#include "timeline2/view/timelinewidget.h"

#include "utils/KMessageBox_KdenliveCompat.h"
#include <KActionCollection>
#include <KConfigGroup>
#include <KJob>
#include <KJobWidgets>
#include <KLocalizedString>
#include <KMessageBox>
#include <KRecentDirs>
#include <kcoreaddons_version.h>

#include "kdenlive_debug.h"
#include <QAction>
#include <QCryptographicHash>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMimeDatabase>
#include <QMimeType>
#include <QProgressDialog>
#include <QSaveFile>
#include <QTimeZone>

static QString getProjectNameFilters(bool ark = true)
{
    auto filter = i18n("Kdenlive Project (*.kdenlive)");
    if (ark) {
        filter.append(";;" + i18n("Archived Project (*.tar.gz *.zip)"));
    }
    return filter;
}

ProjectManager::ProjectManager(QObject *parent)
    : QObject(parent)
    , m_mainTimelineModel(nullptr)
{
    m_fileRevert = KStandardAction::revert(this, SLOT(slotRevert()), pCore->window()->actionCollection());
    m_fileRevert->setIcon(QIcon::fromTheme(QStringLiteral("document-revert")));
    m_fileRevert->setEnabled(false);

    QAction *a = KStandardAction::open(this, SLOT(openFile()), pCore->window()->actionCollection());
    a->setIcon(QIcon::fromTheme(QStringLiteral("document-open")));
    a = KStandardAction::saveAs(this, SLOT(saveFileAs()), pCore->window()->actionCollection());
    a->setIcon(QIcon::fromTheme(QStringLiteral("document-save-as")));
    a = KStandardAction::openNew(this, SLOT(newFile()), pCore->window()->actionCollection());
    a->setIcon(QIcon::fromTheme(QStringLiteral("document-new")));
    m_recentFilesAction = KStandardAction::openRecent(this, SLOT(openFile(QUrl)), pCore->window()->actionCollection());

    QAction *saveCopyAction = new QAction(QIcon::fromTheme(QStringLiteral("document-save-as")), i18n("Save Copy…"), this);
    pCore->window()->addAction(QStringLiteral("file_save_copy"), saveCopyAction);
    connect(saveCopyAction, &QAction::triggered, this, [this] { saveFileAs(true); });

    QAction *backupAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-undo")), i18n("Open Backup File…"), this);
    pCore->window()->addAction(QStringLiteral("open_backup"), backupAction);
    connect(backupAction, SIGNAL(triggered(bool)), SLOT(slotOpenBackup()));

    m_notesPlugin = new NotesPlugin(this);

    m_autoSaveTimer.setSingleShot(true);
    connect(&m_autoSaveTimer, &QTimer::timeout, this, &ProjectManager::slotAutoSave);

    // Ensure the default data folder exist
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    dir.mkpath(QStringLiteral(".backup"));
    dir.mkdir(QStringLiteral("titles"));
}

ProjectManager::~ProjectManager() = default;

void ProjectManager::slotLoadOnOpen()
{
    m_loading = true;
    if (m_startUrl.isValid()) {
        openFile();
    } else if (KdenliveSettings::openlastproject()) {
        openLastFile();
    } else {
        newFile(false);
    }
    if (!m_loadClipsOnOpen.isEmpty() && (m_project != nullptr)) {
        const QStringList list = m_loadClipsOnOpen.split(QLatin1Char(','));
        QList<QUrl> urls;
        urls.reserve(list.count());
        for (const QString &path : list) {
            // qCDebug(KDENLIVE_LOG) << QDir::current().absoluteFilePath(path);
            urls << QUrl::fromLocalFile(QDir::current().absoluteFilePath(path));
        }
        pCore->bin()->droppedUrls(urls);
    }
    m_loadClipsOnOpen.clear();
    m_loading = false;
    emit pCore->closeSplash();
    // Release startup crash lock file
    QFile lockFile(QDir::temp().absoluteFilePath(QStringLiteral("kdenlivelock")));
    lockFile.remove();
    // For some reason Qt seems to be doing some stuff that modifies the tabs text after window is shown, so use a timer
    QTimer::singleShot(1000, this, []() {
        QList<QTabBar *> tabbars = pCore->window()->findChildren<QTabBar *>();
        for (QTabBar *tab : qAsConst(tabbars)) {
            // Fix tabbar tooltip containing ampersand
            for (int i = 0; i < tab->count(); i++) {
                tab->setTabToolTip(i, tab->tabText(i).replace('&', ""));
            }
        }
    });
    pCore->window()->checkMaxCacheSize();
}

void ProjectManager::init(const QUrl &projectUrl, const QString &clipList)
{
    m_startUrl = projectUrl;
    m_loadClipsOnOpen = clipList;
}

void ProjectManager::newFile(bool showProjectSettings)
{
    QString profileName = KdenliveSettings::default_profile();
    if (profileName.isEmpty()) {
        profileName = pCore->getCurrentProfile()->path();
    }
    newFile(profileName, showProjectSettings);
}

void ProjectManager::newFile(QString profileName, bool showProjectSettings)
{
    QUrl startFile = QUrl::fromLocalFile(KdenliveSettings::defaultprojectfolder() + QStringLiteral("/_untitled.kdenlive"));
    if (checkForBackupFile(startFile, true)) {
        return;
    }
    m_fileRevert->setEnabled(false);
    QString projectFolder;
    QMap<QString, QString> documentProperties;
    QMap<QString, QString> documentMetadata;
    QPair<int, int> projectTracks{KdenliveSettings::videotracks(), KdenliveSettings::audiotracks()};
    int audioChannels = 2;
    if (KdenliveSettings::audio_channels() == 1) {
        audioChannels = 4;
    } else if (KdenliveSettings::audio_channels() == 2) {
        audioChannels = 6;
    }
    pCore->monitorManager()->resetDisplay();
    QString documentId = QString::number(QDateTime::currentMSecsSinceEpoch());
    documentProperties.insert(QStringLiteral("documentid"), documentId);
    bool sameProjectFolder = KdenliveSettings::sameprojectfolder();
    if (!showProjectSettings) {
        if (!closeCurrentDocument()) {
            return;
        }
        if (KdenliveSettings::customprojectfolder()) {
            projectFolder = KdenliveSettings::defaultprojectfolder();
            QDir folder(projectFolder);
            if (!projectFolder.endsWith(QLatin1Char('/'))) {
                projectFolder.append(QLatin1Char('/'));
            }
            documentProperties.insert(QStringLiteral("storagefolder"), folder.absoluteFilePath(documentId));
        }
    } else {
        QPointer<ProjectSettings> w = new ProjectSettings(nullptr, QMap<QString, QString>(), QStringList(), projectTracks.first, projectTracks.second,
                                                          audioChannels, KdenliveSettings::defaultprojectfolder(), false, true, pCore->window());
        connect(w.data(), &ProjectSettings::refreshProfiles, pCore->window(), &MainWindow::slotRefreshProfiles);
        if (w->exec() != QDialog::Accepted) {
            delete w;
            return;
        }
        if (!closeCurrentDocument()) {
            delete w;
            return;
        }
        if (KdenliveSettings::videothumbnails() != w->enableVideoThumbs()) {
            pCore->window()->slotSwitchVideoThumbs();
        }
        if (KdenliveSettings::audiothumbnails() != w->enableAudioThumbs()) {
            pCore->window()->slotSwitchAudioThumbs();
        }
        profileName = w->selectedProfile();
        projectFolder = w->storageFolder();
        projectTracks = w->tracks();
        audioChannels = w->audioChannels();
        documentProperties.insert(QStringLiteral("enableproxy"), QString::number(int(w->useProxy())));
        documentProperties.insert(QStringLiteral("generateproxy"), QString::number(int(w->generateProxy())));
        documentProperties.insert(QStringLiteral("proxyminsize"), QString::number(w->proxyMinSize()));
        documentProperties.insert(QStringLiteral("proxyparams"), w->proxyParams());
        documentProperties.insert(QStringLiteral("proxyextension"), w->proxyExtension());
        documentProperties.insert(QStringLiteral("proxyresize"), QString::number(w->proxyResize()));
        documentProperties.insert(QStringLiteral("audioChannels"), QString::number(w->audioChannels()));
        documentProperties.insert(QStringLiteral("generateimageproxy"), QString::number(int(w->generateImageProxy())));
        QString preview = w->selectedPreview();
        if (!preview.isEmpty()) {
            documentProperties.insert(QStringLiteral("previewparameters"), preview.section(QLatin1Char(';'), 0, 0));
            documentProperties.insert(QStringLiteral("previewextension"), preview.section(QLatin1Char(';'), 1, 1));
        }
        documentProperties.insert(QStringLiteral("proxyimageminsize"), QString::number(w->proxyImageMinSize()));
        if (!projectFolder.isEmpty()) {
            if (!projectFolder.endsWith(QLatin1Char('/'))) {
                projectFolder.append(QLatin1Char('/'));
            }
            documentProperties.insert(QStringLiteral("storagefolder"), projectFolder + documentId);
        }
        if (w->useExternalProxy()) {
            documentProperties.insert(QStringLiteral("enableexternalproxy"), QStringLiteral("1"));
            documentProperties.insert(QStringLiteral("externalproxyparams"), w->externalProxyParams());
        }
        sameProjectFolder = w->docFolderAsStorageFolder();
        // Metadata
        documentMetadata = w->metadata();
        delete w;
    }
    m_notesPlugin->clear();
    pCore->bin()->cleanDocument();
    KdenliveDoc *doc = new KdenliveDoc(projectFolder, pCore->window()->m_commandStack, profileName, documentProperties, documentMetadata, projectTracks, audioChannels, pCore->window());
    doc->m_autosave = new KAutoSaveFile(startFile, doc);
    doc->m_sameProjectFolder = sameProjectFolder;
    ThumbnailCache::get()->clearCache();
    pCore->bin()->setDocument(doc);
    m_project = doc;
    updateTimeline(0, QString(), QString(), QDateTime(), 0);
    pCore->window()->connectDocument();
    pCore->mixer()->setModel(m_mainTimelineModel);
    pCore->monitorManager()->activateMonitor(Kdenlive::ProjectMonitor);
    bool disabled = m_project->getDocumentProperty(QStringLiteral("disabletimelineeffects")) == QLatin1String("1");
    QAction *disableEffects = pCore->window()->actionCollection()->action(QStringLiteral("disable_timeline_effects"));
    if (disableEffects) {
        if (disabled != disableEffects->isChecked()) {
            disableEffects->blockSignals(true);
            disableEffects->setChecked(disabled);
            disableEffects->blockSignals(false);
        }
    }
    emit docOpened(m_project);
    m_lastSave.start();
}

void ProjectManager::testSetActiveDocument(KdenliveDoc *doc, std::shared_ptr<TimelineItemModel> timeline)
{
    m_project = doc;
    m_project->addTimeline(doc->uuid(), timeline);
    m_mainTimelineModel = timeline;
}

std::shared_ptr<TimelineItemModel> ProjectManager::getTimeline()
{
    return m_mainTimelineModel;
}

bool ProjectManager::testSaveFileAs(const QString &outputFileName)
{
    QString saveFolder = QFileInfo(outputFileName).absolutePath();
    QMap<QString, QString> docProperties = m_project->documentProperties();
    docProperties.insert(QStringLiteral("timelineHash"), m_mainTimelineModel->timelineHash().toHex());
    pCore->projectItemModel()->saveDocumentProperties(docProperties, QMap<QString, QString>());
    QString scene = m_mainTimelineModel->sceneList(saveFolder);

    QSaveFile file(outputFileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qDebug() << "//////  ERROR writing to file: " << outputFileName;
        return false;
    }

    file.write(scene.toUtf8());
    if (!file.commit()) {
        qDebug() << "Cannot write to file %1";
        return false;
    }
    return true;
}

bool ProjectManager::closeCurrentDocument(bool saveChanges, bool quit)
{
    // Disable autosave
    m_autoSaveTimer.stop();
    if ((m_project != nullptr) && m_project->isModified() && saveChanges) {
        QString message;
        if (m_project->url().fileName().isEmpty()) {
            message = i18n("Save changes to document?");
        } else {
            message = i18n("The project <b>\"%1\"</b> has been changed.\nDo you want to save your changes?", m_project->url().fileName());
        }

        switch (KMessageBox::warningTwoActionsCancel(pCore->window(), message, {}, KStandardGuiItem::save(), KStandardGuiItem::dontSave())) {
        case KMessageBox::PrimaryAction:
            // save document here. If saving fails, return false;
            if (!saveFile()) {
                return false;
            }
            break;
        case KMessageBox::Cancel:
            return false;
            break;
        default:
            break;
        }
    }
    if (m_project) {
        ::mlt_pool_purge();
        pCore->cleanup();
        if (!quit && !qApp->isSavingSession()) {
            pCore->bin()->abortOperations();
        }
        pCore->window()->getCurrentTimeline()->unsetModel();
        pCore->window()->resetSubtitles();
        if (m_mainTimelineModel) {
            m_mainTimelineModel->prepareClose();
        }
    }
    pCore->bin()->cleanDocument();
    if (!quit && !qApp->isSavingSession() && m_project) {
        emit pCore->window()->clearAssetPanel();
        pCore->monitorManager()->clipMonitor()->slotOpenClip(nullptr);
        delete m_project;
        m_project = nullptr;
    }
    pCore->mixer()->unsetModel();
    // Release model shared pointers
    m_mainTimelineModel.reset();
    return true;
}

bool ProjectManager::saveFileAs(const QString &outputFileName, bool saveACopy)
{
    pCore->monitorManager()->pauseActiveMonitor();
    QString oldProjectFolder =
        m_project->url().isEmpty() ? QString() : QFileInfo(m_project->url().toLocalFile()).absolutePath() + QStringLiteral("/cachefiles");
    // this was the old project folder in case the "save in project file location" setting was active

    // Sync document properties
    if (!saveACopy && outputFileName != m_project->url().toLocalFile()) {
        // Project filename changed
        pCore->window()->updateProjectPath(outputFileName);
    }
    prepareSave();
    QString saveFolder = QFileInfo(outputFileName).absolutePath();
    m_project->updateSubtitle(outputFileName);
    QString scene = projectSceneList(saveFolder);
    if (!m_replacementPattern.isEmpty()) {
        QMapIterator<QString, QString> i(m_replacementPattern);
        while (i.hasNext()) {
            i.next();
            scene.replace(i.key(), i.value());
        }
    }
    if (!m_project->saveSceneList(outputFileName, scene)) {
        return false;
    }
    QUrl url = QUrl::fromLocalFile(outputFileName);
    // Save timeline thumbnails
    std::unordered_map<QString, std::vector<int>> thumbKeys = pCore->window()->getCurrentTimeline()->controller()->getThumbKeys();
    pCore->projectItemModel()->updateCacheThumbnail(thumbKeys);
    // Remove duplicates
    for (auto p : thumbKeys) {
        std::sort(p.second.begin(), p.second.end());
        auto last = std::unique(p.second.begin(), p.second.end());
        p.second.erase(last, p.second.end());
    }
    ThumbnailCache::get()->saveCachedThumbs(thumbKeys);
    if (!saveACopy) {
        m_project->setUrl(url);
        // setting up autosave file in ~/.kde/data/stalefiles/kdenlive/
        // saved under file name
        // actual saving by KdenliveDoc::slotAutoSave() called by a timer 3 seconds after the document has been edited
        // This timer is set by KdenliveDoc::setModified()
        const QString projectId = QCryptographicHash::hash(url.fileName().toUtf8(), QCryptographicHash::Md5).toHex();
        QUrl autosaveUrl = QUrl::fromLocalFile(QFileInfo(outputFileName).absoluteDir().absoluteFilePath(projectId + QStringLiteral(".kdenlive")));
        if (m_project->m_autosave == nullptr) {
            // The temporary file is not opened or created until actually needed.
            // The file filename does not have to exist for KAutoSaveFile to be constructed (if it exists, it will not be touched).
            m_project->m_autosave = new KAutoSaveFile(autosaveUrl, m_project);
        } else {
            m_project->m_autosave->setManagedFile(autosaveUrl);
        }

        pCore->window()->setWindowTitle(m_project->description());
        m_project->setModified(false);
    }

    m_recentFilesAction->addUrl(url);
    // remember folder for next project opening
    KRecentDirs::add(QStringLiteral(":KdenliveProjectsFolder"), saveFolder);
    saveRecentFiles();
    if (!saveACopy) {
        m_fileRevert->setEnabled(true);
        pCore->window()->m_undoView->stack()->setClean();
        QString newProjectFolder(saveFolder + QStringLiteral("/cachefiles"));
        if (((oldProjectFolder.isEmpty() && m_project->m_sameProjectFolder) || m_project->projectTempFolder() == oldProjectFolder) &&
            newProjectFolder != m_project->projectTempFolder()) {
            KMessageBox::ButtonCode answer = KMessageBox::warningContinueCancel(
                pCore->window(), i18n("The location of the project file changed. You selected to use the location of the project file to save temporary files. "
                                      "This will move all temporary files from <b>%1</b> to <b>%2</b>, the project file will then be reloaded",
                                      m_project->projectTempFolder(), newProjectFolder));

            if (answer == KMessageBox::Continue) {
                // Proceed with move
                QString documentId = QDir::cleanPath(m_project->getDocumentProperty(QStringLiteral("documentid")));
                bool ok;
                documentId.toLongLong(&ok, 10);
                if (!ok || documentId.isEmpty()) {
                    KMessageBox::error(pCore->window(), i18n("Cannot perform operation, invalid document id: %1", documentId));
                } else {
                    QDir newDir(newProjectFolder);
                    QDir oldDir(m_project->projectTempFolder());
                    if (newDir.exists(documentId)) {
                        KMessageBox::error(pCore->window(),
                                           i18n("Cannot perform operation, target directory already exists: %1", newDir.absoluteFilePath(documentId)));
                    } else {
                        // Proceed with the move
                        moveProjectData(oldDir.absoluteFilePath(documentId), newDir.absolutePath());
                    }
                }
            }
        }
    }
    return true;
}

void ProjectManager::saveRecentFiles()
{
    KSharedConfigPtr config = KSharedConfig::openConfig();
    m_recentFilesAction->saveEntries(KConfigGroup(config, "Recent Files"));
    config->sync();
}

bool ProjectManager::saveFileAs(bool saveACopy)
{
    QFileDialog fd(pCore->window());
    if (saveACopy) {
        fd.setWindowTitle(i18nc("@title:window", "Save Copy"));
    }
    fd.setDirectory(m_project->url().isValid() ? m_project->url().adjusted(QUrl::RemoveFilename).toLocalFile() : KdenliveSettings::defaultprojectfolder());
    fd.setNameFilter(getProjectNameFilters(false));
    fd.setAcceptMode(QFileDialog::AcceptSave);
    fd.setFileMode(QFileDialog::AnyFile);
    fd.setDefaultSuffix(QStringLiteral("kdenlive"));
    if (fd.exec() != QDialog::Accepted || fd.selectedFiles().isEmpty()) {
        return false;
    }

    QString outputFile = fd.selectedFiles().constFirst();

    bool ok = false;
    QDir cacheDir = m_project->getCacheDir(CacheBase, &ok);
    if (ok) {
        QFile file(cacheDir.absoluteFilePath(QString::fromLatin1(QUrl::toPercentEncoding(QStringLiteral(".") + outputFile))));
        file.open(QIODevice::ReadWrite | QIODevice::Text);
        file.close();
    }
    return saveFileAs(outputFile, saveACopy);
}

bool ProjectManager::saveFile()
{
    if (!m_project) {
        // Calling saveFile before a project was created, something is wrong
        qCDebug(KDENLIVE_LOG) << "SaveFile called without project";
        return false;
    }
    if (m_project->url().isEmpty()) {
        return saveFileAs();
    }
    bool result = saveFileAs(m_project->url().toLocalFile());
    m_project->m_autosave->resize(0);
    return result;
}

void ProjectManager::openFile()
{
    if (m_startUrl.isValid()) {
        openFile(m_startUrl);
        m_startUrl.clear();
        return;
    }
    QUrl url = QFileDialog::getOpenFileUrl(pCore->window(), QString(), QUrl::fromLocalFile(KRecentDirs::dir(QStringLiteral(":KdenliveProjectsFolder"))),
                                           getProjectNameFilters());
    if (!url.isValid()) {
        return;
    }
    KRecentDirs::add(QStringLiteral(":KdenliveProjectsFolder"), url.adjusted(QUrl::RemoveFilename).toLocalFile());
    m_recentFilesAction->addUrl(url);
    saveRecentFiles();
    openFile(url);
}

void ProjectManager::openLastFile()
{
    if (m_recentFilesAction->selectableActionGroup()->actions().isEmpty()) {
        // No files in history
        newFile(false);
        return;
    }

    QAction *firstUrlAction = m_recentFilesAction->selectableActionGroup()->actions().last();
    if (firstUrlAction) {
        firstUrlAction->trigger();
    } else {
        newFile(false);
    }
}

// fix mantis#3160 separate check from openFile() so we can call it from newFile()
// to find autosaved files (in ~/.local/share/stalefiles/kdenlive) and recover it
bool ProjectManager::checkForBackupFile(const QUrl &url, bool newFile)
{
    // Check for autosave file that belong to the url we passed in.
    const QString projectId = QCryptographicHash::hash(url.fileName().toUtf8(), QCryptographicHash::Md5).toHex();
    QUrl autosaveUrl = newFile ? url : QUrl::fromLocalFile(QFileInfo(url.path()).absoluteDir().absoluteFilePath(projectId + QStringLiteral(".kdenlive")));
    QList<KAutoSaveFile *> staleFiles = KAutoSaveFile::staleFiles(autosaveUrl);
    QFileInfo sourceInfo(url.toLocalFile());
    QDateTime sourceTime;
    if (sourceInfo.exists()) {
        sourceTime = QFileInfo(url.toLocalFile()).lastModified();
    }
    KAutoSaveFile *orphanedFile = nullptr;
    // Check if we can have a lock on one of the file,
    // meaning it is not handled by any Kdenlive instance
    if (!staleFiles.isEmpty()) {
        for (KAutoSaveFile *stale : qAsConst(staleFiles)) {
            if (stale->open(QIODevice::QIODevice::ReadWrite)) {
                // Found orphaned autosave file
                if (!sourceTime.isValid() || QFileInfo(stale->fileName()).lastModified() > sourceTime) {
                    orphanedFile = stale;
                    break;
                }
            }
        }
    }

    if (orphanedFile) {
        if (KMessageBox::questionTwoActions(nullptr, i18n("Auto-saved file exist. Do you want to recover now?"), i18n("File Recovery"),
                                            KGuiItem(i18n("Recover")), KGuiItem(i18n("Do not recover"))) == KMessageBox::PrimaryAction) {
            doOpenFile(url, orphanedFile);
            return true;
        }
    }
    // remove the stale files
    for (KAutoSaveFile *stale : qAsConst(staleFiles)) {
        stale->open(QIODevice::ReadWrite);
        delete stale;
    }
    return false;
}

void ProjectManager::openFile(const QUrl &url)
{
    QMimeDatabase db;
    // Make sure the url is a Kdenlive project file
    QMimeType mime = db.mimeTypeForUrl(url);
    if (mime.inherits(QStringLiteral("application/x-compressed-tar")) || mime.inherits(QStringLiteral("application/zip"))) {
        // Opening a compressed project file, we need to process it
        // qCDebug(KDENLIVE_LOG)<<"Opening archive, processing";
        QPointer<ArchiveWidget> ar = new ArchiveWidget(url);
        if (ar->exec() == QDialog::Accepted) {
            openFile(QUrl::fromLocalFile(ar->extractedProjectFile()));
        } else if (m_startUrl.isValid()) {
            // we tried to open an invalid file from command line, init new project
            newFile(false);
        }
        delete ar;
        return;
    }

    /*if (!url.fileName().endsWith(".kdenlive")) {
        // This is not a Kdenlive project file, abort loading
        KMessageBox::error(pCore->window(), i18n("File %1 is not a Kdenlive project file", url.toLocalFile()));
        if (m_startUrl.isValid()) {
            // we tried to open an invalid file from command line, init new project
            newFile(false);
        }
        return;
    }*/

    if ((m_project != nullptr) && m_project->url() == url) {
        return;
    }

    if (!closeCurrentDocument()) {
        return;
    }
    if (checkForBackupFile(url)) {
        return;
    }
    pCore->displayMessage(i18n("Opening file %1", url.toLocalFile()), OperationCompletedMessage, 100);
    doOpenFile(url, nullptr);
}

void ProjectManager::doOpenFile(const QUrl &url, KAutoSaveFile *stale, bool isBackup)
{
    Q_ASSERT(m_project == nullptr);
    m_fileRevert->setEnabled(true);

    delete m_progressDialog;
    m_progressDialog = nullptr;
    ThumbnailCache::get()->clearCache();
    pCore->monitorManager()->resetDisplay();
    pCore->monitorManager()->activateMonitor(Kdenlive::ProjectMonitor);
    if (!m_loading) {
        m_progressDialog = new QProgressDialog(pCore->window());
        m_progressDialog->setWindowTitle(i18nc("@title:window", "Loading Project"));
        m_progressDialog->setCancelButton(nullptr);
        m_progressDialog->setLabelText(i18n("Loading project"));
        m_progressDialog->setMaximum(0);
        m_progressDialog->show();
    }
    m_notesPlugin->clear();

    DocOpenResult openResult = KdenliveDoc::Open(stale ? QUrl::fromLocalFile(stale->fileName()) : url,
        QString(), pCore->window()->m_commandStack, false, pCore->window());

    KdenliveDoc *doc;
    if (!openResult.isSuccessful() && !openResult.isAborted()) {
        if (!isBackup) {
            int answer = KMessageBox::warningTwoActionsCancel(
                pCore->window(), i18n("Cannot open the project file. Error:\n%1\nDo you want to open a backup file?", openResult.getError()),
                i18n("Error opening file"), KGuiItem(i18n("Open Backup")), KGuiItem(i18n("Recover")));
            if (answer == KMessageBox::PrimaryAction) { // Open Backup
                slotOpenBackup(url);
            } else if (answer == KMessageBox::SecondaryAction) { // Recover
                // if file was broken by Kdenlive 0.9.4, we can try recovering it. If successful, continue through rest of this function.
                openResult = KdenliveDoc::Open(stale ? QUrl::fromLocalFile(stale->fileName()) : url,
                    QString(), pCore->window()->m_commandStack, true, pCore->window());
                if (openResult.isSuccessful()) {
                    doc = openResult.getDocument().release();
                    doc->requestBackup();
                } else {
                    KMessageBox::error(pCore->window(), i18n("Could not recover corrupted file."));
                }
            }
        } else {
            KMessageBox::detailedError(pCore->window(), i18n("Could not open the backup project file."), openResult.getError());
        }
    } else {
        doc = openResult.getDocument().release();
    }

    // if we could not open the file, and could not recover (or user declined), stop now
    if (!openResult.isSuccessful()) {
        delete m_progressDialog;
        m_progressDialog = nullptr;
        // Open default blank document
        newFile(false);
        return;
    }

    if (openResult.wasUpgraded()) {
        pCore->displayMessage(i18n("Your project was upgraded, a backup will be created on next save"),
            ErrorMessage);
    } else if (openResult.wasModified()) {
        pCore->displayMessage(i18n("Your project was modified on opening, a backup will be created on next save"),
            ErrorMessage);
    }
    pCore->displayMessage(QString(), OperationCompletedMessage);


    if (stale == nullptr) {
        const QString projectId = QCryptographicHash::hash(url.fileName().toUtf8(), QCryptographicHash::Md5).toHex();
        QUrl autosaveUrl = QUrl::fromLocalFile(QFileInfo(url.path()).absoluteDir().absoluteFilePath(projectId + QStringLiteral(".kdenlive")));
        stale = new KAutoSaveFile(autosaveUrl, doc);
        doc->m_autosave = stale;
    } else {
        doc->m_autosave = stale;
        stale->setParent(doc);
        // if loading from an autosave of unnamed file, or restore failed then keep unnamed
        bool loadingFailed = doc->url().isEmpty();
        if (url.fileName().contains(QStringLiteral("_untitled.kdenlive"))) {
            doc->setUrl(QUrl());
            doc->setModified(true);
        } else if (!loadingFailed) {
            doc->setUrl(url);
        }
        doc->setModified(!loadingFailed);
        stale->setParent(doc);
    }
    if (m_progressDialog) {
        m_progressDialog->setLabelText(i18n("Loading clips"));
        m_progressDialog->setMaximum(doc->clipsCount());
    } else {
        emit pCore->loadingMessageUpdated(QString(), 0, doc->clipsCount());
    }

    pCore->bin()->setDocument(doc);

    // Set default target tracks to upper audio / lower video tracks
    m_project = doc;
    QDateTime documentDate = QFileInfo(m_project->url().toLocalFile()).lastModified();

    if (!updateTimeline(m_project->getDocumentProperty(QStringLiteral("position")).toInt(), m_project->getDocumentProperty(QStringLiteral("previewchunks")),
                        m_project->getDocumentProperty(QStringLiteral("dirtypreviewchunks")), documentDate,
                        m_project->getDocumentProperty(QStringLiteral("disablepreview")).toInt())) {
        delete m_progressDialog;
        m_progressDialog = nullptr;
        return;
    }
    pCore->window()->connectDocument();
    pCore->mixer()->setModel(m_mainTimelineModel);
    m_mainTimelineModel->updateFieldOrderFilter(pCore->getCurrentProfile());
    emit docOpened(m_project);
    pCore->displayMessage(QString(), OperationCompletedMessage, 100);
    m_lastSave.start();
    delete m_progressDialog;
    m_progressDialog = nullptr;
}

void ProjectManager::slotRevert()
{
    if (m_project->isModified() &&
        KMessageBox::warningContinueCancel(pCore->window(),
                                           i18n("This will delete all changes made since you last saved your project. Are you sure you want to continue?"),
                                           i18n("Revert to last saved version")) == KMessageBox::Cancel) {
        return;
    }
    QUrl url = m_project->url();
    if (closeCurrentDocument(false)) {
        doOpenFile(url, nullptr);
    }
}

KdenliveDoc *ProjectManager::current()
{
    return m_project;
}

bool ProjectManager::slotOpenBackup(const QUrl &url)
{
    QUrl projectFile;
    QUrl projectFolder;
    QString projectId;
    if (url.isValid()) {
        // we could not open the project file, guess where the backups are
        projectFolder = QUrl::fromLocalFile(KdenliveSettings::defaultprojectfolder());
        projectFile = url;
    } else {
        projectFolder = QUrl::fromLocalFile(m_project ? m_project->projectTempFolder() : QString());
        projectFile = m_project->url();
        projectId = m_project->getDocumentProperty(QStringLiteral("documentid"));
    }
    bool result = false;
    QPointer<BackupWidget> dia = new BackupWidget(projectFile, projectFolder, projectId, pCore->window());
    if (dia->exec() == QDialog::Accepted) {
        QString requestedBackup = dia->selectedFile();
        m_project->backupLastSavedVersion(projectFile.toLocalFile());
        closeCurrentDocument(false);
        doOpenFile(QUrl::fromLocalFile(requestedBackup), nullptr, true);
        if (m_project) {
            if (!m_project->url().isEmpty()) {
                // Only update if restore succeeded
                pCore->window()->slotEditSubtitle();
                m_project->setUrl(projectFile);
                m_project->setModified(true);
            }
            pCore->window()->setWindowTitle(m_project->description());
            result = true;
        }
    }
    delete dia;
    return result;
}

KRecentFilesAction *ProjectManager::recentFilesAction()
{
    return m_recentFilesAction;
}

void ProjectManager::slotStartAutoSave()
{
    if (m_lastSave.elapsed() > 300000) {
        // If the project was not saved in the last 5 minute, force save
        m_autoSaveTimer.stop();
        slotAutoSave();
    } else {
        m_autoSaveTimer.start(3000); // will trigger slotAutoSave() in 3 seconds
    }
}

void ProjectManager::slotAutoSave()
{
    prepareSave();
    QString saveFolder = m_project->url().adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash).toLocalFile();
    QString scene = projectSceneList(saveFolder);
    if (!m_replacementPattern.isEmpty()) {
        QMapIterator<QString, QString> i(m_replacementPattern);
        while (i.hasNext()) {
            i.next();
            scene.replace(i.key(), i.value());
        }
    }
    if (!scene.contains(QLatin1String("<track "))) {
        // In some unexplained cases, the MLT playlist is corrupted and all tracks are deleted. Don't save in that case.
        pCore->displayMessage(i18n("Project was corrupted, cannot backup. Please close and reopen your project file to recover last backup"), ErrorMessage);
        return;
    }
    m_project->slotAutoSave(scene);
    m_lastSave.start();
}

QString ProjectManager::projectSceneList(const QString &outputFolder, const QString &overlayData)
{
    // Disable multitrack view and overlay
    bool isMultiTrack = pCore->monitorManager()->isMultiTrack();
    bool hasPreview = pCore->window()->getCurrentTimeline()->controller()->hasPreviewTrack();
    bool isTrimming = pCore->monitorManager()->isTrimming();
    if (isMultiTrack) {
        pCore->window()->getCurrentTimeline()->controller()->slotMultitrackView(false, false);
    }
    if (hasPreview) {
        pCore->window()->getCurrentTimeline()->controller()->updatePreviewConnection(false);
    }
    if (isTrimming) {
        pCore->window()->getCurrentTimeline()->controller()->requestEndTrimmingMode();
    }
    pCore->mixer()->pauseMonitoring(true);
    QString scene = m_mainTimelineModel->sceneList(outputFolder, QString(), overlayData);
    pCore->mixer()->pauseMonitoring(false);
    if (isMultiTrack) {
        pCore->window()->getCurrentTimeline()->controller()->slotMultitrackView(true, false);
    }
    if (hasPreview) {
        pCore->window()->getCurrentTimeline()->controller()->updatePreviewConnection(true);
    }
    if (isTrimming) {
        pCore->window()->getCurrentTimeline()->controller()->requestStartTrimmingMode();
    }
    return scene;
}

void ProjectManager::setDocumentNotes(const QString &notes)
{
    if (m_notesPlugin) {
        m_notesPlugin->widget()->setHtml(notes);
    }
}

QString ProjectManager::documentNotes() const
{
    QString text = m_notesPlugin->widget()->toPlainText().simplified();
    if (text.isEmpty()) {
        return QString();
    }
    return m_notesPlugin->widget()->toHtml();
}

void ProjectManager::slotAddProjectNote()
{
    m_notesPlugin->showDock();
    m_notesPlugin->widget()->setFocus();
    m_notesPlugin->widget()->addProjectNote();
}

void ProjectManager::slotAddTextNote(const QString &text)
{
    m_notesPlugin->showDock();
    m_notesPlugin->widget()->setFocus();
    m_notesPlugin->widget()->addTextNote(text);
}

void ProjectManager::prepareSave()
{
    pCore->projectItemModel()->saveDocumentProperties(pCore->window()->getCurrentTimeline()->controller()->documentProperties(), m_project->metadata());
    pCore->bin()->saveFolderState();
    pCore->projectItemModel()->saveProperty(QStringLiteral("kdenlive:documentnotes"), documentNotes());
    pCore->projectItemModel()->saveProperty(QStringLiteral("kdenlive:docproperties.groups"), m_mainTimelineModel->groupsData());
}

void ProjectManager::slotResetProfiles(bool reloadThumbs)
{
    m_project->resetProfile(reloadThumbs);
    pCore->monitorManager()->updateScopeSource();
}

void ProjectManager::slotResetConsumers(bool fullReset)
{
    pCore->monitorManager()->resetConsumers(fullReset);
}

void ProjectManager::disableBinEffects(bool disable, bool refreshMonitor)
{
    if (m_project) {
        if (disable) {
            m_project->setDocumentProperty(QStringLiteral("disablebineffects"), QString::number(1));
        } else {
            m_project->setDocumentProperty(QStringLiteral("disablebineffects"), QString());
        }
    }
    if (refreshMonitor) {
        pCore->monitorManager()->refreshProjectMonitor();
        pCore->monitorManager()->refreshClipMonitor();
    }
}

void ProjectManager::slotDisableTimelineEffects(bool disable)
{
    if (disable) {
        m_project->setDocumentProperty(QStringLiteral("disabletimelineeffects"), QString::number(true));
    } else {
        m_project->setDocumentProperty(QStringLiteral("disabletimelineeffects"), QString());
    }
    m_mainTimelineModel->setTimelineEffectsEnabled(!disable);
    pCore->monitorManager()->refreshProjectMonitor();
}

void ProjectManager::slotSwitchTrackDisabled()
{
    pCore->window()->getCurrentTimeline()->controller()->switchTrackDisabled();
}

void ProjectManager::slotSwitchTrackLock()
{
    pCore->window()->getCurrentTimeline()->controller()->switchTrackLock();
}

void ProjectManager::slotSwitchTrackActive()
{
    pCore->window()->getCurrentTimeline()->controller()->switchTrackActive();
}

void ProjectManager::slotSwitchAllTrackActive()
{
    pCore->window()->getCurrentTimeline()->controller()->switchAllTrackActive();
}

void ProjectManager::slotMakeAllTrackActive()
{
    pCore->window()->getCurrentTimeline()->controller()->makeAllTrackActive();
}

void ProjectManager::slotRestoreTargetTracks()
{
    pCore->window()->getCurrentTimeline()->controller()->restoreTargetTracks();
}

void ProjectManager::slotSwitchAllTrackLock()
{
    pCore->window()->getCurrentTimeline()->controller()->switchTrackLock(true);
}

void ProjectManager::slotSwitchTrackTarget()
{
    pCore->window()->getCurrentTimeline()->controller()->switchTargetTrack();
}

QString ProjectManager::getDefaultProjectFormat()
{
    // On first run, lets use an HD1080p profile with fps related to timezone country. Then, when the first video is added to a project, if it does not match
    // our profile, propose a new default.
    QTimeZone zone;
    zone = QTimeZone::systemTimeZone();

    QList<int> ntscCountries;
    ntscCountries << QLocale::Canada << QLocale::Chile << QLocale::CostaRica << QLocale::Cuba << QLocale::DominicanRepublic << QLocale::Ecuador;
    ntscCountries << QLocale::Japan << QLocale::Mexico << QLocale::Nicaragua << QLocale::Panama << QLocale::Peru << QLocale::Philippines;
    ntscCountries << QLocale::PuertoRico << QLocale::SouthKorea << QLocale::Taiwan << QLocale::UnitedStates;
    bool ntscProject = ntscCountries.contains(zone.country());
    if (!ntscProject) {
        return QStringLiteral("atsc_1080p_25");
    }
    return QStringLiteral("atsc_1080p_2997");
}

void ProjectManager::saveZone(const QStringList &info, const QDir &dir)
{
    pCore->bin()->saveZone(info, dir);
}

void ProjectManager::moveProjectData(const QString &src, const QString &dest)
{
    // Move tmp folder (thumbnails, timeline preview)
    m_project->moveProjectData(src, dest);
    KIO::CopyJob *copyJob = KIO::move(QUrl::fromLocalFile(src), QUrl::fromLocalFile(dest), KIO::DefaultFlags);
    if (copyJob->uiDelegate()) {
        KJobWidgets::setWindow(copyJob, pCore->window());
    }
    connect(copyJob, &KJob::result, this, &ProjectManager::slotMoveFinished);
    connect(copyJob, &KJob::percentChanged, this, &ProjectManager::slotMoveProgress);
}

void ProjectManager::slotMoveProgress(KJob *, unsigned long progress)
{
    pCore->displayMessage(i18n("Moving project folder"), ProcessingJobMessage, static_cast<int>(progress));
}

void ProjectManager::slotMoveFinished(KJob *job)
{
    if (job->error() == 0) {
        pCore->displayMessage(QString(), OperationCompletedMessage, 100);
        auto *copyJob = static_cast<KIO::CopyJob *>(job);
        QString newFolder = copyJob->destUrl().toLocalFile();
        // Check if project folder is inside document folder, in which case, paths will be relative
        QDir projectDir(m_project->url().toString(QUrl::RemoveFilename | QUrl::RemoveScheme));
        QDir srcDir(m_project->projectTempFolder());
        if (srcDir.absolutePath().startsWith(projectDir.absolutePath())) {
            m_replacementPattern.insert(QStringLiteral(">proxy/"), QStringLiteral(">") + newFolder + QStringLiteral("/proxy/"));
        } else {
            m_replacementPattern.insert(m_project->projectTempFolder() + QStringLiteral("/proxy/"), newFolder + QStringLiteral("/proxy/"));
        }
        m_project->setProjectFolder(QUrl::fromLocalFile(newFolder));
        saveFile();
        m_replacementPattern.clear();
        slotRevert();
    } else {
        KMessageBox::error(pCore->window(), i18n("Error moving project folder: %1", job->errorText()));
    }
}

void ProjectManager::requestBackup(const QString &errorMessage)
{
    KMessageBox::ButtonCode res = KMessageBox::warningContinueCancel(qApp->activeWindow(), errorMessage);
    pCore->window()->getCurrentTimeline()->loading = false;
    m_project->setModified(false);
    if (res == KMessageBox::Continue) {
        // Try opening backup
        if (!slotOpenBackup(m_project->url())) {
            newFile(false);
        }
    } else {
        newFile(false);
    }
}

bool ProjectManager::updateTimeline(int pos, const QString &chunks, const QString &dirty, const QDateTime &documentDate, int enablePreview)
{
    pCore->taskManager.slotCancelJobs();

    QScopedPointer<Mlt::Producer> xmlProd(new Mlt::Producer(*pCore->getProjectProfile(), "xml-string", m_project->getAndClearProjectXml().constData()));

    Mlt::Service s(*xmlProd);
    Mlt::Tractor tractor(s);
    if (tractor.count() == 0) {
        // Wow we have a project file with empty tractor, probably corrupted, propose to open a recovery file
        requestBackup(i18n("Project file is corrupted (no tracks). Try to find a backup file?"));
        return false;
    }
    QUuid uuid = m_project->uuid();
    m_mainTimelineModel = TimelineItemModel::construct(uuid, pCore->getProjectProfile(), m_project->commandStack());
    // Add snap point at project start
    m_project->addTimeline(uuid, m_mainTimelineModel);
    m_mainTimelineModel->addSnap(0);
    if (pCore->window()) {
        pCore->window()->getCurrentTimeline()->setModel(m_mainTimelineModel, pCore->monitorManager()->projectMonitor()->getControllerProxy());
    }
    bool projectErrors = false;
    m_project->cleanupTimelinePreview(documentDate);
    if (!constructTimelineFromMelt(m_mainTimelineModel, tractor, m_progressDialog, m_project->modifiedDecimalPoint(), chunks, dirty, enablePreview,
                                   &projectErrors)) {
        // TODO: act on project load failure
        qDebug() << "// Project failed to load!!";
        requestBackup(i18n("Project file is corrupted - failed to load tracks. Try to find a backup file?"));
        return false;
    }
    // Free memory used by original playlist
    xmlProd->clear();
    xmlProd.reset(nullptr);
    const QString groupsData = m_project->getDocumentProperty(QStringLiteral("groups"));
    if (!groupsData.isEmpty()) {
        m_mainTimelineModel->loadGroups(groupsData);
    }
    if (pCore->monitorManager()) {
        emit pCore->monitorManager()->updatePreviewScaling();
        pCore->monitorManager()->projectMonitor()->slotActivateMonitor();
        pCore->monitorManager()->projectMonitor()->setProducer(m_mainTimelineModel->producer(), pos);
        pCore->monitorManager()->projectMonitor()->adjustRulerSize(m_mainTimelineModel->duration() - 1, m_project->getFilteredGuideModel());
    }

    m_mainTimelineModel->setUndoStack(m_project->commandStack());

    // Reset locale to C to ensure numbers are serialised correctly
    LocaleHandling::resetLocale();
    if (projectErrors) {
        m_notesPlugin->showDock();
        m_notesPlugin->widget()->raise();
        m_notesPlugin->widget()->setFocus();
    }
    return true;
}

void ProjectManager::adjustProjectDuration(int duration)
{
    pCore->monitorManager()->projectMonitor()->adjustRulerSize(duration - 1, nullptr);
}

void ProjectManager::activateAsset(const QVariantMap &effectData)
{
    if (effectData.contains(QStringLiteral("kdenlive/effect"))) {
        pCore->window()->addEffect(effectData.value(QStringLiteral("kdenlive/effect")).toString());
    } else {
        pCore->window()->getCurrentTimeline()->controller()->addAsset(effectData);
    }
}

std::shared_ptr<MarkerListModel> ProjectManager::getGuideModel()
{
    return current()->getGuideModel();
}

std::shared_ptr<DocUndoStack> ProjectManager::undoStack()
{
    return current()->commandStack();
}

const QDir ProjectManager::cacheDir(bool audio, bool *ok) const
{
    return m_project->getCacheDir(audio ? CacheAudio : CacheThumbs, ok);
}

void ProjectManager::saveWithUpdatedProfile(const QString &updatedProfile)
{
    // First backup current project with fps appended
    bool saveInTempFile = false;
    if (m_project && m_project->isModified()) {
        switch (KMessageBox::warningTwoActionsCancel(pCore->window(),
                                                     i18n("The project <b>\"%1\"</b> has been changed.\nDo you want to save your changes?",
                                                          m_project->url().fileName().isEmpty() ? i18n("Untitled") : m_project->url().fileName()),
                                                     {}, KStandardGuiItem::save(), KStandardGuiItem::dontSave())) {
        case KMessageBox::PrimaryAction:
            // save document here. If saving fails, return false;
            if (!saveFile()) {
                pCore->displayBinMessage(i18n("Project profile change aborted"), KMessageWidget::Information);
                return;
            }
            break;
        case KMessageBox::Cancel:
            pCore->displayBinMessage(i18n("Project profile change aborted"), KMessageWidget::Information);
            return;
            break;
        default:
            saveInTempFile = true;
            break;
        }
    }

    if (!m_project) {
        pCore->displayBinMessage(i18n("Project profile change aborted"), KMessageWidget::Information);
        return;
    }
    QString currentFile = m_project->url().toLocalFile();

    // Now update to new profile
    auto &newProfile = ProfileRepository::get()->getProfile(updatedProfile);
    QString convertedFile = currentFile.section(QLatin1Char('.'), 0, -2);
    double fpsRatio = newProfile->fps() / pCore->getCurrentFps();
    convertedFile.append(QString("-%1.kdenlive").arg(int(newProfile->fps() * 100)));
    QString saveFolder = m_project->url().adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash).toLocalFile();
    QTemporaryFile tmpFile(saveFolder + "/kdenlive-XXXXXX.mlt");
    if (saveInTempFile) {
        // Save current playlist in tmp file
        if (!tmpFile.open()) {
            // Something went wrong
            pCore->displayBinMessage(i18n("Project profile change aborted"), KMessageWidget::Information);
            return;
        }
        prepareSave();
        QString scene = projectSceneList(saveFolder);
        if (!m_replacementPattern.isEmpty()) {
            QMapIterator<QString, QString> i(m_replacementPattern);
            while (i.hasNext()) {
                i.next();
                scene.replace(i.key(), i.value());
            }
        }
        tmpFile.write(scene.toUtf8());
        if (tmpFile.error() != QFile::NoError) {
            tmpFile.close();
            return;
        }
        tmpFile.close();
        currentFile = tmpFile.fileName();
        // Don't ask again to save
        m_project->setModified(false);
    }

    QDomDocument doc;
    if (!Xml::docContentFromFile(doc, currentFile, false)) {
        KMessageBox::error(qApp->activeWindow(), i18n("Cannot read file %1", currentFile));
        return;
    }

    QDomElement mltProfile = doc.documentElement().firstChildElement(QStringLiteral("profile"));
    if (!mltProfile.isNull()) {
        mltProfile.setAttribute(QStringLiteral("frame_rate_num"), newProfile->frame_rate_num());
        mltProfile.setAttribute(QStringLiteral("frame_rate_den"), newProfile->frame_rate_den());
        mltProfile.setAttribute(QStringLiteral("display_aspect_num"), newProfile->display_aspect_num());
        mltProfile.setAttribute(QStringLiteral("display_aspect_den"), newProfile->display_aspect_den());
        mltProfile.setAttribute(QStringLiteral("sample_aspect_num"), newProfile->sample_aspect_num());
        mltProfile.setAttribute(QStringLiteral("sample_aspect_den"), newProfile->sample_aspect_den());
        mltProfile.setAttribute(QStringLiteral("colorspace"), newProfile->colorspace());
        mltProfile.setAttribute(QStringLiteral("progressive"), newProfile->progressive());
        mltProfile.setAttribute(QStringLiteral("description"), newProfile->description());
        mltProfile.setAttribute(QStringLiteral("width"), newProfile->width());
        mltProfile.setAttribute(QStringLiteral("height"), newProfile->height());
    }
    QDomNodeList playlists = doc.documentElement().elementsByTagName(QStringLiteral("playlist"));
    for (int i = 0; i < playlists.count(); ++i) {
        QDomElement e = playlists.at(i).toElement();
        if (e.attribute(QStringLiteral("id")) == QLatin1String("main_bin")) {
            Xml::setXmlProperty(e, QStringLiteral("kdenlive:docproperties.profile"), updatedProfile);
            // Update guides
            const QString &guidesData = Xml::getXmlProperty(e, QStringLiteral("kdenlive:docproperties.guides"));
            if (!guidesData.isEmpty()) {
                // Update guides position
                auto json = QJsonDocument::fromJson(guidesData.toUtf8());

                QJsonArray updatedList;
                if (json.isArray()) {
                    auto list = json.array();
                    for (const auto &entry : qAsConst(list)) {
                        if (!entry.isObject()) {
                            qDebug() << "Warning : Skipping invalid marker data";
                            continue;
                        }
                        auto entryObj = entry.toObject();
                        if (!entryObj.contains(QLatin1String("pos"))) {
                            qDebug() << "Warning : Skipping invalid marker data (does not contain position)";
                            continue;
                        }
                        int pos = qRound(double(entryObj[QLatin1String("pos")].toInt()) * fpsRatio);
                        QJsonObject currentMarker;
                        currentMarker.insert(QLatin1String("pos"), QJsonValue(pos));
                        currentMarker.insert(QLatin1String("comment"), entryObj[QLatin1String("comment")]);
                        currentMarker.insert(QLatin1String("type"), entryObj[QLatin1String("type")]);
                        updatedList.push_back(currentMarker);
                    }
                    QJsonDocument updatedJSon(updatedList);
                    Xml::setXmlProperty(e, QStringLiteral("kdenlive:docproperties.guides"), QString::fromUtf8(updatedJSon.toJson()));
                }
            }
            break;
        }
    }
    QDomNodeList producers = doc.documentElement().elementsByTagName(QStringLiteral("producer"));
    for (int i = 0; i < producers.count(); ++i) {
        QDomElement e = producers.at(i).toElement();
        bool ok;
        if (Xml::getXmlProperty(e, QStringLiteral("mlt_service")) == QLatin1String("qimage") && Xml::hasXmlProperty(e, QStringLiteral("ttl"))) {
            // Slideshow, duration is frame based, should be calculated again
            Xml::setXmlProperty(e, QStringLiteral("length"), QStringLiteral("0"));
            Xml::removeXmlProperty(e, QStringLiteral("kdenlive:duration"));
            e.setAttribute(QStringLiteral("out"), -1);
            continue;
        }
        int length = Xml::getXmlProperty(e, QStringLiteral("length")).toInt(&ok);
        if (ok && length > 0) {
            // calculate updated length
            Xml::setXmlProperty(e, QStringLiteral("length"), pCore->window()->getCurrentTimeline()->controller()->framesToClock(length));
        }
    }
    if (QFile::exists(convertedFile)) {
        if (KMessageBox::warningTwoActions(qApp->activeWindow(), i18n("Output file %1 already exists.\nDo you want to overwrite it?", convertedFile), {},
                                           KStandardGuiItem::overwrite(), KStandardGuiItem::cancel()) != KMessageBox::PrimaryAction) {
            return;
        }
    }
    QFile file(convertedFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }
    QTextStream out(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    out.setCodec("UTF-8");
#endif
    out << doc.toString();
    if (file.error() != QFile::NoError) {
        KMessageBox::error(qApp->activeWindow(), i18n("Cannot write to file %1", convertedFile));
        file.close();
        return;
    }
    file.close();
    // Copy subtitle file if any
    if (QFile::exists(currentFile + QStringLiteral(".srt"))) {
        QFile(currentFile + QStringLiteral(".srt")).copy(convertedFile + QStringLiteral(".srt"));
    }
    openFile(QUrl::fromLocalFile(convertedFile));
    pCore->displayBinMessage(i18n("Project profile changed"), KMessageWidget::Information);
}

QPair<int, int> ProjectManager::avTracksCount()
{
    return pCore->window()->getCurrentTimeline()->controller()->getAvTracksCount();
}

void ProjectManager::addAudioTracks(int tracksCount)
{
    pCore->window()->getCurrentTimeline()->controller()->addTracks(0, tracksCount);
}
