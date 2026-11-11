#pragma once

#include "ACPModels.h"
#include <QHash>
#include <QObject>
#include <QProcessEnvironment>
#include <functional>
#include <optional>

#include <KPtyProcess>

class QTimer;

class TerminalManager : public QObject
{
    Q_OBJECT

public:
    explicit TerminalManager(QObject *parent = nullptr);
    ~TerminalManager() override;

    // Create a new terminal and spawn the command
    // Returns empty string on failure
    QString createTerminal(const QString &command, const QStringList &args,
                          const QProcessEnvironment &env, const QString &cwd,
                          qint64 outputByteLimit = 0);

    // Query terminal output (non-blocking)
    struct OutputResult {
        QString output;
        bool truncated = false;
        std::optional<int> exitStatus;
    };
    OutputResult getOutput(const QString &terminalId) const;

    // Wait for a terminal to exit without blocking: callback runs once, when
    // the process exits (success), the timeout expires, the process fails to
    // start or the terminal is released (success false). It may run before
    // this returns if the terminal has already exited.
    struct WaitResult {
        QString output;
        bool truncated = false;
        int exitStatus = -1;
        bool success = false;
    };
    using WaitCallback = std::function<void(const WaitResult &)>;
    void waitForExit(const QString &terminalId, int timeoutMs, WaitCallback callback);

    // Kill the terminal process (keeps terminal valid for output queries)
    bool killTerminal(const QString &terminalId);

    // Release terminal (kill if running, invalidate)
    bool releaseTerminal(const QString &terminalId);

    // Check if terminal exists and is valid
    bool isValid(const QString &terminalId) const;

    // Release all terminals (called on session end)
    void releaseAll();

    // Set default terminal size for new terminals (columns x rows)
    void setDefaultTerminalSize(int columns, int rows);

    // Get current default terminal size
    int defaultColumns() const { return m_defaultColumns; }
    int defaultRows() const { return m_defaultRows; }

Q_SIGNALS:
    // Emitted when new output is available (for live UI updates)
    void outputAvailable(const QString &terminalId, const QString &output, bool finished);

    // Emitted when terminal exits
    void terminalExited(const QString &terminalId, int exitCode);

private Q_SLOTS:
    void onProcessReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);

private:
    QString generateTerminalId();
    void truncateOutputIfNeeded(const QString &terminalId);

    struct TerminalData {
        KPtyProcess *process = nullptr;
        QByteArray outputBuffer;
        TerminalStatus status = TerminalStatus::Running;
        int exitCode = -1;
        qint64 outputByteLimit = 0;
        bool truncated = false;
        QString command;  // For debugging/display
    };

    struct Waiter {
        quint64 id;
        QString terminalId;
        WaitCallback callback;
        QTimer *timer;
    };
    // Complete (and remove) the waiters of a terminal, or one waiter by id.
    void completeWaiters(const QString &terminalId);
    void completeWaiter(quint64 waiterId);
    WaitResult currentResult(const QString &terminalId) const;

    QHash<QString, TerminalData> m_terminals;
    QList<Waiter> m_waiters;
    quint64 m_nextWaiterId = 0;
    int m_idCounter = 0;
    int m_defaultColumns = 120;  // Default terminal width
    int m_defaultRows = 40;      // Default terminal height
};
