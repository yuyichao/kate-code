#include "ACPService.h"

#include "AgentInstaller.h"

#include "RemoteLocation.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>

ACPService::ACPService(QObject *parent)
    : QObject(parent)
    , m_process(nullptr)
    , m_messageId(0)
    , m_executable(QStringLiteral("claude-agent-acp"))
{
}

void ACPService::setExecutable(const QString &executable, const QStringList &args)
{
    m_executable = executable;
    m_executableArgs = args;
}

ACPService::~ACPService()
{
    // Disconnect signals before cleanup to prevent signal emission during destruction
    if (m_process) {
        disconnect(m_process, nullptr, this, nullptr);
    }
    stop();
    removeAskpassState();
}

// kate-remote-fs-askpass, as kate-remote-fs uses it (empty when its
// KdePasswords setting is off or the daemon does not run).
QString ACPService::remoteFsAskpassProgram()
{
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KateRemoteFS"),
                                                       QStringLiteral("/org/kde/KateRemoteFS"),
                                                       QStringLiteral("org.kde.KateRemoteFS"),
                                                       QStringLiteral("askpassProgram"));
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 3000);
    return reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty() ? reply.arguments().constFirst().toString() : QString();
}

QString ACPService::remoteHostFor(const QString &workingDir)
{
    const auto location = KateRemote::locationForPath(workingDir, KateRemote::configuredMountRoot());
    if (!location || KateRemote::sshAgentCommand(*location, {QStringLiteral("ssh")}, QStringLiteral("agent"), {}).isEmpty()) {
        return {};
    }
    return location->authority;
}

QStringList ACPService::remoteCommand(const QString &workingDir)
{
    m_pathMap = KateRemote::RemotePathMap();
    m_remoteHost.clear();
    if (!m_allowRemote) {
        return {};
    }
    const auto location = KateRemote::locationForPath(workingDir, KateRemote::configuredMountRoot());
    if (!location || !KateRemote::isSshScheme(location->scheme)) {
        return {};
    }
    const QStringList command = KateRemote::sshAgentCommand(*location, KateRemote::configuredSshCommand(), m_executable, m_executableArgs);
    if (command.isEmpty()) {
        // fish logins ssh does not take (DOMAIN;user): the agent works on
        // the mount locally, as for other remote folders.
        qWarning() << "[ACPService] Cannot run the agent on" << location->authority << "- running it locally";
        return {};
    }
    m_remoteHost = location->authority;
    const QString hostDir = KateRemote::pathForLocation({location->scheme, location->authority, QStringLiteral("/")}, KateRemote::configuredMountRoot());
    const QString windowsPath = KateRemote::windowsPathForSftpPath(location->path);
    m_pathMap = windowsPath.isEmpty() ? KateRemote::RemotePathMap(hostDir)
                                      : KateRemote::RemotePathMap(hostDir, KateRemote::RemotePathMap::Style::Windows, location->path.at(1));
    return command;
}

QString ACPService::remoteMcpUrl() const
{
    return m_mcpRemotePort > 0 ? QStringLiteral("http://127.0.0.1:%1/mcp").arg(m_mcpRemotePort) : QString();
}

quint16 ACPService::startMcpServer()
{
    QByteArray token(32, Qt::Uninitialized);
    for (char &c : token) {
        c = "0123456789abcdef"[QRandomGenerator::system()->bounded(16)];
    }
    QStringList args{QStringLiteral("--http"), QStringLiteral("--remote-root"), m_pathMap.hostDir()};
    if (m_pathMap.style() == KateRemote::RemotePathMap::Style::Windows) {
        // The drive as the mount spells it: the one of the folder.
        args << QStringLiteral("--remote-windows") << QString(m_pathMap.drive().isNull() ? QLatin1Char('C') : m_pathMap.drive());
    }
    QProcessEnvironment env = m_mcpEnvironment;
    env.insert(QStringLiteral("KATECODE_MCP_TOKEN"), QString::fromLatin1(token));
    m_mcpProcess = new QProcess(this);
    m_mcpProcess->setProcessEnvironment(env);
    m_mcpProcess->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    m_mcpProcess->start(m_mcpProgram, args);
    // It prints its port at once ("PORT <n>").
    QByteArray output;
    QElapsedTimer timer;
    timer.start();
    while (!output.contains('\n') && timer.elapsed() < 3000 && m_mcpProcess->state() != QProcess::NotRunning) {
        if (m_mcpProcess->waitForReadyRead(100)) {
            output += m_mcpProcess->readAllStandardOutput();
        }
    }
    const QByteArray line = output.left(output.indexOf('\n')).trimmed();
    const quint16 port = line.startsWith("PORT ") ? line.mid(5).toUShort() : 0;
    if (port == 0) {
        qWarning() << "[ACPService] Kate's MCP server did not start:" << m_mcpProgram << output;
        stopMcpServer();
        return 0;
    }
    m_mcpToken = token;
    connect(m_mcpProcess, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        qWarning() << "[ACPService] Kate's MCP server for" << m_remoteHost << "exited:" << exitCode << status;
    });
    return port;
}

void ACPService::stopMcpServer()
{
    if (m_mcpProcess) {
        m_mcpProcess->disconnect(this);
        // It ends at end of input.
        m_mcpProcess->closeWriteChannel();
        if (!m_mcpProcess->waitForFinished(1000)) {
            m_mcpProcess->kill();
            m_mcpProcess->waitForFinished(1000);
        }
        m_mcpProcess->deleteLater();
        m_mcpProcess = nullptr;
    }
    m_mcpLocalPort = 0;
    m_mcpRemotePort = 0;
    m_mcpToken.clear();
    if (m_mcpPending) {
        m_mcpPending = false;
        Q_EMIT remoteMcpReady();
    }
}

void ACPService::mcpForwardResult(int remotePort)
{
    if (!m_mcpPending) {
        return;
    }
    m_mcpPending = false;
    m_mcpRemotePort = remotePort;
    qDebug() << "[ACPService] Kate's MCP server on" << m_remoteHost << ":" << remoteMcpUrl();
    Q_EMIT remoteMcpReady();
}

void ACPService::removeAskpassState()
{
    if (!m_askpassState.isEmpty()) {
        QFile::remove(m_askpassState);
        m_askpassState.clear();
    }
}

bool ACPService::start(const QString &workingDir)
{
    qDebug() << "[ACPService] Starting" << m_executable << "in:" << workingDir;

    if (m_process) {
        qDebug() << "[ACPService] Stopping existing process";
        stop();
    }

    // A partial line left over from a crashed process must not prepend
    // itself to the new process's first message.
    m_buffer.clear();

    m_stderrTail.clear();
    m_stderrLine.clear();
    removeAskpassState();
    stopMcpServer();

    QStringList remote = remoteCommand(workingDir);
    if (!remote.isEmpty() && !m_mcpProgram.isEmpty()) {
        // Kate's tools: an HTTP MCP server here, reached from the host's
        // 127.0.0.1 through ssh. Without ExitOnForwardFailure a host that
        // refuses forwarding still runs the agent (without the tools).
        m_mcpLocalPort = startMcpServer();
        if (m_mcpLocalPort > 0) {
            // Before "-- host <command>". A connection of its own: through
            // a shared one (ControlMaster) ssh does not report the port,
            // and the forward would outlive the agent; the report is an
            // INFO message (a LogLevel of ERROR in ssh_config hides it).
            // ssh takes the first value of an option: these beat the
            // config files.
            const QStringList forward{QStringLiteral("-o"),
                                      QStringLiteral("ControlPath=none"),
                                      QStringLiteral("-o"),
                                      QStringLiteral("LogLevel=INFO"),
                                      QStringLiteral("-R"),
                                      QStringLiteral("0:127.0.0.1:%1").arg(m_mcpLocalPort)};
            for (const QString &arg : forward) {
                remote.insert(remote.size() - 3, arg);
            }
            m_mcpPending = true;
        }
    }

    m_process = new QProcess(this);

    connect(m_process, &QProcess::readyReadStandardOutput, this, &ACPService::onStdout);
    connect(m_process, &QProcess::readyReadStandardError, this, &ACPService::onStderr);
    connect(m_process, &QProcess::finished, this, &ACPService::onFinished);
    connect(m_process, &QProcess::errorOccurred, this, &ACPService::onError);

    if (!remote.isEmpty()) {
        // ssh without a terminal: passwords through SSH_ASKPASS, from KDE's
        // password cache like the mounts, else a dialog.
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        const QString state = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/kate-code-askpass-")
            + QUuid::createUuid().toString(QUuid::Id128);
        if (KateRemote::setSshAskpassEnvironment(env, remoteFsAskpassProgram(), m_remoteHost, state)) {
            m_askpassState = state;
        }
        m_process->setProcessEnvironment(env);
        // Not the mount: the agent's directory is on the host.
        m_process->setWorkingDirectory(QDir::homePath());
        qDebug() << "[ACPService] Starting agent on" << m_remoteHost << ":" << remote;
        m_process->start(remote.constFirst(), remote.mid(1));
    } else {
        m_process->setWorkingDirectory(workingDir);

        // PATH, then per-user directories desktop sessions may lack in PATH,
        // then the agents Kate Code installed (AgentInstaller).
        QString resolvedExecutable = AgentInstaller::findLocalExecutable(m_executable);
        if (resolvedExecutable.isEmpty()) {
            resolvedExecutable = m_executable;
        }

        qDebug() << "[ACPService] Starting process:" << resolvedExecutable << m_executableArgs;
        m_process->start(resolvedExecutable, m_executableArgs);
    }

    qDebug() << "[ACPService] Waiting for process to start...";
    if (!m_process->waitForStarted(5000)) {
        qDebug() << "[ACPService] Process failed to start";
        Q_EMIT errorOccurred(isRemote() ? QStringLiteral("Failed to start ssh for the agent on %1").arg(m_remoteHost)
                             : AgentInstaller::findLocalExecutable(m_executable).isEmpty()
                             ? QStringLiteral("%1 is not installed on this computer (Settings > Configure Kate > Kate Code: Install...)").arg(m_executable)
                             : QStringLiteral("Failed to start %1").arg(m_executable));
        return false;
    }

    qDebug() << "[ACPService] Process started successfully";
    Q_EMIT connected();
    return true;
}

void ACPService::stop()
{
    if (m_process) {
        // Disconnect signals BEFORE killing to prevent onFinished from being called
        // This avoids a race condition where both stop() and onFinished() try to clean up m_process
        disconnect(m_process, nullptr, this, nullptr);

        m_process->kill();
        m_process->waitForFinished(1000);
        m_process->deleteLater();
        m_process = nullptr;

        // Emit disconnected signal since onFinished won't be called (signals disconnected)
        Q_EMIT disconnected(0);
    }
    removeAskpassState();
    stopMcpServer();
}

void ACPService::write(const QJsonObject &msg)
{
    const QByteArray data = QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n";
    Q_EMIT jsonPayload(QStringLiteral(">>"), QString::fromUtf8(data).trimmed());
    m_process->write(data);
}

int ACPService::sendRequest(const QString &method, const QJsonObject &params)
{
    if (!m_process || m_process->state() != QProcess::Running) {
        qWarning() << "[ACPService] Cannot send request: ACP not connected";
        return -1;
    }

    m_messageId++;

    QJsonObject msg;
    msg[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    msg[QStringLiteral("id")] = m_messageId;
    msg[QStringLiteral("method")] = method;

    if (!params.isEmpty()) {
        // Local paths as the host's for a remote agent.
        msg[QStringLiteral("params")] = m_pathMap.outgoing(params);
    }

    qDebug() << "[ACPService] >>" << method << "id:" << m_messageId;
    write(msg);
    return m_messageId;
}

void ACPService::sendNotification(const QString &method, const QJsonObject &params)
{
    if (!m_process || m_process->state() != QProcess::Running) {
        qWarning() << "[ACPService] Cannot send notification: ACP not connected";
        return;
    }

    QJsonObject msg;
    msg[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    msg[QStringLiteral("method")] = method;

    if (!params.isEmpty()) {
        msg[QStringLiteral("params")] = m_pathMap.outgoing(params);
    }

    qDebug() << "[ACPService] >> notification:" << method;
    write(msg);
}

void ACPService::sendResponse(int requestId, const QJsonObject &result, const QJsonObject &error)
{
    if (!m_process || m_process->state() != QProcess::Running) {
        return;
    }

    QJsonObject msg;
    msg[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    msg[QStringLiteral("id")] = requestId;

    // Not translated: results carry file contents, not paths.
    if (!error.isEmpty()) {
        msg[QStringLiteral("error")] = error;
    } else {
        msg[QStringLiteral("result")] = result;
    }

    qDebug() << "[ACPService] >> response for request id:" << requestId;
    write(msg);
}

bool ACPService::isRunning() const
{
    return m_process && m_process->state() == QProcess::Running;
}

void ACPService::onStdout()
{
    if (!m_process) {
        return;
    }

    m_buffer += m_process->readAllStandardOutput();

    // Parse newline-delimited JSON, splitting at the byte level so multi-byte
    // UTF-8 sequences that straddle a chunk boundary decode correctly.
    int newlineIndex;
    while ((newlineIndex = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(newlineIndex);
        m_buffer.remove(0, newlineIndex + 1);

        if (line.trimmed().isEmpty()) {
            continue;
        }

        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);

        if (parseError.error != QJsonParseError::NoError) {
            qWarning() << "[ACPService] Failed to parse JSON:" << parseError.errorString();
            qWarning() << "[ACPService] Line:" << line;
            continue;
        }

        handleMessage(doc.object());
    }
}

void ACPService::handleMessage(const QJsonObject &rawMsg)
{
    Q_EMIT jsonPayload(QStringLiteral("<<"), QString::fromUtf8(QJsonDocument(rawMsg).toJson(QJsonDocument::Compact)));
    // The host's paths as local ones for a remote agent.
    const QJsonObject msg = m_pathMap.incoming(rawMsg);

    if (msg.contains(QStringLiteral("method"))) {
        // Notification or request from ACP
        QString method = msg[QStringLiteral("method")].toString();
        QJsonObject params = msg[QStringLiteral("params")].toObject();
        int requestId = msg[QStringLiteral("id")].toInt(-1);

        // Log session/update type for debugging
        if (method == QStringLiteral("session/update")) {
            QJsonObject update = params[QStringLiteral("update")].toObject();
            QString updateType = update[QStringLiteral("sessionUpdate")].toString();
            qDebug() << "[ACPService] <<" << method << "(type:" << updateType << ")";
        } else {
            qDebug() << "[ACPService] <<" << method;
        }

        Q_EMIT notificationReceived(method, params, requestId);
    } else if (msg.contains(QStringLiteral("id"))) {
        // Response to our request
        int id = msg[QStringLiteral("id")].toInt();
        QJsonObject result = msg[QStringLiteral("result")].toObject();
        QJsonObject error = msg[QStringLiteral("error")].toObject();

        qDebug() << "[ACPService] << response for request id:" << id
                 << "raw:" << QJsonDocument(msg).toJson(QJsonDocument::Compact);
        Q_EMIT responseReceived(id, result, error);
    }
}

void ACPService::onStderr()
{
    if (!m_process) {
        return;
    }

    QByteArray data = m_process->readAllStandardError();
    if (isRemote()) {
        m_stderrTail = QByteArray(m_stderrTail + data).right(2000);
        if (m_mcpPending) {
            // ssh reports the forward: "Allocated port <n> for remote
            // forward to 127.0.0.1:<local>", or "Warning: remote port
            // forwarding failed for listen port 0".
            m_stderrLine += data;
            qsizetype end;
            while (m_mcpPending && (end = m_stderrLine.indexOf('\n')) >= 0) {
                const QString line = QString::fromLocal8Bit(m_stderrLine.left(end)).trimmed();
                m_stderrLine.remove(0, end + 1);
                static const QRegularExpression allocated(QStringLiteral("^Allocated port (\\d+) for remote forward to 127\\.0\\.0\\.1:(\\d+)"));
                const auto match = allocated.match(line);
                if (match.hasMatch() && match.captured(2).toUShort() == m_mcpLocalPort) {
                    mcpForwardResult(match.captured(1).toInt());
                } else if (line.contains(QLatin1String("remote port forwarding failed"))) {
                    qWarning() << "[ACPService]" << m_remoteHost << "refused the forward for Kate's MCP server:" << line;
                    mcpForwardResult(0);
                }
            }
        }
    }
    QString message = QString::fromUtf8(data).trimmed();

    if (!message.isEmpty()) {
        // Bridge stderr is largely informational (progress, warnings). Log it,
        // but do NOT route it through errorOccurred: that signal is surfaced in
        // the chat now and should carry only genuine errors (process failures
        // and JSON-RPC error responses), not noisy stderr chatter.
        qDebug() << "[ACPService] stderr:" << message;
    }
}

void ACPService::onFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    qDebug() << "[ACPService] Process finished with exit code:" << exitCode;
    removeAskpassState();
    stopMcpServer();

    if (isRemote() && (exitCode != 0 || exitStatus != QProcess::NormalExit)) {
        // ssh's or the remote shell's reason, else a bare disconnect.
        const QStringList lines = QString::fromLocal8Bit(m_stderrTail).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        const QString details = lines.mid(std::max<qsizetype>(0, lines.size() - 3)).join(QLatin1Char('\n')).trimmed();
        QString message;
        if (exitCode == 127) {
            // Also a program that cannot run there (another platform, a
            // missing interpreter): the stderr lines below tell.
            message = m_pathMap.style() == KateRemote::RemotePathMap::Style::Windows
                ? QStringLiteral("%1 was not found on %2 (searched PATH, %USERPROFILE%\\.local\\bin, \\bin, \\.cargo\\bin, %APPDATA%\\npm and Kate Code's %LOCALAPPDATA%\\kate-code\\bin).")
                      .arg(QFileInfo(m_executable).fileName(), m_remoteHost)
                : QStringLiteral("%1 was not found on %2 (searched the login shell's PATH, ~/.local/bin, ~/bin, ~/.cargo/bin and Kate Code's ~/.local/share/kate-code/bin), or cannot run there.")
                      .arg(QFileInfo(m_executable).fileName(), m_remoteHost);
            Q_EMIT remoteAgentMissing();
        } else if (exitCode == 255) {
            message = QStringLiteral("ssh to %1 failed or the connection was lost.").arg(m_remoteHost);
        } else {
            message = QStringLiteral("The agent on %1 exited (exit code %2).").arg(m_remoteHost).arg(exitCode);
        }
        if (!details.isEmpty()) {
            message += QLatin1Char('\n') + details;
        }
        Q_EMIT errorOccurred(message);
    }

    if (m_process) {
        // Disconnect signals to prevent further callbacks during cleanup
        disconnect(m_process, nullptr, this, nullptr);
        m_process->deleteLater();
        m_process = nullptr;
    }

    Q_EMIT disconnected(exitCode);
}

void ACPService::onError(QProcess::ProcessError error)
{
    QString errorMsg = m_process ? m_process->errorString() : QString::number(error);
    qWarning() << "[ACPService] Process error:" << errorMsg;
    Q_EMIT errorOccurred(errorMsg);
}
