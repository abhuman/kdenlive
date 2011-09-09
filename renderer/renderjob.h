/***************************************************************************
 *   Copyright (C) 2007 by Jean-Baptiste Mardelle (jb@kdenlive.org)        *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA          *
 ***************************************************************************/


#ifndef RENDERJOB_H
#define RENDERJOB_H

#include <QProcess>
#include <QObject>
#include <QDBusInterface>
#include <QTime>
// Testing
#include <QTemporaryFile>
#include <QTextStream>

class RenderJob : public QObject
{
    Q_OBJECT

public:
    RenderJob(bool erase, bool usekuiserver, const QString& renderer, const QString& profile, const QString& rendermodule, const QString& player, const QString& scenelist, const QString& dest, const QStringList& preargs, const QStringList& args, int in = -1, int out = -1);
    ~RenderJob();
    void setLocale(const QString &locale);

public slots:
    void start();

private slots:
    void slotIsOver(QProcess::ExitStatus status, bool isWritable = true);
    void receivedStderr();
    void slotAbort();
    void slotAbort(const QString& url);
    void slotCheckProcess(QProcess::ProcessState state);

private:
    QString m_scenelist;
    QString m_dest;
    int m_progress;
    QProcess* m_renderProcess;
    QString m_errorMessage;
    QString m_prog;
    QString m_player;
    QStringList m_args;
    bool m_erase;
    bool m_dualpass;
    QDBusInterface* m_jobUiserver;
    QDBusInterface* m_kdenliveinterface;
    QList<QVariant> m_dbusargs;
    QTime m_startTime;
    void initKdenliveDbusInterface();
    bool m_usekuiserver;
    bool m_enablelog;
    /** @brief Used to create a temporary file for logging. */
    QTemporaryFile m_logfile;
    /** @brief Used to write to the log file. */
    QTextStream m_logstream;

signals:
    void renderingFinished();
};

#endif
