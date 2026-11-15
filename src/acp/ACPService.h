#pragma once

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>

#include "RemotePathMap.h"

class ACPService : public QObject
{
    Q_OBJECT

public:
    explicit ACPService(QObject *parent = nullptr);
    ~ACPService() override;

    void setExecutable(const QString &executable, const QStringList &args = QStringList());
    bool start(const QString &workingDir);
    void stop();

    int sendRequest(const QString &method, const QJsonObject &params = QJsonObject());
    void sendNotification(const QString &method, const QJsonObject &params = QJsonObject());
    void sendResponse(int requestId, const QJsonObject &result = QJsonObject(), const QJsonObject &error = QJsonObject());

    bool isRunning() const;

    // Agents for folders on ssh hosts (below kate-remote-fs's mount root,
    // sftp/fish) run on the host, through ssh (default). Off: always local.
    void setAllowRemote(bool allow)
    {
        m_allowRemote = allow;
    }
    // Whether the agent started last runs on a remote host. Messages are
    // translated: the agent sees the host's paths, signals and callers
    // local ones (pathMap()).
    bool isRemote() const
    {
        return !m_pathMap.isIdentity();
    }
    const KateRemote::RemotePathMap &pathMap() const
    {
        return m_pathMap;
    }
    // The host ([user@]host[:port]) an agent for workingDir runs on (a
    // folder on an ssh host, see setAllowRemote()), else empty (local).
    static QString remoteHostFor(const QString &workingDir);
    // kate-remote-fs-askpass from the kate-remote-fs daemon (DBus), empty
    // without it: for ssh without a terminal.
    static QString remoteFsAskpassProgram();
    // Kate's MCP server for a remote agent: program (kate-mcp-server) and
    // its environment. A remote start runs it with --http on a local port
    // and asks ssh to forward a port of the host's 127.0.0.1 to it (-R
    // 0:...). Empty program: none.
    void setRemoteMcpServer(const QString &program, const QProcessEnvironment &env)
    {
        m_mcpProgram = program;
        m_mcpEnvironment = env;
    }
    // Waiting for ssh to report the forwarded port (remoteMcpReady()).
    bool isRemoteMcpPending() const
    {
        return m_mcpPending;
    }
    // The server's URL on the agent's host and its bearer token, once
    // forwarded; empty when there is none (forwarding refused...).
    QString remoteMcpUrl() const;
    QString remoteMcpToken() const
    {
        return m_mcpRemotePort > 0 ? QString::fromLatin1(m_mcpToken) : QString();
    }

    // [user@]host[:port] of a remote agent, else empty.
    QString remoteHost() const
    {
        return m_remoteHost;
    }

Q_SIGNALS:
    void notificationReceived(const QString &method, const QJsonObject &params, int requestId);
    void responseReceived(int id, const QJsonObject &result, const QJsonObject &error);
    void connected();
    void disconnected(int exitCode);
    void errorOccurred(const QString &message);
    // A remote agent exited with 127: not found on the host (or not
    // runnable there). Emitted before errorOccurred() and disconnected().
    void remoteAgentMissing();
    void jsonPayload(const QString &direction, const QString &json);
    // The remote MCP server's port is known (or failed): see remoteMcpUrl().
    void remoteMcpReady();

private Q_SLOTS:
    void onStdout();
    void onStderr();
    void onFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onError(QProcess::ProcessError error);

private:
    void handleMessage(const QJsonObject &rawMsg);
    void write(const QJsonObject &msg);
    // The ssh command for an agent in workingDir on its host, or empty
    // when the agent runs locally (sets up m_pathMap and m_remoteHost).
    QStringList remoteCommand(const QString &workingDir);
    void removeAskpassState();
    // Starts kate-mcp-server --http; its local port, 0 on failure.
    quint16 startMcpServer();
    void stopMcpServer();
    void mcpForwardResult(int remotePort);

    QProcess *m_process;
    int m_messageId;
    // Raw bytes: split into lines before UTF-8 decoding, so multi-byte
    // sequences straddling a read boundary are never mangled.
    QByteArray m_buffer;
    QString m_executable;
    QStringList m_executableArgs;
    bool m_allowRemote = true;
    KateRemote::RemotePathMap m_pathMap;
    QString m_remoteHost;
    QString m_askpassState;
    // The last bytes the remote side wrote on stderr (ssh's or the agent's
    // messages), reported when it fails.
    QByteArray m_stderrTail;
    QByteArray m_stderrLine; // an incomplete line of ssh's stderr

    QString m_mcpProgram;
    QProcessEnvironment m_mcpEnvironment;
    QProcess *m_mcpProcess = nullptr;
    QByteArray m_mcpToken;
    quint16 m_mcpLocalPort = 0;
    int m_mcpRemotePort = 0;
    bool m_mcpPending = false;
};
