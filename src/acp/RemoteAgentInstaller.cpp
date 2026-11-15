/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#include "RemoteAgentInstaller.h"

#include "ACPService.h"

#include <KTar>
#include <KZip>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QThread>
#include <QUuid>

#include <memory>

namespace
{
// The registry's program path ("./a/b", "./a\\b") with '/'.
QString cmdPath(const QString &cmd)
{
    QString path = cmd;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return path.startsWith(QLatin1String("./")) ? path.mid(2) : path;
}

// A program name without ".exe" (Windows builds), for comparing with the
// provider's executable.
QString programName(const QString &path)
{
    QString name = QFileInfo(path).fileName();
    if (name.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)) {
        name.chop(4);
    }
    return name;
}

bool isTarGz(const QString &name)
{
    return name.endsWith(QLatin1String(".tar.gz")) || name.endsWith(QLatin1String(".tgz"));
}

bool isArchive(const QString &name)
{
    static const QStringList suffixes{QStringLiteral(".tar.gz"),
                                      QStringLiteral(".tgz"),
                                      QStringLiteral(".tar.bz2"),
                                      QStringLiteral(".tbz2"),
                                      QStringLiteral(".tar.xz"),
                                      QStringLiteral(".txz"),
                                      QStringLiteral(".tar.zst"),
                                      QStringLiteral(".tar"),
                                      QStringLiteral(".zip")};
    return std::any_of(suffixes.begin(), suffixes.end(), [&](const QString &suffix) {
        return name.endsWith(suffix);
    });
}

bool copyEntries(const KArchiveDirectory *dir, const QString &prefix, KArchive &out, QString *error)
{
    for (const QString &name : dir->entries()) {
        const KArchiveEntry *entry = dir->entry(name);
        const QString path = prefix.isEmpty() ? name : prefix + QLatin1Char('/') + name;
        if (!entry->symLinkTarget().isEmpty()) {
            if (!out.writeSymLink(path, entry->symLinkTarget(), entry->user(), entry->group(), entry->permissions())) {
                *error = QStringLiteral("cannot write %1: %2").arg(path, out.errorString());
                return false;
            }
        } else if (entry->isDirectory()) {
            if (!out.writeDir(path, entry->user(), entry->group(), entry->permissions() ? entry->permissions() : 0755)) {
                *error = QStringLiteral("cannot write %1: %2").arg(path, out.errorString());
                return false;
            }
            if (!copyEntries(static_cast<const KArchiveDirectory *>(entry), path, out, error)) {
                return false;
            }
        } else if (entry->isFile()) {
            const auto *file = static_cast<const KArchiveFile *>(entry);
            std::unique_ptr<QIODevice> in(file->createDevice());
            if (!in || !out.prepareWriting(path, entry->user(), entry->group(), file->size(), entry->permissions() ? entry->permissions() : 0644)) {
                *error = QStringLiteral("cannot write %1: %2").arg(path, out.errorString());
                return false;
            }
            QByteArray buffer;
            while (!in->atEnd()) {
                buffer = in->read(1 << 20);
                if (buffer.isEmpty() || !out.writeData(buffer.constData(), buffer.size())) {
                    *error = QStringLiteral("cannot write %1: %2").arg(path, out.errorString());
                    return false;
                }
            }
            if (!out.finishWriting(file->size())) {
                *error = QStringLiteral("cannot write %1: %2").arg(path, out.errorString());
                return false;
            }
        }
    }
    return true;
}

// The download as a .tar.gz at target: archives re-packed, a bare program
// put in as cmd (executable). Runs in a worker thread.
QString repack(const QString &source, const QString &cmd, const QString &target)
{
    KTar out(target, QStringLiteral("application/x-gzip"));
    if (!out.open(QIODevice::WriteOnly)) {
        return QStringLiteral("cannot create %1: %2").arg(target, out.errorString());
    }
    QString error;
    const QString name = QFileInfo(source).fileName();
    if (!isArchive(name)) {
        QFile in(source);
        if (!in.open(QIODevice::ReadOnly)) {
            return QStringLiteral("cannot read %1").arg(source);
        }
        const QString path = cmdPath(cmd);
        if (!out.prepareWriting(path, QString(), QString(), in.size(), 0755)) {
            return out.errorString();
        }
        while (!in.atEnd()) {
            const QByteArray buffer = in.read(1 << 20);
            if (buffer.isEmpty() || !out.writeData(buffer.constData(), buffer.size())) {
                return out.errorString();
            }
        }
        if (!out.finishWriting(in.size())) {
            return out.errorString();
        }
    } else {
        std::unique_ptr<KArchive> in;
        if (name.endsWith(QLatin1String(".zip"))) {
            in = std::make_unique<KZip>(source);
        } else {
            in = std::make_unique<KTar>(source);
        }
        if (!in->open(QIODevice::ReadOnly)) {
            return QStringLiteral("cannot read the archive: %1").arg(in->errorString());
        }
        if (!copyEntries(in->directory(), QString(), out, &error)) {
            return error;
        }
    }
    if (!out.close()) {
        return out.errorString();
    }
    return {};
}

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
}

RemoteAgentInstaller::RemoteAgentInstaller(const QJsonObject &agent, const QString &workingDir, const QString &program, QObject *parent)
    : QObject(parent)
    , m_agent(agent)
    , m_workingDir(workingDir)
    , m_program(program)
    , m_downloadDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/kate-code/agent-downloads"))
{
    m_location = KateRemote::locationForPath(workingDir, KateRemote::configuredMountRoot());
    if (m_location) {
        m_windows = !KateRemote::windowsPathForSftpPath(m_location->path).isEmpty();
    }
}

RemoteAgentInstaller::~RemoteAgentInstaller()
{
    cancel();
}

bool RemoteAgentInstaller::canInstall(const QJsonObject &agent, const QString &program)
{
    const QJsonObject builds = agent.value(QStringLiteral("distribution")).toObject().value(QStringLiteral("binary")).toObject();
    const QString name = programName(program);
    if (name.isEmpty()) {
        return false;
    }
    for (auto it = builds.begin(); it != builds.end(); ++it) {
        const QString cmd = it.value().toObject().value(QStringLiteral("cmd")).toString();
        if (programName(cmdPath(cmd)).compare(name, it.key().startsWith(QLatin1String("windows")) ? Qt::CaseInsensitive : Qt::CaseSensitive) == 0) {
            return true;
        }
    }
    return false;
}

void RemoteAgentInstaller::setDownloadDirectory(const QString &directory)
{
    m_downloadDir = directory;
}

void RemoteAgentInstaller::setAllowFileUrls(bool allow)
{
    m_allowFileUrls = allow;
}

QString RemoteAgentInstaller::agentName() const
{
    return m_agent.value(QStringLiteral("name")).toString(m_agent.value(QStringLiteral("id")).toString());
}

QString RemoteAgentInstaller::host() const
{
    return m_location ? m_location->authority : QString();
}

void RemoteAgentInstaller::start()
{
    if (!m_location || !KateRemote::isSshScheme(m_location->scheme)) {
        fail(QStringLiteral("%1 is not a folder on an ssh host.").arg(m_workingDir));
        return;
    }
    probePlatform();
}

void RemoteAgentInstaller::cancel()
{
    m_done = true;
    if (m_reply) {
        m_reply->abort();
    }
    if (m_process) {
        // May be called from the process's own signal handlers.
        m_process->disconnect(this);
        m_process->kill();
        m_process->deleteLater();
        m_process = nullptr;
    }
    delete m_downloadFile; // QSaveFile: discarded without commit()
    m_downloadFile = nullptr;
    delete m_hash;
    m_hash = nullptr;
    removeAskpassState();
    if (!m_tempArchive.isEmpty()) {
        QFile::remove(m_tempArchive);
        m_tempArchive.clear();
    }
}

void RemoteAgentInstaller::fail(const QString &message)
{
    if (m_done) {
        return;
    }
    cancel();
    Q_EMIT finished(false, message);
}

void RemoteAgentInstaller::succeed(const QString &message)
{
    if (m_done) {
        return;
    }
    cancel();
    Q_EMIT finished(true, message);
}

void RemoteAgentInstaller::removeAskpassState()
{
    if (!m_askpassState.isEmpty()) {
        QFile::remove(m_askpassState);
        m_askpassState.clear();
    }
}

void RemoteAgentInstaller::runScript(const QString &script, const QString &stdinFile, std::function<void(int, const QByteArray &, const QByteArray &)> done)
{
    const QStringList command = KateRemote::sshScriptCommand(*m_location, KateRemote::configuredSshCommand(), script);
    if (command.isEmpty()) {
        fail(QStringLiteral("Cannot run commands on %1 through ssh.").arg(m_location->authority));
        return;
    }
    removeAskpassState();
    // Like the agent's ssh (ACPService::start()): passwords from KDE's
    // cache, else a dialog.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString state = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/kate-code-askpass-")
        + QUuid::createUuid().toString(QUuid::Id128);
    if (KateRemote::setSshAskpassEnvironment(env, ACPService::remoteFsAskpassProgram(), m_location->authority, state)) {
        m_askpassState = state;
    }
    auto *process = new QProcess(this);
    m_process = process;
    process->setProcessEnvironment(env);
    process->setWorkingDirectory(QDir::homePath());
    if (!stdinFile.isEmpty()) {
        process->setStandardInputFile(stdinFile);
    } else {
        process->setStandardInputFile(QProcess::nullDevice());
    }
    connect(process, &QProcess::finished, this, [this, process, done](int exitCode, QProcess::ExitStatus status) {
        const QByteArray out = process->readAllStandardOutput();
        const QByteArray err = process->readAllStandardError();
        process->deleteLater();
        m_process = nullptr;
        removeAskpassState();
        done(status == QProcess::NormalExit ? exitCode : -1, out, err);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            fail(QStringLiteral("Cannot start ssh: %1").arg(process->errorString()));
        }
    });
    process->start(command.constFirst(), command.mid(1));
}

static QString lastLines(const QByteArray &text)
{
    const QStringList lines = QString::fromLocal8Bit(text).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    return lines.mid(std::max<qsizetype>(0, lines.size() - 3)).join(QLatin1Char('\n')).trimmed();
}

void RemoteAgentInstaller::probePlatform()
{
    const QString name = programName(m_program);
    // The name goes into the scripts: validated first.
    if (!KateRemote::isValidRemoteAgentPackage({QStringLiteral("a"), QStringLiteral("1"), QStringLiteral("./a"), name})) {
        fail(QStringLiteral("Cannot install an agent run as %1.").arg(m_program));
        return;
    }
    Q_EMIT progress(QStringLiteral("Checking %1's platform…").arg(m_location->authority));
    runScript(KateRemote::remotePlatformScript(m_windows, name), QString(), [this, name](int exitCode, const QByteArray &out, const QByteArray &err) {
        const auto platform = KateRemote::parseRemotePlatform(QString::fromLocal8Bit(out));
        if (exitCode != 0 || !platform) {
            const QString details = lastLines(err);
            fail(exitCode == 255 ? QStringLiteral("ssh to %1 failed.%2").arg(m_location->authority, details.isEmpty() ? QString() : QLatin1Char('\n') + details)
                                 : QStringLiteral("Cannot tell %1's platform.%2").arg(m_location->authority, details.isEmpty() ? QString() : QLatin1Char('\n') + details));
            return;
        }
        const QJsonObject build = m_agent.value(QStringLiteral("distribution"))
                                      .toObject()
                                      .value(QStringLiteral("binary"))
                                      .toObject()
                                      .value(platform->key)
                                      .toObject();
        if (build.isEmpty()) {
            fail(QStringLiteral("The ACP registry has no %1 build for %2 (%3).").arg(agentName(), m_location->authority, platform->key));
            return;
        }
        if (platform->installed) {
            Q_EMIT progress(QStringLiteral("A copy Kate Code installed earlier is on %1; it is replaced.").arg(m_location->authority));
        }
        m_package.id = m_agent.value(QStringLiteral("id")).toString();
        m_package.version = m_agent.value(QStringLiteral("version")).toString();
        m_package.cmd = build.value(QStringLiteral("cmd")).toString();
        m_package.name = name;
        if (!KateRemote::isValidRemoteAgentPackage(m_package)
            || programName(cmdPath(m_package.cmd)).compare(name, m_windows ? Qt::CaseInsensitive : Qt::CaseSensitive) != 0) {
            fail(QStringLiteral("The ACP registry's %1 build for %2 does not run %3.").arg(agentName(), platform->key, name));
            return;
        }
        download(build);
    });
}

void RemoteAgentInstaller::download(const QJsonObject &build)
{
    const QUrl url(build.value(QStringLiteral("archive")).toString());
    m_expectedSha256 = build.value(QStringLiteral("sha256")).toString().toLower();
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
    if (!hex.match(m_expectedSha256).hasMatch()) {
        fail(QStringLiteral("The ACP registry gives no SHA-256 for %1's download.").arg(agentName()));
        return;
    }
    if (!url.isValid() || (url.scheme() != QLatin1String("https") && !(m_allowFileUrls && url.scheme() == QLatin1String("file")))) {
        fail(QStringLiteral("The ACP registry's download for %1 is not an https URL.").arg(agentName()));
        return;
    }
    QString fileName = url.fileName();
    if (fileName.isEmpty() || fileName.startsWith(QLatin1Char('.'))) {
        fileName = QStringLiteral("download");
    }
    // Per hash: a cached file is the verified one.
    const QString dir = m_downloadDir + QLatin1Char('/') + m_expectedSha256;
    m_archive = dir + QLatin1Char('/') + fileName;
    if (QFileInfo::exists(m_archive) && sha256Of(m_archive) == m_expectedSha256) {
        prepareArchive();
        return;
    }
    QDir().mkpath(dir);
    m_downloadFile = new QSaveFile(m_archive);
    if (!m_downloadFile->open(QIODevice::WriteOnly)) {
        fail(QStringLiteral("Cannot write %1: %2").arg(m_archive, m_downloadFile->errorString()));
        return;
    }
    m_hash = new QCryptographicHash(QCryptographicHash::Sha256);
    if (!m_network) {
        m_network = new QNetworkAccessManager(this);
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    Q_EMIT progress(QStringLiteral("Downloading %1 %2 (%3)…").arg(agentName(), m_package.version, url.toDisplayString()));
    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        const QByteArray data = m_reply->readAll();
        m_hash->addData(data);
        if (m_downloadFile->write(data) != data.size()) {
            fail(QStringLiteral("Cannot write %1: %2").arg(m_archive, m_downloadFile->errorString()));
        }
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (total <= 0) {
            return;
        }
        const int percent = int(received * 100 / total);
        if (percent / 10 != m_lastPercent / 10) {
            m_lastPercent = percent;
            Q_EMIT progress(QStringLiteral("Downloaded %1% of %2 MB").arg(percent).arg(total / (1024 * 1024)));
        }
    });
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_reply;
        reply->deleteLater();
        if (m_done) {
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            fail(QStringLiteral("Download failed: %1").arg(reply->errorString()));
            return;
        }
        const QByteArray rest = reply->readAll();
        m_hash->addData(rest);
        m_downloadFile->write(rest);
        const QString sha256 = QString::fromLatin1(m_hash->result().toHex());
        if (sha256 != m_expectedSha256) {
            fail(QStringLiteral("The download's SHA-256 is %1, not the registry's %2: not installed.").arg(sha256, m_expectedSha256));
            return;
        }
        if (!m_downloadFile->commit()) {
            fail(QStringLiteral("Cannot write %1: %2").arg(m_archive, m_downloadFile->errorString()));
            return;
        }
        delete m_downloadFile;
        m_downloadFile = nullptr;
        prepareArchive();
    });
}

void RemoteAgentInstaller::prepareArchive()
{
    if (isTarGz(QFileInfo(m_archive).fileName())) {
        m_sendFile = m_archive;
        install();
        return;
    }
    // The host gets a .tar.gz whatever the download is (zip, .tar.bz2, a
    // bare program): only tar (and gzip) are needed there.
    Q_EMIT progress(QStringLiteral("Re-packing %1 for %2…").arg(QFileInfo(m_archive).fileName(), m_location->authority));
    QTemporaryFile temp(QDir::tempPath() + QStringLiteral("/kate-code-agent-XXXXXX.tar.gz"));
    temp.setAutoRemove(false);
    if (!temp.open()) {
        fail(QStringLiteral("Cannot create a temporary file: %1").arg(temp.errorString()));
        return;
    }
    m_tempArchive = temp.fileName();
    temp.close();
    auto result = std::make_shared<QString>();
    const QString source = m_archive;
    const QString target = m_tempArchive;
    const QString cmd = m_package.cmd;
    QThread *thread = QThread::create([result, source, cmd, target] {
        *result = repack(source, cmd, target);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    const QPointer<RemoteAgentInstaller> self(this);
    connect(thread, &QThread::finished, this, [self, result, target] {
        if (!self || self->m_done || self->m_tempArchive != target) {
            QFile::remove(target);
            return;
        }
        if (!result->isEmpty()) {
            self->fail(QStringLiteral("Cannot re-pack the download: %1").arg(*result));
            return;
        }
        self->m_sendFile = target;
        self->install();
    });
    thread->start();
}

void RemoteAgentInstaller::install()
{
    Q_EMIT progress(QStringLiteral("Installing %1 %2 on %3…").arg(agentName(), m_package.version, m_location->authority));
    runScript(KateRemote::remoteAgentInstallScript(m_windows, m_package), m_sendFile, [this](int exitCode, const QByteArray &out, const QByteArray &err) {
        if (exitCode == 0 && out.contains("installed")) {
            succeed(QStringLiteral("Installed %1 %2 on %3 (%4).")
                        .arg(agentName(),
                             m_package.version,
                             m_location->authority,
                             m_windows ? QStringLiteral("%LOCALAPPDATA%\\kate-code") : QStringLiteral("~/.local/share/kate-code")));
            return;
        }
        const QString details = lastLines(err);
        fail(QStringLiteral("Installing on %1 failed (exit code %2).%3").arg(m_location->authority).arg(exitCode).arg(details.isEmpty() ? QString() : QLatin1Char('\n') + details));
    });
}
