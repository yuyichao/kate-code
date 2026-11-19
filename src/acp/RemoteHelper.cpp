/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#include "RemoteHelper.h"

#include "ACPService.h"
#include "RemoteStdio.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

namespace
{
// Before the helper's hello: shell startup files may print over ssh, a
// password may be asked; but not forever, and not megabytes.
constexpr int HelloTimeoutMs = 60000;
constexpr qint64 MaxPreHelloBytes = 64 * 1024;
// A helper that was running is restarted after its connection ends, a few
// times (a dropped network), with growing delays.
constexpr int MaxRestarts = 3;

QString sha256Of(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    return QString::fromLatin1(hash.result().toHex());
}

QString lastLines(const QByteArray &text)
{
    const QStringList lines = QString::fromLocal8Bit(text).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    return lines.mid(std::max<qsizetype>(0, lines.size() - 3)).join(QLatin1Char('\n')).trimmed();
}

QJsonObject errorObject(const QString &message)
{
    return QJsonObject{{QStringLiteral("code"), 1}, {QStringLiteral("message"), message}};
}
}

RemoteHelper::RemoteHelper(const QString &workingDir, const KateRemote::RemotePathMap &map, QObject *parent)
    : QObject(parent)
    , m_workingDir(workingDir)
    , m_map(map)
    , m_downloadDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/kate-code/helper-downloads"))
{
    m_location = KateRemote::locationForPath(workingDir, KateRemote::configuredMountRoot());
    if (m_location) {
        m_windows = !KateRemote::windowsPathForSftpPath(m_location->path).isEmpty();
    }
}

RemoteHelper::~RemoteHelper()
{
    stop();
}

std::optional<KateRemote::RemoteHelperPin> RemoteHelper::builtinPin()
{
    QFile file(QStringLiteral(":/katecode/remote-helper-pin.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        // Not the placeholder: a build without the resource.
        qWarning() << "[RemoteHelper] the pin resource is missing:" << file.fileName();
        return std::nullopt;
    }
    return KateRemote::parseRemoteHelperPin(file.readAll());
}

void RemoteHelper::setPin(const KateRemote::RemoteHelperPin &pin)
{
    m_pin = pin;
}

void RemoteHelper::setDownloadDirectory(const QString &directory)
{
    m_downloadDir = directory;
}

void RemoteHelper::setAllowFileUrls(bool allow)
{
    m_allowFileUrls = allow;
}

QString RemoteHelper::host() const
{
    return m_location ? m_location->authority : QString();
}

RemoteStdio *RemoteHelper::newSshProcess()
{
    auto *process = new RemoteStdio(this);
    // Like the agent's ssh (ACPService::start()): passwords from KDE's
    // cache, else a dialog.
    process->setAskpass(ACPService::remoteFsAskpassProgram(), m_location->authority);
    if (KateRemote::usesTunnel(*m_location)) {
        process->setTunnel(*m_location, KateRemote::configuredSshCommand());
    }
    process->setWorkingDirectory(QDir::homePath());
    return process;
}

void RemoteHelper::start()
{
    m_stopped = false;
    m_installTried = false;
    m_restarts = 0;
    if (!m_location || !KateRemote::isSshScheme(m_location->scheme)) {
        finish(QStringLiteral("%1 is not a folder on an ssh host").arg(m_workingDir));
        return;
    }
    if (!m_pin) {
        m_pin = builtinPin();
    }
    if (!m_pin) {
        finish(QStringLiteral("no kate-code-helper release is pinned in this Kate Code"));
        return;
    }
    launch();
}

void RemoteHelper::launch()
{
    const QStringList command = KateRemote::sshHelperCommand(*m_location, KateRemote::configuredSshCommand(), *m_pin);
    if (command.isEmpty()) {
        finish(QStringLiteral("cannot run commands on %1 through ssh").arg(m_location->authority));
        return;
    }
    m_buffer.clear();
    m_stderrTail.clear();
    m_preHelloBytes = 0;
    m_helloSeen = false;
    m_ready = false;
    m_installPlatform.clear();
    m_unsupported.clear();
    m_watchedOnHelper.clear();
    m_hostToLocal.clear();

    RemoteStdio *process = newSshProcess();
    m_process = process;
    connect(process, &RemoteStdio::readyReadStandardOutput, this, &RemoteHelper::onStdout);
    connect(process, &RemoteStdio::readyReadStandardError, this, [this, process] {
        m_stderrTail += process->readAllStandardError();
        if (m_stderrTail.size() > 4096) {
            m_stderrTail = m_stderrTail.right(4096);
        }
    });
    connect(process, &RemoteStdio::finished, this, [this, process](int exitCode, QProcess::ExitStatus status) {
        if (process != m_process) {
            return;
        }
        onFinished(status == QProcess::NormalExit ? exitCode : -1);
    });
    connect(process, &RemoteStdio::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (process == m_process && error == QProcess::FailedToStart) {
            m_process = nullptr;
            process->deleteLater();
            finish(QStringLiteral("cannot start ssh: %1").arg(process->errorString()));
        }
    });
    if (!m_helloTimer) {
        m_helloTimer = new QTimer(this);
        m_helloTimer->setSingleShot(true);
        connect(m_helloTimer, &QTimer::timeout, this, [this] {
            if (m_process && !m_helloSeen) {
                qWarning() << "[RemoteHelper]" << host() << ": no hello from kate-code-helper";
                m_process->kill(); // onFinished() reports it
            }
        });
    }
    m_helloTimer->start(HelloTimeoutMs);
    process->start(command.constFirst(), command.mid(1));
}

void RemoteHelper::stop()
{
    m_stopped = true;
    m_ready = false;
    if (m_helloTimer) {
        m_helloTimer->stop();
    }
    for (QPointer<RemoteStdio> *p : {&m_process, &m_installProcess}) {
        if (RemoteStdio *process = *p) {
            process->disconnect(this);
            process->kill();
            process->waitForFinished(1000);
            process->deleteLater();
            *p = nullptr;
        }
    }
    if (m_reply) {
        m_reply->abort();
    }
    delete m_downloadFile; // QSaveFile: discarded without commit()
    m_downloadFile = nullptr;
    delete m_hash;
    m_hash = nullptr;
    failAllPending(QStringLiteral("kate-code-helper stopped"));
}

void RemoteHelper::finish(const QString &message)
{
    if (m_stopped) {
        return;
    }
    qDebug() << "[RemoteHelper]" << host() << ":" << message;
    stop();
    Q_EMIT stopped(message);
}

void RemoteHelper::failAllPending(const QString &message)
{
    const QHash<int, Callback> pending = std::exchange(m_pending, {});
    for (const Callback &callback : pending) {
        callback(QJsonObject(), errorObject(message));
    }
}

void RemoteHelper::onStdout()
{
    if (!m_process) {
        return;
    }
    m_buffer += m_process->readAllStandardOutput();
    qsizetype newline;
    while ((newline = m_buffer.indexOf('\n')) >= 0) {
        QByteArray line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        handleLine(line);
        if (!m_process) {
            return; // finished meanwhile
        }
    }
    if (!m_helloSeen && m_preHelloBytes + m_buffer.size() > MaxPreHelloBytes) {
        qWarning() << "[RemoteHelper]" << host() << ": too much output before kate-code-helper's hello";
        m_process->kill();
    }
}

void RemoteHelper::handleLine(const QByteArray &line)
{
    if (!m_helloSeen) {
        m_preHelloBytes += line.size() + 1;
        // The launch script's verdicts (anything else is a login shell's
        // noise).
        if (line.startsWith("kate-code-helper: install ")) {
            m_installPlatform = QString::fromUtf8(line.mid(26)).trimmed();
            return;
        }
        if (line.startsWith("kate-code-helper: unsupported ")) {
            m_unsupported = QString::fromUtf8(line.mid(30)).trimmed();
            return;
        }
        if (!line.startsWith('{')) {
            return;
        }
    }
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return;
    }
    handleMessage(doc.object());
}

void RemoteHelper::handleMessage(const QJsonObject &message)
{
    const QString method = message.value(QStringLiteral("method")).toString();
    if (!m_helloSeen) {
        if (method == QLatin1String("hello")) {
            m_helloSeen = true;
            m_helloTimer->stop();
            becomeReady(message.value(QStringLiteral("params")).toObject());
        }
        return;
    }
    if (method == QLatin1String("changed")) {
        QStringList paths;
        const QJsonArray hostPaths = message.value(QStringLiteral("params")).toObject().value(QStringLiteral("paths")).toArray();
        for (const QJsonValue &value : hostPaths) {
            const QString local = toLocal(value.toString());
            if (m_watched.contains(local)) {
                paths << local;
            }
        }
        if (!paths.isEmpty()) {
            Q_EMIT changed(paths);
        }
        return;
    }
    if (!method.isEmpty() || !message.contains(QStringLiteral("id"))) {
        return; // notifications this client does not know
    }
    const QJsonValue id = message.value(QStringLiteral("id"));
    const QJsonObject error = message.value(QStringLiteral("error")).toObject();
    if (id.isNull()) {
        // A line the helper could not take: whichever request it was, it
        // gets no answer.
        failAllPending(error.value(QStringLiteral("message")).toString(QStringLiteral("kate-code-helper refused a request")));
        return;
    }
    const Callback callback = m_pending.take(id.toInt());
    if (callback) {
        callback(message.value(QStringLiteral("result")).toObject(), error);
    }
}

void RemoteHelper::becomeReady(const QJsonObject &hello)
{
    const int protocol = hello.value(QStringLiteral("protocol")).toInt();
    const QString version = hello.value(QStringLiteral("version")).toString();
    QSet<QString> capabilities;
    for (const QJsonValue &value : hello.value(QStringLiteral("capabilities")).toArray()) {
        capabilities.insert(value.toString());
    }
    // The launch script checked the program's SHA-256 when the host has a
    // tool for it; the version is checked here in any case.
    if (protocol != m_pin->protocol || version != m_pin->version) {
        finish(QStringLiteral("the helper on %1 is %2 (protocol %3), not the pinned %4 (protocol %5)")
                   .arg(host(), version)
                   .arg(protocol)
                   .arg(m_pin->version)
                   .arg(m_pin->protocol));
        return;
    }
    if (!capabilities.contains(QStringLiteral("digest")) || !capabilities.contains(QStringLiteral("watch"))) {
        finish(QStringLiteral("the helper on %1 cannot compute digests or watch files").arg(host()));
        return;
    }
    qDebug() << "[RemoteHelper]" << host() << ": kate-code-helper" << version << hello.value(QStringLiteral("platform")).toString() << "ready";
    m_ready = true;
    syncWatches();
    Q_EMIT ready();
}

void RemoteHelper::onFinished(int exitCode)
{
    RemoteStdio *process = m_process;
    m_process = nullptr;
    if (process) {
        process->deleteLater();
    }
    m_helloTimer->stop();
    const bool wasReady = m_ready;
    m_ready = false;
    failAllPending(QStringLiteral("kate-code-helper on %1 ended").arg(host()));
    if (m_stopped) {
        return;
    }
    const QString details = lastLines(m_stderrTail);
    const QString suffix = details.isEmpty() ? QString() : QStringLiteral(": ") + details;
    if (!m_helloSeen && exitCode == KateRemote::RemoteHelperInstallNeeded && !m_installPlatform.isEmpty()) {
        if (m_installTried) {
            finish(QStringLiteral("the helper installed on %1 does not match the pinned one").arg(host()));
        } else {
            install(m_installPlatform);
        }
        return;
    }
    if (!m_helloSeen && exitCode == KateRemote::RemoteHelperUnsupported) {
        finish(QStringLiteral("no kate-code-helper build for %1 (%2)").arg(host(), m_unsupported));
        return;
    }
    if (wasReady && m_restarts < MaxRestarts) {
        // The connection ended (dropped network, host restart): again later.
        const int delay = 5000 << m_restarts;
        ++m_restarts;
        qDebug() << "[RemoteHelper]" << host() << ": kate-code-helper ended (exit code" << exitCode << "), restarting in" << delay / 1000 << "s";
        QTimer::singleShot(delay, this, [this] {
            if (!m_stopped && !m_process && !m_installProcess) {
                launch();
            }
        });
        return;
    }
    finish(exitCode == 255 ? QStringLiteral("ssh to %1 failed%2").arg(host(), suffix)
                           : QStringLiteral("kate-code-helper on %1 ended (exit code %2)%3").arg(host()).arg(exitCode).arg(suffix));
}

void RemoteHelper::install(const QString &platform)
{
    m_installTried = true;
    const KateRemote::RemoteHelperBuild *build = m_pin->build(platform);
    if (!build) {
        finish(QStringLiteral("no kate-code-helper build for %1 (%2)").arg(host(), platform));
        return;
    }
    const KateRemote::RemoteHelperBuild b = *build;
    download(b, [this, b](const QString &file) {
        runInstall(b, file);
    });
}

void RemoteHelper::download(const KateRemote::RemoteHelperBuild &build, std::function<void(const QString &file)> done)
{
    const QUrl url = m_pin->archiveUrl(build);
    if (!url.isValid() || (url.scheme() != QLatin1String("https") && !(m_allowFileUrls && url.scheme() == QLatin1String("file")))) {
        finish(QStringLiteral("the pinned download %1 is not an https URL").arg(url.toDisplayString()));
        return;
    }
    // Per hash: a cached file is the verified one.
    const QString dir = m_downloadDir + QLatin1Char('/') + build.sha256;
    const QString path = dir + QLatin1Char('/') + build.archive;
    if (QFileInfo::exists(path) && sha256Of(path) == build.sha256) {
        done(path);
        return;
    }
    QDir().mkpath(dir);
    m_downloadFile = new QSaveFile(path);
    if (!m_downloadFile->open(QIODevice::WriteOnly)) {
        finish(QStringLiteral("cannot write %1: %2").arg(path, m_downloadFile->errorString()));
        return;
    }
    m_hash = new QCryptographicHash(QCryptographicHash::Sha256);
    if (!m_network) {
        m_network = new QNetworkAccessManager(this);
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    Q_EMIT progress(QStringLiteral("Downloading kate-code-helper %1 (%2)").arg(m_pin->version, url.toDisplayString()));
    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        const QByteArray data = m_reply->readAll();
        m_hash->addData(data);
        if (m_downloadFile->write(data) != data.size()) {
            finish(QStringLiteral("cannot write the download: %1").arg(m_downloadFile->errorString()));
        }
    });
    connect(m_reply, &QNetworkReply::finished, this, [this, build, path, done] {
        QNetworkReply *reply = m_reply;
        reply->deleteLater();
        if (m_stopped || !m_downloadFile) {
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            finish(QStringLiteral("downloading kate-code-helper failed: %1").arg(reply->errorString()));
            return;
        }
        const QByteArray rest = reply->readAll();
        m_hash->addData(rest);
        m_downloadFile->write(rest);
        const QString sha256 = QString::fromLatin1(m_hash->result().toHex());
        delete m_hash;
        m_hash = nullptr;
        if (sha256 != build.sha256) {
            delete m_downloadFile;
            m_downloadFile = nullptr;
            finish(QStringLiteral("the kate-code-helper download's SHA-256 is %1, not the pinned %2: not installed").arg(sha256, build.sha256));
            return;
        }
        const bool committed = m_downloadFile->commit();
        const QString error = m_downloadFile->errorString();
        delete m_downloadFile;
        m_downloadFile = nullptr;
        if (!committed) {
            finish(QStringLiteral("cannot write %1: %2").arg(path, error));
            return;
        }
        done(path);
    });
}

void RemoteHelper::runInstall(const KateRemote::RemoteHelperBuild &build, const QString &archive)
{
    const QStringList command =
        KateRemote::sshScriptCommand(*m_location, KateRemote::configuredSshCommand(), KateRemote::remoteHelperInstallScript(m_windows, m_pin->version, build));
    if (command.isEmpty()) {
        finish(QStringLiteral("cannot install kate-code-helper %1 on %2").arg(m_pin->version, host()));
        return;
    }
    Q_EMIT progress(QStringLiteral("Installing kate-code-helper %1 (%2) on %3").arg(m_pin->version, build.platform, host()));
    RemoteStdio *process = newSshProcess();
    m_installProcess = process;
    process->setStandardInputFile(archive);
    connect(process, &RemoteStdio::finished, this, [this, process](int exitCode, QProcess::ExitStatus status) {
        const QByteArray out = process->readAllStandardOutput();
        const QByteArray err = process->readAllStandardError();
        process->deleteLater();
        m_installProcess = nullptr;
        if (status != QProcess::NormalExit || exitCode != 0 || !out.contains("installed")) {
            const QString details = lastLines(err);
            finish(QStringLiteral("installing kate-code-helper on %1 failed (exit code %2)%3")
                       .arg(host())
                       .arg(exitCode)
                       .arg(details.isEmpty() ? QString() : QStringLiteral(": ") + details));
            return;
        }
        Q_EMIT progress(QStringLiteral("Installed kate-code-helper %1 on %2").arg(m_pin->version, host()));
        launch();
    });
    connect(process, &RemoteStdio::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finish(QStringLiteral("cannot start ssh: %1").arg(process->errorString()));
        }
    });
    process->start(command.constFirst(), command.mid(1));
}

QString RemoteHelper::toHost(const QString &localPath) const
{
    return m_map.toRemote(QDir::cleanPath(localPath), true);
}

QString RemoteHelper::toLocal(const QString &hostPath) const
{
    const QString local = m_hostToLocal.value(hostPath);
    return local.isEmpty() ? m_map.toLocal(hostPath) : local;
}

void RemoteHelper::request(const QString &method, const QJsonObject &params, Callback callback)
{
    if (!m_ready || !m_process) {
        if (callback) {
            callback(QJsonObject(), errorObject(QStringLiteral("kate-code-helper is not running")));
        }
        return;
    }
    const int id = m_nextId++;
    if (callback) {
        m_pending.insert(id, std::move(callback));
    }
    const QJsonObject message{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                              {QStringLiteral("id"), id},
                              {QStringLiteral("method"), method},
                              {QStringLiteral("params"), params}};
    m_process->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void RemoteHelper::digest(const QStringList &localPaths, DigestCallback callback)
{
    if (localPaths.isEmpty() || !m_ready) {
        callback({});
        return;
    }
    QJsonArray paths;
    QHash<QString, QString> local;
    for (const QString &path : localPaths) {
        const QString hostPath = toHost(path);
        local.insert(hostPath, path);
        paths.append(hostPath);
    }
    request(QStringLiteral("digest"), QJsonObject{{QStringLiteral("paths"), paths}}, [callback, local](const QJsonObject &result, const QJsonObject &) {
        QHash<QString, QByteArray> digests;
        for (const QJsonValue &value : result.value(QStringLiteral("digests")).toArray()) {
            const QJsonObject entry = value.toObject();
            const QString path = local.value(entry.value(QStringLiteral("path")).toString());
            const QByteArray digest = QByteArray::fromHex(entry.value(QStringLiteral("digest")).toString().toLatin1());
            if (!path.isEmpty() && digest.size() == 20) {
                digests.insert(path, digest);
            }
        }
        callback(digests);
    });
}

void RemoteHelper::setWatched(const QStringList &localPaths)
{
    m_watched.clear();
    for (const QString &path : localPaths) {
        m_watched.insert(QDir::cleanPath(path));
    }
    syncWatches();
}

void RemoteHelper::syncWatches()
{
    if (!m_ready) {
        return;
    }
    QJsonArray add;
    QJsonArray remove;
    for (const QString &path : std::as_const(m_watched)) {
        if (!m_watchedOnHelper.contains(path)) {
            const QString hostPath = toHost(path);
            m_hostToLocal.insert(hostPath, path);
            add.append(hostPath);
        }
    }
    for (const QString &path : std::as_const(m_watchedOnHelper)) {
        if (!m_watched.contains(path)) {
            remove.append(toHost(path));
        }
    }
    m_watchedOnHelper = m_watched;
    if (!remove.isEmpty()) {
        request(QStringLiteral("unwatch"), QJsonObject{{QStringLiteral("paths"), remove}}, nullptr);
    }
    if (!add.isEmpty()) {
        request(QStringLiteral("watch"), QJsonObject{{QStringLiteral("paths"), add}}, [this](const QJsonObject &result, const QJsonObject &) {
            for (const QJsonValue &value : result.value(QStringLiteral("failed")).toArray()) {
                qDebug() << "[RemoteHelper]" << host() << ": cannot watch" << value.toObject().value(QStringLiteral("path")).toString()
                         << value.toObject().value(QStringLiteral("error")).toString();
            }
        });
    }
}
