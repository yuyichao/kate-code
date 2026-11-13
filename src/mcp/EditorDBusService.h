/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2025 Kate Code contributors
*/

#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>

class EditorSession;

namespace KTextEditor {
class Document;
class MainWindow;
}

class EditorDBusService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.katecode.Editor")

public:
    explicit EditorDBusService(QObject *parent = nullptr);
    ~EditorDBusService() override;

    // Whether the open document's file changed on disk since its buffer was
    // loaded or last saved: Kate flagged it, or (before Kate notices) the
    // file no longer matches the document's checksum. Saving the buffer
    // then would make KTextEditor ask with a modal dialog.
    static bool documentChangedOnDisk(KTextEditor::Document *doc, const QString &filePath);

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

    // Read a file: the editor buffer when it is open in Kate (and has not
    // been changed on disk behind an unmodified buffer), else the file on
    // disk. Returns "ERROR: ..." on failure.
    QString readDocument(const QString &filePath);

    // Edit a file by replacing old_text with new_text, and save it.
    // Returns "OK" on success or "ERROR: ..." on failure.
    //
    // None of the document operations opens, activates or creates views, or
    // can show a dialog: a file that is not open is changed on disk; an open
    // one in its buffer (so the edit can be undone there) and then saved, or
    // on disk and then reloaded if it changed on disk while its buffer is
    // unmodified. A buffer with unsaved changes whose file also changed on
    // disk is refused.
    QString editDocument(const QString &filePath, const QString &oldText, const QString &newText);

    // Write content to a file (creates or overwrites), see editDocument().
    // Returns "OK" on success or "ERROR: ..." on failure.
    QString writeDocument(const QString &filePath, const QString &content);

    // Questions belong to a chat: only a chat's own endpoint (EditorSession)
    // can ask. On this shared object it always fails.
    QString askUserQuestion(const QString &questionsJson);

    // Sends text to Kate's embedded terminal without executing it (no Enter).
    // Returns "OK" on success or "ERROR: ..." on failure.
    QString pasteToTerminal(const QString &text);

private:
    // Replace the whole text (oldText empty) or the unique occurrence of
    // oldText, wherever the file lives (see editDocument()).
    QString changeDocument(const QString &filePath, const QString &oldText, const QString &newText, bool whole);
    KTextEditor::Document *findDocument(const QString &filePath) const;
    // Whether Kate has flagged the document as changed on disk (its
    // checksum is then already the one of the new file).
    bool flaggedModifiedOnDisk(KTextEditor::Document *doc) const;
    void trackDocument(KTextEditor::Document *doc);

    int m_nextSessionId = 0;
    QHash<KTextEditor::Document *, bool> m_modifiedOnDisk;
};
