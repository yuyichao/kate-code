#include "RemoteStdio.h"

#include <QDeadlineTimer>
#include <QDebug>
#include <QFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

namespace
{
// After the tunnel ends normally the script's ssh ends once the program has
// exited and its stderr is written (the script allows itself 5 + 2 s); after
// the script's ssh ends, the tunnel's (closed by the script) ends at once.
constexpr int TunnelGraceMs = 15000;
// The tunnel's ssh failed (or a mux client reports the end oddly): the
// script's ssh may still end on its own (then its exit code counts), else
// it is killed (exit code 255).
constexpr int TunnelFailureGraceMs = 3000;
// Input from a file: written in chunks, this much queued at most.
constexpr qint64 InputChunk = 256 * 1024;
}

RemoteStdio::RemoteStdio(QObject *parent)
    : QObject(parent)
{
}

RemoteStdio::~RemoteStdio()
{
    for (QProcess *process : {m_tunnel.data(), m_launcher.data()}) {
        if (process) {
            process->disconnect(this);
            if (process->state() != QProcess::NotRunning) {
                process->kill();
                process->waitForFinished(1000);
            }
        }
    }
    cleanUp();
}

void RemoteStdio::setProcessEnvironment(const QProcessEnvironment &env)
{
    m_environment = env;
}

void RemoteStdio::setWorkingDirectory(const QString &dir)
{
    m_workingDirectory = dir;
}

void RemoteStdio::setStandardInputFile(const QString &file)
{
    m_inputFile = file;
}

void RemoteStdio::setAskpass(const QString &askpass, const QString &authority)
{
    m_useAskpass = true;
    m_askpass = askpass;
    m_askpassAuthority = authority;
}

void RemoteStdio::setTunnel(const KateRemote::Location &location, const QStringList &ssh)
{
    m_location = location;
    m_ssh = ssh;
}

QProcessEnvironment RemoteStdio::environmentFor(QString *stateFile)
{
    QProcessEnvironment env = m_environment;
    stateFile->clear();
    if (m_useAskpass) {
        const QString state = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/kate-code-askpass-")
            + QUuid::createUuid().toString(QUuid::Id128);
        if (KateRemote::setSshAskpassEnvironment(env, m_askpass, m_askpassAuthority, state)) {
            *stateFile = state;
            m_askpassStates << state;
        }
    }
    return env;
}

void RemoteStdio::start(const QString &program, const QStringList &args)
{
    m_launcher = new QProcess(this);
    QString state;
    m_launcher->setProcessEnvironment(environmentFor(&state));
    if (!m_workingDirectory.isEmpty()) {
        m_launcher->setWorkingDirectory(m_workingDirectory);
    }
    if (m_childModifier) {
        m_launcher->setChildProcessModifier(m_childModifier);
    }
    QStringList arguments = args;
    if (m_location) {
        // The tunnel's ssh goes through this connection (no second login,
        // nor a second password or host key question): a master of its own
        // socket, which ends with it. Before "-- host <command>"; an
        // option's first value counts, so these beat ssh_config.
        m_controlPath = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/kate-code-ssh-")
            + QUuid::createUuid().toString(QUuid::Id128).left(16);
        const QStringList master{QStringLiteral("-o"),
                                 QStringLiteral("ControlMaster=yes"),
                                 QStringLiteral("-o"),
                                 QStringLiteral("ControlPath=") + m_controlPath,
                                 QStringLiteral("-o"),
                                 QStringLiteral("ControlPersist=no")};
        const qsizetype at = std::max<qsizetype>(0, arguments.size() - 3);
        for (qsizetype i = 0; i < master.size(); ++i) {
            arguments.insert(at + i, master.at(i));
        }
        m_launcher->setStandardInputFile(QProcess::nullDevice());
    } else if (!m_inputFile.isEmpty()) {
        m_launcher->setStandardInputFile(m_inputFile);
    }
    connect(m_launcher, &QProcess::readyReadStandardOutput, this, &RemoteStdio::onLauncherStdout);
    connect(m_launcher, &QProcess::readyReadStandardError, this, [this] {
        appendStderr(m_launcher->readAllStandardError());
    });
    connect(m_launcher, &QProcess::finished, this, &RemoteStdio::onLauncherFinished);
    connect(m_launcher, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_launcherDone = true;
            m_finished = true; // no finished() after a failed start, as QProcess
            cleanUp();
        } else if (error == QProcess::Crashed && m_forcedExitCode) {
            return; // killed here: finished() tells why
        }
        Q_EMIT errorOccurred(error);
    });
    m_launcher->start(program, arguments);
}

void RemoteStdio::onLauncherStdout()
{
    if (!m_launcher) {
        return;
    }
    const QByteArray data = m_launcher->readAllStandardOutput();
    if (!m_location || m_tunnelSeen) {
        appendStdout(data);
        return;
    }
    m_launcherLine += data;
    QByteArray passed;
    qsizetype newline;
    while (!m_tunnelSeen && (newline = m_launcherLine.indexOf('\n')) >= 0) {
        const QByteArray line = m_launcherLine.left(newline + 1);
        m_launcherLine.remove(0, newline + 1);
        if (const auto endpoint = KateRemote::parseTunnelLine(line.chopped(1))) {
            m_tunnelSeen = true;
            appendStdout(passed);
            passed.clear();
            startTunnel(endpoint->port, endpoint->token);
            // Whatever follows (at the end of the session) is output too.
            passed = std::exchange(m_launcherLine, {});
        } else {
            passed += line;
        }
    }
    appendStdout(passed);
}

void RemoteStdio::startTunnel(quint16 port, const QByteArray &token)
{
    const QStringList command = KateRemote::sshTunnelCommand(*m_location, m_ssh, m_controlPath, port);
    if (command.isEmpty()) {
        appendStderr(QByteArrayLiteral("Cannot reach the tunnel through ssh\n"));
        m_forcedExitCode = 255;
        m_launcher->kill();
        return;
    }
    m_tunnel = new QProcess(this);
    QString state;
    m_tunnel->setProcessEnvironment(environmentFor(&state));
    if (!m_workingDirectory.isEmpty()) {
        m_tunnel->setWorkingDirectory(m_workingDirectory);
    }
    connect(m_tunnel, &QProcess::readyReadStandardOutput, this, [this] {
        appendStdout(m_tunnel->readAllStandardOutput());
    });
    connect(m_tunnel, &QProcess::readyReadStandardError, this, [this] {
        appendStderr(m_tunnel->readAllStandardError());
    });
    connect(m_tunnel, &QProcess::bytesWritten, this, &RemoteStdio::pumpInput);
    connect(m_tunnel, &QProcess::finished, this, &RemoteStdio::onTunnelFinished);
    connect(m_tunnel, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            appendStderr(QStringLiteral("Cannot start ssh for the tunnel: %1\n").arg(m_tunnel->errorString()).toLocal8Bit());
            onTunnelFinished(255, QProcess::CrashExit);
        }
    });
    qDebug() << "[RemoteStdio] Tunnel to" << m_location->authority << "port" << port;
    m_tunnel->start(command.constFirst(), command.mid(1));
    m_tunnel->write(token + '\n');
    if (!m_inputFile.isEmpty()) {
        m_input = new QFile(m_inputFile, this);
        if (!m_input->open(QIODevice::ReadOnly)) {
            appendStderr(QStringLiteral("Cannot read %1: %2\n").arg(m_inputFile, m_input->errorString()).toLocal8Bit());
            delete m_input;
            m_input = nullptr;
            m_tunnel->closeWriteChannel();
        } else {
            pumpInput();
        }
    } else {
        if (!m_pendingInput.isEmpty()) {
            m_tunnel->write(std::exchange(m_pendingInput, {}));
        }
        if (m_closeRequested) {
            m_tunnel->closeWriteChannel();
        }
    }
    Q_EMIT tunnelStarted();
}

void RemoteStdio::pumpInput()
{
    if (!m_input || !m_tunnel) {
        return;
    }
    while (m_tunnel->bytesToWrite() < InputChunk && !m_input->atEnd()) {
        const QByteArray chunk = m_input->read(InputChunk);
        if (chunk.isEmpty()) {
            break;
        }
        m_tunnel->write(chunk);
    }
    if (m_input->atEnd()) {
        delete m_input;
        m_input = nullptr;
        m_tunnel->closeWriteChannel();
    }
}

void RemoteStdio::onLauncherFinished(int exitCode, QProcess::ExitStatus status)
{
    onLauncherStdout();
    if (m_launcher) {
        appendStderr(m_launcher->readAllStandardError());
    }
    if (!m_tunnelSeen && !m_launcherLine.isEmpty()) {
        appendStdout(std::exchange(m_launcherLine, {}));
    }
    m_launcherDone = true;
    m_exitCode = exitCode;
    m_exitStatus = status;
    if (m_tunnel && !m_tunnelDone) {
        killLater(m_tunnel, TunnelGraceMs, std::nullopt);
        return;
    }
    maybeFinish();
}

void RemoteStdio::onTunnelFinished(int exitCode, QProcess::ExitStatus status)
{
    if (m_tunnelDone) {
        return;
    }
    if (m_tunnel) {
        appendStdout(m_tunnel->readAllStandardOutput());
        appendStderr(m_tunnel->readAllStandardError());
    }
    m_tunnelDone = true;
    delete m_input;
    m_input = nullptr;
    if (m_launcher && !m_launcherDone) {
        if (exitCode != 0 || status != QProcess::NormalExit) {
            // Never connected (forwarding refused...) or the connection
            // failed: the script waits in vain.
            qWarning() << "[RemoteStdio] The tunnel to" << (m_location ? m_location->authority : QString()) << "ended:" << exitCode << status;
            killLater(m_launcher, TunnelFailureGraceMs, 255);
            return;
        }
        killLater(m_launcher, TunnelGraceMs, std::nullopt);
        return;
    }
    maybeFinish();
}

void RemoteStdio::killLater(QProcess *process, int msecs, std::optional<int> forcedExitCode)
{
    if (!m_graceTimer) {
        m_graceTimer = new QTimer(this);
        m_graceTimer->setSingleShot(true);
    }
    m_graceTimer->disconnect(this);
    connect(m_graceTimer, &QTimer::timeout, this, [this, process = QPointer<QProcess>(process), forcedExitCode] {
        if (process && process->state() != QProcess::NotRunning) {
            if (forcedExitCode) {
                m_forcedExitCode = forcedExitCode;
            }
            process->kill();
        }
    });
    m_graceTimer->start(msecs);
}

void RemoteStdio::maybeFinish()
{
    if (m_finished || !m_launcherDone || (m_tunnel && !m_tunnelDone)) {
        return;
    }
    m_finished = true;
    if (m_graceTimer) {
        m_graceTimer->stop();
    }
    cleanUp();
    if (m_forcedExitCode) {
        Q_EMIT finished(*m_forcedExitCode, QProcess::NormalExit);
    } else {
        Q_EMIT finished(m_exitCode, m_exitStatus);
    }
}

void RemoteStdio::cleanUp()
{
    for (const QString &state : std::as_const(m_askpassStates)) {
        QFile::remove(state);
    }
    m_askpassStates.clear();
    if (!m_controlPath.isEmpty() && (!m_launcher || m_launcher->state() == QProcess::NotRunning)) {
        // ssh removes its socket when it ends, but not when killed.
        QFile::remove(m_controlPath);
        m_controlPath.clear();
    }
}

void RemoteStdio::appendStdout(const QByteArray &data)
{
    if (data.isEmpty()) {
        return;
    }
    m_stdout += data;
    Q_EMIT readyReadStandardOutput();
}

void RemoteStdio::appendStderr(const QByteArray &data)
{
    if (data.isEmpty()) {
        return;
    }
    m_stderr += data;
    Q_EMIT readyReadStandardError();
}

bool RemoteStdio::waitForStarted(int msecs)
{
    return m_launcher && m_launcher->waitForStarted(msecs);
}

bool RemoteStdio::waitForFinished(int msecs)
{
    const QDeadlineTimer deadline(msecs);
    for (const QPointer<QProcess> *p : {&m_launcher, &m_tunnel}) {
        if (*p && (*p)->state() != QProcess::NotRunning) {
            (*p)->waitForFinished(int(std::max<qint64>(0, deadline.remainingTime())));
        }
    }
    // A tunnel started meanwhile (its line came while waiting).
    if (m_tunnel && m_tunnel->state() != QProcess::NotRunning && !deadline.hasExpired()) {
        m_tunnel->waitForFinished(int(std::max<qint64>(0, deadline.remainingTime())));
    }
    return state() == QProcess::NotRunning;
}

QProcess::ProcessState RemoteStdio::state() const
{
    if (m_launcher && m_launcher->state() != QProcess::NotRunning) {
        return m_launcher->state();
    }
    if (m_tunnel && m_tunnel->state() != QProcess::NotRunning) {
        return QProcess::Running;
    }
    return QProcess::NotRunning;
}

QString RemoteStdio::errorString() const
{
    return m_launcher ? m_launcher->errorString() : QString();
}

qint64 RemoteStdio::write(const QByteArray &data)
{
    if (!m_location) {
        return m_launcher ? m_launcher->write(data) : -1;
    }
    if (m_tunnel) {
        return m_tunnelDone ? -1 : m_tunnel->write(data);
    }
    if (m_closeRequested) {
        return -1;
    }
    // Until the tunnel is up.
    m_pendingInput += data;
    return data.size();
}

void RemoteStdio::closeWriteChannel()
{
    if (!m_location) {
        if (m_launcher) {
            m_launcher->closeWriteChannel();
        }
        return;
    }
    m_closeRequested = true;
    if (m_tunnel && !m_input) {
        m_tunnel->closeWriteChannel();
    }
}

void RemoteStdio::kill()
{
    // The tunnel first: closing the connection ends the program on the host
    // (the script closes its input, then kills it).
    if (m_tunnel && m_tunnel->state() != QProcess::NotRunning) {
        m_tunnel->kill();
    }
    if (m_launcher && m_launcher->state() != QProcess::NotRunning) {
        m_launcher->kill();
    }
}

QByteArray RemoteStdio::readAllStandardOutput()
{
    return std::exchange(m_stdout, {});
}

QByteArray RemoteStdio::readAllStandardError()
{
    return std::exchange(m_stderr, {});
}
