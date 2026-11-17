#include "ACPSessionLister.h"

#include "ACPService.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

namespace
{
// Starting the agent can take a while (e.g. a Node.js agent on a cold cache).
constexpr int TimeoutMs = 60000;
// More pages are not worth the wait for a side-panel list.
constexpr int MaxPages = 10;

QString describeError(const QJsonObject &error)
{
    return error[QStringLiteral("message")].toString(QStringLiteral("error %1").arg(error[QStringLiteral("code")].toInt()));
}
}

ACPSessionLister::ACPSessionLister(const QString &executable, const QStringList &args, const QString &workingDir, const QString &cwdFilter,
                                   QObject *parent)
    : QObject(parent)
    , m_service(new ACPService(this))
    , m_timeout(new QTimer(this))
    , m_workingDir(workingDir)
    , m_cwdFilter(cwdFilter)
{
    m_service->setExecutable(executable, args);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this]() {
        finish(QStringLiteral("the agent did not answer in time"));
    });
    connect(m_service, &ACPService::responseReceived, this, &ACPSessionLister::onResponse);
    connect(m_service, &ACPService::remoteAgentMissing, this, [this]() {
        m_missing = true;
    });
    // The reason comes before the exit (disconnected()), except when the
    // agent does not start at all (start() fails).
    connect(m_service, &ACPService::errorOccurred, this, [this](const QString &message) {
        m_error = message;
    });
    connect(m_service, &ACPService::disconnected, this, [this](int exitCode) {
        m_exitCode = exitCode;
        finish(m_error.isEmpty() ? QStringLiteral("the agent exited") : m_error, m_missing);
    });
    // Requests from the agent (none are expected) get an error, so it does
    // not wait on them.
    connect(m_service, &ACPService::notificationReceived, this, [this](const QString &, const QJsonObject &, int requestId) {
        if (requestId >= 0) {
            QJsonObject error;
            error[QStringLiteral("code")] = -32601;
            error[QStringLiteral("message")] = QStringLiteral("Method not supported");
            m_service->sendResponse(requestId, QJsonObject(), error);
        }
    });
}

ACPSessionLister::~ACPSessionLister()
{
    disconnect(m_service, nullptr, this, nullptr);
    m_service->stop();
}

void ACPSessionLister::start()
{
    m_timeout->start(TimeoutMs);
    const bool started = m_service->start(m_workingDir);
    if (!started) {
        // Not installed here, mostly.
        finish(m_error.isEmpty() ? QStringLiteral("the agent could not be started") : m_error, true);
        return;
    }
    QJsonObject fs;
    fs[QStringLiteral("readTextFile")] = false;
    fs[QStringLiteral("writeTextFile")] = false;
    QJsonObject capabilities;
    capabilities[QStringLiteral("fs")] = fs;
    capabilities[QStringLiteral("terminal")] = false;
    QJsonObject params;
    params[QStringLiteral("protocolVersion")] = 1;
    params[QStringLiteral("clientCapabilities")] = capabilities;
    m_initializeId = m_service->sendRequest(QStringLiteral("initialize"), params);
    if (m_initializeId < 0) {
        finish(QStringLiteral("the agent could not be started"));
    }
}

void ACPSessionLister::requestPage(const QString &cursor)
{
    QJsonObject params;
    if (!m_cwdFilter.isEmpty()) {
        // A local path; a remote agent gets its host's (ACPService).
        params[QStringLiteral("cwd")] = m_cwdFilter;
    }
    if (!cursor.isEmpty()) {
        params[QStringLiteral("cursor")] = cursor;
    }
    m_listId = m_service->sendRequest(QStringLiteral("session/list"), params);
    if (m_listId < 0) {
        finish(QStringLiteral("the agent exited"));
    }
}

void ACPSessionLister::onResponse(int id, const QJsonObject &result, const QJsonObject &error)
{
    if (m_finished) {
        return;
    }
    if (id == m_initializeId) {
        m_initializeId = -1;
        if (!error.isEmpty()) {
            finish(describeError(error));
            return;
        }
        const QJsonObject sessionCapabilities =
            result[QStringLiteral("agentCapabilities")].toObject()[QStringLiteral("sessionCapabilities")].toObject();
        if (!sessionCapabilities[QStringLiteral("list")].isObject()) {
            finish(QStringLiteral("the agent cannot list its sessions"), true);
            return;
        }
        requestPage(QString());
        return;
    }
    if (id != m_listId) {
        return;
    }
    m_listId = -1;
    if (!error.isEmpty()) {
        finish(describeError(error));
        return;
    }
    for (const QJsonValue &value : result[QStringLiteral("sessions")].toArray()) {
        const QJsonObject object = value.toObject();
        Session session;
        session.sessionId = object[QStringLiteral("sessionId")].toString();
        session.cwd = object[QStringLiteral("cwd")].toString();
        session.title = object[QStringLiteral("title")].toString();
        session.updatedAt = QDateTime::fromString(object[QStringLiteral("updatedAt")].toString(), Qt::ISODateWithMs);
        if (!session.sessionId.isEmpty() && !session.cwd.isEmpty()) {
            m_sessions.append(session);
        }
    }
    const QString next = result[QStringLiteral("nextCursor")].toString();
    if (!next.isEmpty() && m_sessions.size() < m_maxSessions && ++m_pages < MaxPages) {
        requestPage(next);
        return;
    }
    finish(QString());
}

void ACPSessionLister::finish(const QString &error, bool unavailable)
{
    if (m_finished) {
        return;
    }
    m_finished = true;
    m_timeout->stop();
    disconnect(m_service, nullptr, this, nullptr);
    m_service->stop();
    if (m_sessions.size() > m_maxSessions) {
        m_sessions.resize(m_maxSessions);
    }
    Q_EMIT finished(m_sessions, error, unavailable);
    deleteLater();
}
