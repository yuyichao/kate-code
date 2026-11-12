#include "ACPSessionDeleter.h"

#include "ACPService.h"
#include "ACPSession.h"

#include <QJsonArray>
#include <QTimer>

namespace {
// Starting the agent can take a while (e.g. a Node.js agent on a cold cache).
constexpr int TimeoutMs = 60000;
}

ACPSessionDeleter::ACPSessionDeleter(const QString &executable, const QStringList &args,
                                     const QString &workingDir, const QStringList &sessionIds,
                                     QObject *parent)
    : QObject(parent)
    , m_service(new ACPService(this))
    , m_timeout(new QTimer(this))
    , m_workingDir(workingDir)
    , m_sessionIds(sessionIds)
{
    m_service->setExecutable(executable, args);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this]() {
        finish(QStringLiteral("the agent did not answer in time"));
    });
    connect(m_service, &ACPService::responseReceived, this, &ACPSessionDeleter::onResponse);
    connect(m_service, &ACPService::disconnected, this, [this]() {
        finish(QStringLiteral("the agent exited"));
    });
    // Requests from the agent (none are expected) get an error, so it does
    // not wait on them.
    connect(m_service, &ACPService::notificationReceived, this,
            [this](const QString &, const QJsonObject &, int requestId) {
                if (requestId >= 0) {
                    QJsonObject error;
                    error[QStringLiteral("code")] = -32601;
                    error[QStringLiteral("message")] = QStringLiteral("Method not supported");
                    m_service->sendResponse(requestId, QJsonObject(), error);
                }
            });
}

ACPSessionDeleter::~ACPSessionDeleter()
{
    // Stopping emits disconnected(); nothing is left to report by now.
    disconnect(m_service, nullptr, this, nullptr);
    m_service->stop();
}

void ACPSessionDeleter::start()
{
    m_timeout->start(TimeoutMs);
    if (!m_service->start(m_workingDir)) {
        finish(QStringLiteral("the agent could not be started"));
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

void ACPSessionDeleter::onResponse(int id, const QJsonObject &result, const QJsonObject &error)
{
    if (m_finished) {
        return;
    }
    if (id == m_initializeId) {
        m_initializeId = -1;
        if (!error.isEmpty()) {
            finish(describeACPError(error));
            return;
        }
        const QJsonObject sessionCapabilities = result[QStringLiteral("agentCapabilities")].toObject()
                                                    [QStringLiteral("sessionCapabilities")].toObject();
        if (!sessionCapabilities[QStringLiteral("delete")].isObject()) {
            finish(QStringLiteral("the agent does not support deleting sessions"));
            return;
        }
        for (const QString &sessionId : std::as_const(m_sessionIds)) {
            QJsonObject params;
            params[QStringLiteral("sessionId")] = sessionId;
            const int requestId = m_service->sendRequest(QStringLiteral("session/delete"), params);
            if (requestId < 0) {
                finish(QStringLiteral("the agent exited"));
                return;
            }
            m_pending.insert(requestId);
        }
        if (m_pending.isEmpty()) {
            finish(QString());
        }
        return;
    }
    if (!m_pending.remove(id)) {
        return;
    }
    if (error.isEmpty()) {
        ++m_deleted;
    } else {
        m_errors << describeACPError(error);
    }
    if (m_pending.isEmpty()) {
        m_errors.removeDuplicates();
        finish(m_errors.join(QStringLiteral("; ")));
    }
}

void ACPSessionDeleter::finish(const QString &error)
{
    if (m_finished) {
        return;
    }
    m_finished = true;
    m_timeout->stop();
    disconnect(m_service, nullptr, this, nullptr);
    m_service->stop();
    Q_EMIT finished(m_deleted, error);
    deleteLater();
}
