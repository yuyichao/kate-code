/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2025 Kate Code contributors
*/

#pragma once

#include <QObject>
#include <QStringList>

class EditorSession;

namespace KTextEditor {
class MainWindow;
}

class EditorDBusService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.katecode.Editor")

public:
    explicit EditorDBusService(QObject *parent = nullptr);

    // Register this service on the session bus.
    // Returns true on success.
    bool registerOnBus();

    // Create the DBus endpoint of one chat (/KateCode/Session/sN), owned by
    // parent. Check EditorSession::isRegistered() for success.
    EditorSession *createSession(QObject *parent);

    // Kate's embedded terminal of window (or the active window when null).
    static QString pasteToTerminalIn(const QString &text, KTextEditor::MainWindow *window);

public Q_SLOTS:
    QStringList listDocuments();

    // Read a document's content. Returns the text content.
    // If the document is not open, returns an error string starting with "ERROR:".
    QString readDocument(const QString &filePath);

    // Edit a document by replacing old_text with new_text.
    // Returns "OK" on success or "ERROR: ..." on failure.
    QString editDocument(const QString &filePath, const QString &oldText, const QString &newText);

    // Write content to a document (creates or overwrites).
    // Returns "OK" on success or "ERROR: ..." on failure.
    QString writeDocument(const QString &filePath, const QString &content);

    // Questions belong to a chat: only a chat's own endpoint (EditorSession)
    // can ask. On this shared object it always fails.
    QString askUserQuestion(const QString &questionsJson);

    // Sends text to Kate's embedded terminal without executing it (no Enter).
    // Returns "OK" on success or "ERROR: ..." on failure.
    QString pasteToTerminal(const QString &text);

private:
    int m_nextSessionId = 0;
};
