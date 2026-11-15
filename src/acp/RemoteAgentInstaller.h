/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

#include "../remotefs/RemoteLocation.h"

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QSaveFile;
class QCryptographicHash;

/**
 * Installs an agent's ACP registry binary on the host of a remote folder,
 * for agents that run there (see ACPService): asks the host for its
 * platform, downloads that build here (checked against the registry's
 * SHA-256, kept in a cache), re-packs it as .tar.gz unless it is one, and
 * streams it to the host's install script (KateRemote::
 * remoteAgentInstallScript()) over ssh. The host needs only tar, not
 * network access.
 *
 * Started by the user (the chat offers it when the agent is not found on
 * the host); finished() reports the outcome once.
 */
class RemoteAgentInstaller : public QObject
{
    Q_OBJECT

public:
    // agent: the registry entry; workingDir: the local path of the remote
    // folder; program: what the provider runs.
    RemoteAgentInstaller(const QJsonObject &agent, const QString &workingDir, const QString &program, QObject *parent = nullptr);
    ~RemoteAgentInstaller() override;

    // Whether agent has a binary build whose program is 'program' (its file
    // name; ".exe" aside): only then does a launcher named after the
    // provider's executable run the right thing.
    static bool canInstall(const QJsonObject &agent, const QString &program);

    // Default: ~/.cache/kate-code/agent-downloads.
    void setDownloadDirectory(const QString &directory);
    // file:// downloads (tests); otherwise only https.
    void setAllowFileUrls(bool allow);

    void start();
    // Stops whatever runs; finished() is not emitted.
    void cancel();

    QString agentName() const;
    QString host() const;

Q_SIGNALS:
    void progress(const QString &message);
    void finished(bool ok, const QString &message);

private:
    void probePlatform();
    void download(const QJsonObject &build);
    void prepareArchive();
    void install();
    void fail(const QString &message);
    void succeed(const QString &message);
    // ssh running script on the host; done(exit code, stdout, stderr).
    void runScript(const QString &script, const QString &stdinFile, std::function<void(int, const QByteArray &, const QByteArray &)> done);
    void removeAskpassState();

    QJsonObject m_agent;
    QString m_workingDir;
    QString m_program;
    QString m_downloadDir;
    std::optional<KateRemote::Location> m_location;
    bool m_windows = false;
    KateRemote::RemoteAgentPackage m_package;
    QString m_archive; // the verified download
    QString m_sendFile; // what is sent (the download, or its .tar.gz)
    QString m_expectedSha256;
    bool m_done = false;
    bool m_allowFileUrls = false;

    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    QSaveFile *m_downloadFile = nullptr;
    QCryptographicHash *m_hash = nullptr;
    QPointer<QProcess> m_process;
    QString m_askpassState;
    QString m_tempArchive; // re-packed .tar.gz, removed at the end
    int m_lastPercent = -1;
};
