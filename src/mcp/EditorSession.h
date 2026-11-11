/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2025 Kate Code contributors
*/

#pragma once

#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <functional>

class EditorDBusService;
class QTimer;

namespace KTextEditor {
class MainWindow;
}

/**
 * EditorSession - The editor DBus endpoint of one chat.
 *
 * Each chat gets its own object (/KateCode/Session/sN) implementing the
 * org.kde.katecode.Editor interface; its kate-mcp-server is told the path via
 * KATECODE_DBUS_PATH. Calls on it therefore belong to that chat alone, so
 * questions and edit notifications reach the right chat when several agents
 * run in one Kate process.
 *
 * askUserQuestion() replies asynchronously (delayed DBus reply) instead of
 * blocking in a nested event loop, so several questions can be pending and
 * answered in any order.
 */
class EditorSession : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.katecode.Editor")

public:
    using MainWindowProvider = std::function<KTextEditor::MainWindow *()>;

    EditorSession(EditorDBusService *service, const QString &objectPath, QObject *parent);
    ~EditorSession() override;

    QString objectPath() const { return m_objectPath; }
    bool isRegistered() const { return m_registered; }

    // The window used for window-specific tools (Kate's terminal).
    void setMainWindowProvider(MainWindowProvider provider) { m_mainWindowProvider = std::move(provider); }

    // Answer a pending question (from the chat UI).
    void provideQuestionResponse(const QString &requestId, const QString &responseJson);
    // Fail every pending question, e.g. when the agent session ends or the
    // chat view is reset; emits questionCancelled for each.
    void cancelPendingQuestions(const QString &reason);

public Q_SLOTS:
    // DBus methods (org.kde.katecode.Editor). Only DBus methods are slots
    // here: every public slot is exported.
    QStringList listDocuments();
    QString readDocument(const QString &filePath);
    QString editDocument(const QString &filePath, const QString &oldText, const QString &newText);
    QString writeDocument(const QString &filePath, const QString &content);
    QString askUserQuestion(const QString &questionsJson);
    QString pasteToTerminal(const QString &text);

Q_SIGNALS:
    void editApplied(const QString &filePath, const QString &oldText, const QString &newText);
    void questionRequested(const QString &requestId, const QString &questionsJson);
    void questionCancelled(const QString &requestId);

private:
    friend class EditorDBusService;
    void setRegistered(bool registered) { m_registered = registered; }
    void finishQuestion(const QString &requestId, const QString &reply, bool notifyUi);

    struct PendingQuestion {
        QDBusConnection connection;
        QDBusMessage call;
        QTimer *timer;
    };

    QPointer<EditorDBusService> m_service;
    QString m_objectPath;
    bool m_registered = false;
    bool m_destroying = false;
    MainWindowProvider m_mainWindowProvider;
    QHash<QString, PendingQuestion> m_pendingQuestions;
};
