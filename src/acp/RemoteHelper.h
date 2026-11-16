/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcessEnvironment>
#include <QSet>
#include <QString>

#include <functional>
#include <optional>

#include "../remotefs/RemoteLocation.h"
#include "RemotePathMap.h"

class QCryptographicHash;
class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QSaveFile;
class QTimer;

/**
 * Kate Code's helper (remote-helper/, kate-code-helper) on the host of a
 * remote folder, for a chat whose agent runs there: the host's files as
 * they are now (the mount lags behind) and change notifications.
 *
 * One ssh connection runs KateRemote::remoteHelperLaunchScript(): it execs
 * the copy installed for the pinned version, or asks for an install (once
 * per start(): the pinned archive is downloaded here, checked against its
 * SHA-256, sent to the host's install script, and the helper launched
 * again). Optional: when anything fails, stopped() is emitted and the
 * caller keeps working without it. Paths are local ones (below the host
 * directory) both ways; the helper sees the host's.
 *
 * No KTextEditor here (acpdriver drives it in tests).
 */
class RemoteHelper : public QObject
{
    Q_OBJECT

public:
    // workingDir: the local path of the remote folder; map: the agent's
    // (ACPService::pathMap()).
    RemoteHelper(const QString &workingDir, const KateRemote::RemotePathMap &map, QObject *parent = nullptr);
    ~RemoteHelper() override;

    // The release pinned in Kate Code's source (:/katecode/remote-helper-pin.json,
    // written by remote-helper/pin-release.py); nullopt while none is.
    static std::optional<KateRemote::RemoteHelperPin> builtinPin();

    // Default: builtinPin().
    void setPin(const KateRemote::RemoteHelperPin &pin);
    // Default: ~/.cache/kate-code/helper-downloads.
    void setDownloadDirectory(const QString &directory);
    // file:// downloads (tests); otherwise only https.
    void setAllowFileUrls(bool allow);

    void start();
    // Ends the helper; no stopped() afterwards.
    void stop();
    bool isReady() const
    {
        return m_ready;
    }
    QString host() const;

    // Digests (KTextEditor::Document::checksum()'s: raw SHA-1 of the git
    // blob) of local files; files that cannot be read are missing from
    // the result. Called once; with an empty result when the helper is not
    // ready or fails.
    using DigestCallback = std::function<void(const QHash<QString, QByteArray> &digests)>;
    void digest(const QStringList &localPaths, DigestCallback callback);
    // The files to watch (all of them; sent as watch/unwatch differences,
    // again after a restart): changed() reports them.
    void setWatched(const QStringList &localPaths);

    // A raw request (host paths in params); callback(result, error) once.
    using Callback = std::function<void(const QJsonObject &result, const QJsonObject &error)>;
    void request(const QString &method, const QJsonObject &params, Callback callback);

Q_SIGNALS:
    void ready();
    // The helper stopped for good (failed, or its connection ended more
    // often than it is restarted): the message says why. While it
    // restarts, isReady() is false; ready() follows when it is back.
    void stopped(const QString &message);
    // Watched files that may have changed (local paths).
    void changed(const QStringList &localPaths);
    // Installation steps, for logs.
    void progress(const QString &message);

private:
    void launch();
    void onStdout();
    void onFinished(int exitCode);
    void handleLine(const QByteArray &line);
    void handleMessage(const QJsonObject &message);
    void becomeReady(const QJsonObject &hello);
    void install(const QString &platform);
    void download(const KateRemote::RemoteHelperBuild &build, std::function<void(const QString &file)> done);
    void runInstall(const KateRemote::RemoteHelperBuild &build, const QString &archive);
    void failAllPending(const QString &message);
    void finish(const QString &message);
    void syncWatches();
    QString toHost(const QString &localPath) const;
    QString toLocal(const QString &hostPath) const;
    // ssh's environment (askpass, like the agent's ssh); stateFile: the
    // askpass state file to remove afterwards (empty if none).
    QProcessEnvironment sshEnvironment(QString *stateFile) const;

    QString m_workingDir;
    KateRemote::RemotePathMap m_map;
    std::optional<KateRemote::Location> m_location;
    bool m_windows = false;
    std::optional<KateRemote::RemoteHelperPin> m_pin;
    QString m_downloadDir;
    bool m_allowFileUrls = false;

    QPointer<QProcess> m_process;
    QString m_askpassState;
    QByteArray m_buffer;
    QByteArray m_stderrTail;
    qint64 m_preHelloBytes = 0;
    bool m_helloSeen = false;
    bool m_ready = false;
    bool m_stopped = true;
    QString m_installPlatform; // the launch script asked for an install
    QString m_unsupported; // ... or said the host is not supported
    bool m_installTried = false;
    int m_restarts = 0;
    QTimer *m_helloTimer = nullptr;

    int m_nextId = 1;
    QHash<int, Callback> m_pending;
    QSet<QString> m_watched; // local paths wanted
    QSet<QString> m_watchedOnHelper; // local paths the running helper watches
    QHash<QString, QString> m_hostToLocal; // for changed()

    // Installing.
    QPointer<QProcess> m_installProcess;
    QString m_installAskpassState;
    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    QSaveFile *m_downloadFile = nullptr;
    QCryptographicHash *m_hash = nullptr;
};
