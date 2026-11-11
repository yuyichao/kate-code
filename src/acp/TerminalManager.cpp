#include "TerminalManager.h"

#include <QDebug>
#include <QTimer>

#include <KPtyDevice>

TerminalManager::TerminalManager(QObject *parent)
    : QObject(parent)
    , m_idCounter(0)
{
}

TerminalManager::~TerminalManager()
{
    // The owner (ACPSession) is being destroyed: drop pending waits without
    // running their callbacks, which would call back into it.
    m_waiters.clear();
    releaseAll();
}

QString TerminalManager::generateTerminalId()
{
    return QStringLiteral("term_%1").arg(++m_idCounter);
}

QString TerminalManager::createTerminal(const QString &command, const QStringList &args,
                                        const QProcessEnvironment &env, const QString &cwd,
                                        qint64 outputByteLimit)
{
    QString terminalId = generateTerminalId();

    qDebug() << "[TerminalManager] Creating terminal" << terminalId << "command:" << command << "args:" << args;

    auto *process = new KPtyProcess(this);
    process->setWorkingDirectory(cwd);
    process->setProcessEnvironment(env);

    // Use PTY for all channels - this makes programs think they're in a real terminal
    process->setPtyChannels(KPtyProcess::AllChannels);

    // Store terminal ID in process property for slot handlers
    process->setProperty("terminalId", terminalId);

    connect(process, &KPtyProcess::finished, this, &TerminalManager::onProcessFinished);
    connect(process, &KPtyProcess::errorOccurred, this, &TerminalManager::onProcessError);

    TerminalData data;
    data.process = process;
    data.outputByteLimit = outputByteLimit;
    data.command = command;
    m_terminals.insert(terminalId, data);

    process->setProgram(command);
    process->setArguments(args);
    process->start();

    if (!process->waitForStarted(5000)) {
        qWarning() << "[TerminalManager] Failed to start process for terminal" << terminalId;
        m_terminals.remove(terminalId);
        process->deleteLater();
        return QString();
    }

    // Connect to PTY device for reading output after process starts
    KPtyDevice *pty = process->pty();
    if (pty) {
        pty->setProperty("terminalId", terminalId);
        connect(pty, &KPtyDevice::readyRead, this, &TerminalManager::onProcessReadyRead);

        // Set terminal window size so programs know available columns/rows
        pty->setWinSize(m_defaultRows, m_defaultColumns);
    }

    qDebug() << "[TerminalManager] Terminal" << terminalId << "started with PTY size" << m_defaultColumns << "x" << m_defaultRows;
    return terminalId;
}

void TerminalManager::onProcessReadyRead()
{
    auto *pty = qobject_cast<KPtyDevice *>(sender());
    if (!pty) {
        return;
    }

    QString terminalId = pty->property("terminalId").toString();
    if (!m_terminals.contains(terminalId)) {
        return;
    }

    QByteArray newData = pty->readAll();
    m_terminals[terminalId].outputBuffer.append(newData);

    truncateOutputIfNeeded(terminalId);

    // Emit signal for live UI updates
    QString output = QString::fromUtf8(m_terminals[terminalId].outputBuffer);
    bool finished = m_terminals[terminalId].status != TerminalStatus::Running;
    Q_EMIT outputAvailable(terminalId, output, finished);
}

void TerminalManager::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitStatus);

    auto *process = qobject_cast<KPtyProcess *>(sender());
    if (!process) {
        return;
    }

    QString terminalId = process->property("terminalId").toString();
    if (!m_terminals.contains(terminalId)) {
        return;
    }

    qDebug() << "[TerminalManager] Terminal" << terminalId << "finished with exit code:" << exitCode;

    // Read any remaining output from PTY
    KPtyDevice *pty = process->pty();
    if (pty) {
        QByteArray remaining = pty->readAll();
        if (!remaining.isEmpty()) {
            m_terminals[terminalId].outputBuffer.append(remaining);
            truncateOutputIfNeeded(terminalId);
        }
    }

    m_terminals[terminalId].exitCode = exitCode;
    if (m_terminals[terminalId].status == TerminalStatus::Running) {
        m_terminals[terminalId].status = TerminalStatus::Exited;
    }

    // Emit final output update and exit signal
    QString output = QString::fromUtf8(m_terminals[terminalId].outputBuffer);
    Q_EMIT outputAvailable(terminalId, output, true);
    Q_EMIT terminalExited(terminalId, exitCode);
    completeWaiters(terminalId);
}

void TerminalManager::onProcessError(QProcess::ProcessError error)
{
    auto *process = qobject_cast<KPtyProcess *>(sender());
    if (!process) {
        return;
    }

    QString terminalId = process->property("terminalId").toString();
    qWarning() << "[TerminalManager] Terminal" << terminalId << "error:" << error << process->errorString();

    // A process that never started emits no finished(): end the terminal
    // here so waiters do not sit until their timeout.
    if (error == QProcess::FailedToStart && m_terminals.contains(terminalId)
        && m_terminals[terminalId].status == TerminalStatus::Running) {
        m_terminals[terminalId].status = TerminalStatus::Exited;
        m_terminals[terminalId].exitCode = -1;
        Q_EMIT terminalExited(terminalId, -1);
        completeWaiters(terminalId);
    }
}

void TerminalManager::truncateOutputIfNeeded(const QString &terminalId)
{
    if (!m_terminals.contains(terminalId)) {
        return;
    }

    auto &data = m_terminals[terminalId];
    if (data.outputByteLimit > 0 && data.outputBuffer.size() > data.outputByteLimit) {
        // Truncate from the beginning to stay within limit
        qint64 excess = data.outputBuffer.size() - data.outputByteLimit;
        data.outputBuffer.remove(0, excess);
        data.truncated = true;
    }
}

TerminalManager::OutputResult TerminalManager::getOutput(const QString &terminalId) const
{
    OutputResult result;

    if (!m_terminals.contains(terminalId)) {
        qWarning() << "[TerminalManager] getOutput: terminal not found:" << terminalId;
        return result;
    }

    const auto &data = m_terminals[terminalId];
    result.output = QString::fromUtf8(data.outputBuffer);
    result.truncated = data.truncated;

    if (data.status != TerminalStatus::Running) {
        result.exitStatus = data.exitCode;
    }

    return result;
}

TerminalManager::WaitResult TerminalManager::currentResult(const QString &terminalId) const
{
    WaitResult result;
    auto it = m_terminals.constFind(terminalId);
    if (it == m_terminals.constEnd()) {
        return result; // released: success false, no output
    }
    result.output = QString::fromUtf8(it->outputBuffer);
    result.truncated = it->truncated;
    if (it->status != TerminalStatus::Running) {
        result.exitStatus = it->exitCode;
        result.success = true;
    }
    return result;
}

void TerminalManager::waitForExit(const QString &terminalId, int timeoutMs, WaitCallback callback)
{
    auto it = m_terminals.constFind(terminalId);
    if (it == m_terminals.constEnd() || it->status != TerminalStatus::Running) {
        callback(currentResult(terminalId));
        return;
    }

    Waiter waiter{m_nextWaiterId++, terminalId, std::move(callback), nullptr};
    if (timeoutMs > 0) {
        waiter.timer = new QTimer(this);
        waiter.timer->setSingleShot(true);
        const quint64 id = waiter.id;
        connect(waiter.timer, &QTimer::timeout, this, [this, id]() { completeWaiter(id); });
        waiter.timer->start(timeoutMs);
    }
    m_waiters.append(std::move(waiter));
}

void TerminalManager::completeWaiter(quint64 waiterId)
{
    for (int i = 0; i < m_waiters.size(); ++i) {
        if (m_waiters.at(i).id == waiterId) {
            Waiter waiter = m_waiters.takeAt(i);
            if (waiter.timer) {
                waiter.timer->deleteLater();
            }
            waiter.callback(currentResult(waiter.terminalId));
            return;
        }
    }
}

void TerminalManager::completeWaiters(const QString &terminalId)
{
    // Callbacks may add or complete waiters, so take matching ones first.
    QList<Waiter> done;
    for (int i = m_waiters.size() - 1; i >= 0; --i) {
        if (m_waiters.at(i).terminalId == terminalId) {
            done.prepend(m_waiters.takeAt(i));
        }
    }
    const WaitResult result = currentResult(terminalId);
    for (const Waiter &waiter : std::as_const(done)) {
        if (waiter.timer) {
            waiter.timer->stop();
            waiter.timer->deleteLater();
        }
        waiter.callback(result);
    }
}

bool TerminalManager::killTerminal(const QString &terminalId)
{
    if (!m_terminals.contains(terminalId)) {
        qWarning() << "[TerminalManager] killTerminal: terminal not found:" << terminalId;
        return false;
    }

    auto &data = m_terminals[terminalId];

    if (data.status != TerminalStatus::Running) {
        // Already stopped
        return true;
    }

    qDebug() << "[TerminalManager] Killing terminal" << terminalId;

    if (data.process) {
        data.process->kill();
        data.process->waitForFinished(1000);
    }

    data.status = TerminalStatus::Killed;
    return true;
}

bool TerminalManager::releaseTerminal(const QString &terminalId)
{
    if (!m_terminals.contains(terminalId)) {
        qWarning() << "[TerminalManager] releaseTerminal: terminal not found:" << terminalId;
        return false;
    }

    qDebug() << "[TerminalManager] Releasing terminal" << terminalId;

    auto &data = m_terminals[terminalId];

    if (data.process) {
        // Disconnect signals before cleanup
        disconnect(data.process, nullptr, this, nullptr);

        if (data.status == TerminalStatus::Running) {
            data.process->kill();
            data.process->waitForFinished(1000);
        }
        data.process->deleteLater();
    }

    m_terminals.remove(terminalId);
    // Answer pending waits: the terminal is gone.
    completeWaiters(terminalId);
    return true;
}

bool TerminalManager::isValid(const QString &terminalId) const
{
    return m_terminals.contains(terminalId);
}

void TerminalManager::releaseAll()
{
    qDebug() << "[TerminalManager] Releasing all terminals (" << m_terminals.size() << "terminals)";

    QStringList ids = m_terminals.keys();
    for (const QString &id : ids) {
        releaseTerminal(id);
    }
}

void TerminalManager::setDefaultTerminalSize(int columns, int rows)
{
    m_defaultColumns = qBound(40, columns, 500);  // Reasonable bounds
    m_defaultRows = qBound(10, rows, 200);
    qDebug() << "[TerminalManager] Default terminal size set to" << m_defaultColumns << "x" << m_defaultRows;
}
