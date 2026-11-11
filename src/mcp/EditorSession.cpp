/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2025 Kate Code contributors
*/

#include "EditorSession.h"
#include "EditorDBusService.h"

#include <QCoreApplication>
#include <QDebug>
#include <QTimer>

namespace
{
// Shorter than kate-mcp-server's 5 minute DBus timeout, so the tool gets
// Kate's answer (and the prompt is removed) rather than a transport timeout.
constexpr int QuestionTimeoutMs = 4 * 60 * 1000;

int s_nextQuestionId = 0;
}

EditorSession::EditorSession(EditorDBusService *service, const QString &objectPath, QObject *parent)
    : QObject(parent)
    , m_service(service)
    , m_objectPath(objectPath)
{
}

EditorSession::~EditorSession()
{
    m_destroying = true;
    // Answer outstanding calls so the agent's tool does not hang. No UI
    // signals while the owning chat is being destroyed.
    cancelPendingQuestions(QStringLiteral("the Kate Code chat was closed"));
    if (m_registered) {
        QDBusConnection::sessionBus().unregisterObject(m_objectPath);
    }
}

QStringList EditorSession::listDocuments()
{
    return m_service ? m_service->listDocuments() : QStringList();
}

QString EditorSession::readDocument(const QString &filePath)
{
    return m_service ? m_service->readDocument(filePath) : QStringLiteral("ERROR: Kate Code is shutting down");
}

QString EditorSession::editDocument(const QString &filePath, const QString &oldText, const QString &newText)
{
    if (!m_service) {
        return QStringLiteral("ERROR: Kate Code is shutting down");
    }
    const QString result = m_service->editDocument(filePath, oldText, newText);
    if (result == QStringLiteral("OK")) {
        Q_EMIT editApplied(filePath, oldText, newText);
    }
    return result;
}

QString EditorSession::writeDocument(const QString &filePath, const QString &content)
{
    return m_service ? m_service->writeDocument(filePath, content) : QStringLiteral("ERROR: Kate Code is shutting down");
}

QString EditorSession::pasteToTerminal(const QString &text)
{
    KTextEditor::MainWindow *window = m_mainWindowProvider ? m_mainWindowProvider() : nullptr;
    return EditorDBusService::pasteToTerminalIn(text, window);
}

QString EditorSession::askUserQuestion(const QString &questionsJson)
{
    if (!calledFromDBus()) {
        return QStringLiteral("ERROR: askUserQuestion must be called over DBus");
    }

    const QString requestId = QStringLiteral("q_%1_%2")
                                  .arg(QCoreApplication::applicationPid())
                                  .arg(s_nextQuestionId++);

    // Reply later, from provideQuestionResponse() or the timeout, without
    // blocking this call in a nested event loop.
    setDelayedReply(true);
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, requestId]() {
        finishQuestion(requestId, QStringLiteral("ERROR: Question timeout or cancelled"), true);
    });
    timer->start(QuestionTimeoutMs);
    m_pendingQuestions.insert(requestId, PendingQuestion{connection(), message(), timer});

    qDebug() << "[EditorSession]" << m_objectPath << "question" << requestId;
    Q_EMIT questionRequested(requestId, questionsJson);
    return QString(); // ignored: the reply is delayed
}

void EditorSession::provideQuestionResponse(const QString &requestId, const QString &responseJson)
{
    if (!m_pendingQuestions.contains(requestId)) {
        qWarning() << "[EditorSession] No pending question" << requestId;
        return;
    }
    finishQuestion(requestId, responseJson, false);
}

void EditorSession::cancelPendingQuestions(const QString &reason)
{
    const QStringList ids = m_pendingQuestions.keys();
    for (const QString &id : ids) {
        finishQuestion(id, QStringLiteral("ERROR: Question cancelled: %1").arg(reason), true);
    }
}

void EditorSession::finishQuestion(const QString &requestId, const QString &reply, bool notifyUi)
{
    auto it = m_pendingQuestions.find(requestId);
    if (it == m_pendingQuestions.end()) {
        return;
    }
    PendingQuestion pending = it.value();
    m_pendingQuestions.erase(it);

    pending.timer->stop();
    pending.timer->deleteLater();
    pending.connection.send(pending.call.createReply(reply));

    if (notifyUi && !m_destroying) {
        Q_EMIT questionCancelled(requestId);
    }
}
