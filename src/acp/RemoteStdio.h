#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

#include "RemoteLocation.h"

class QFile;
class QTimer;

// A program whose stdin/stdout are a byte stream (an ACP agent,
// kate-code-helper, an install script), with QProcess's interface:
// - Directly: a process (an agent on this computer, `ssh -T` to a POSIX
//   host: the program's stdio is ssh's).
// - Through a tunnel (Windows hosts, setTunnel()): the command started is
//   the ssh running one of KateRemote's Windows scripts, which prints
//   "KATECODE-TUNNEL <port> <token>" (KateRemote::TunnelLinePrefix) and
//   serves the stream on the host's 127.0.0.1. Then a second ssh
//   (KateRemote::sshTunnelCommand(): -W, through the first one's connection,
//   which is started as a ControlMaster) carries it, the token first.
//   Output and input before the tunnel (lines the script prints before it,
//   writes) are passed on and kept; the exit code and status are the
//   script's ssh's (the program's), stderr both ssh's. The tunnel's ssh
//   failing while the script runs (forwarding refused...) ends the script's
//   ssh unless it ends on its own within 3 s: exit code 255. After the
//   tunnel ends normally the script's ssh has 15 s to end (the scripts keep
//   the connection until they end), and the other way round.
class RemoteStdio : public QObject
{
    Q_OBJECT

public:
    explicit RemoteStdio(QObject *parent = nullptr);
    ~RemoteStdio() override;

    // Before start().
    void setProcessEnvironment(const QProcessEnvironment &env);
    void setWorkingDirectory(const QString &dir);
    // The stream's input: this file, then its end.
    void setStandardInputFile(const QString &file);
    // ssh's passwords through askpass (KateRemote::setSshAskpassEnvironment(),
    // with a state file per ssh, removed when it ends).
    void setAskpass(const QString &askpass, const QString &authority);
    // The stream goes through a tunnel to location's host (ssh: the ssh
    // command for the tunnel's ssh); start()'s command line ends with
    // "-- host <command>".
    void setTunnel(const KateRemote::Location &location, const QStringList &ssh);
    bool usesTunnel() const
    {
        return m_location.has_value();
    }

    // For the process start() starts (QProcess::setChildProcessModifier()).
    void setChildProcessModifier(const std::function<void()> &modifier)
    {
        m_childModifier = modifier;
    }
    // The process start() started (the script's ssh with a tunnel), e.g. for
    // its processId().
    QProcess *launcher() const
    {
        return m_launcher;
    }

    void start(const QString &program, const QStringList &args);
    bool waitForStarted(int msecs = 30000);
    bool waitForFinished(int msecs = 30000);
    // Running while either process runs.
    QProcess::ProcessState state() const;
    QString errorString() const;

    qint64 write(const QByteArray &data);
    void closeWriteChannel();
    void kill();
    QByteArray readAllStandardOutput();
    QByteArray readAllStandardError();

Q_SIGNALS:
    void readyReadStandardOutput();
    void readyReadStandardError();
    void finished(int exitCode, QProcess::ExitStatus status);
    // The launcher's errors (QProcess::FailedToStart: no finished()).
    void errorOccurred(QProcess::ProcessError error);
    // The tunnel's ssh was started.
    void tunnelStarted();

private:
    QProcessEnvironment environmentFor(QString *stateFile);
    void onLauncherStdout();
    void startTunnel(quint16 port, const QByteArray &token);
    void pumpInput();
    void onLauncherFinished(int exitCode, QProcess::ExitStatus status);
    void onTunnelFinished(int exitCode, QProcess::ExitStatus status);
    // The other process should end soon: killed if it has not after msecs
    // (then finished() gives forcedExitCode, if any).
    void killLater(QProcess *process, int msecs, std::optional<int> forcedExitCode);
    void maybeFinish();
    void appendStdout(const QByteArray &data);
    void appendStderr(const QByteArray &data);
    void cleanUp();

    QPointer<QProcess> m_launcher;
    QPointer<QProcess> m_tunnel;
    QProcessEnvironment m_environment = QProcessEnvironment::systemEnvironment();
    QString m_workingDirectory;
    std::function<void()> m_childModifier;
    QString m_inputFile;
    QFile *m_input = nullptr;
    QString m_askpass;
    QString m_askpassAuthority;
    bool m_useAskpass = false;
    QStringList m_askpassStates;

    std::optional<KateRemote::Location> m_location;
    QStringList m_ssh;
    QString m_controlPath;
    QByteArray m_launcherLine; // launcher output before the tunnel line
    bool m_tunnelSeen = false;
    QByteArray m_pendingInput;
    bool m_closeRequested = false;
    QTimer *m_graceTimer = nullptr;

    QByteArray m_stdout;
    QByteArray m_stderr;
    bool m_launcherDone = false;
    bool m_tunnelDone = false;
    bool m_finished = false;
    int m_exitCode = 0;
    QProcess::ExitStatus m_exitStatus = QProcess::NormalExit;
    std::optional<int> m_forcedExitCode;
};
