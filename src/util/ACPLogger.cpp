#include "ACPLogger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>

ACPLogger::ACPLogger(QObject *parent)
    : QObject(parent)
{
}

ACPLogger::~ACPLogger()
{
    endSession();
}

void ACPLogger::configure(bool enabled, const QString &baseDir, const QString &subDir)
{
    m_baseDir = baseDir;
    m_subDir = subDir;

    // If logging was switched off, close any open file straight away.
    if (m_enabled && !enabled) {
        endSession();
    }
    m_enabled = enabled;
}

void ACPLogger::logPayload(const QString &direction, const QString &json)
{
    if (!m_enabled) {
        return;
    }

    if (!m_file.isOpen()) {
        openNewFile();
        if (!m_file.isOpen()) {
            return;  // Could not open; give up silently to avoid log spam.
        }
    }

    // One JSON object per line (JSONL). The payload is already valid JSON text,
    // so embed it verbatim rather than re-encoding.
    m_stream << QStringLiteral("{\"time\":\"%1\",\"dir\":\"%2\",\"msg\":%3}\n")
                    .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
                         direction, json);
    m_stream.flush();
}

void ACPLogger::endSession()
{
    if (m_file.isOpen()) {
        m_stream.flush();
        m_file.close();
    }
}

void ACPLogger::openNewFile()
{
    const QString dirPath = m_baseDir + QStringLiteral("/") + m_subDir;
    QDir dir(dirPath);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        qWarning() << "[ACPLogger] Could not create log directory:" << dirPath;
        return;
    }

    // Named by connection time, since the ACP session id is not yet known when
    // the first initialize payloads flow.
    // Several sessions (and Kate processes) can start in the same second, so
    // add the process id and a per-process counter.
    static int s_sequence = 0;
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss"));
    const QString filePath = dirPath + QStringLiteral("/acp-") + stamp
                             + QStringLiteral("-p%1-%2").arg(QCoreApplication::applicationPid()).arg(s_sequence++)
                             + QStringLiteral(".json");

    m_file.setFileName(filePath);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        qWarning() << "[ACPLogger] Could not open log file:" << filePath;
        return;
    }
    m_stream.setDevice(&m_file);
    qDebug() << "[ACPLogger] Logging ACP session to:" << filePath;
}
