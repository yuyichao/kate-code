#pragma once

#include <QDateTime>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>

struct SessionEntry {
    QString id;
    QString name;
    QString note;
    QDateTime timestamp;
    QString title;      // the session's title (from the agent or renamed)
    QString providerId; // ACP provider the session ran with
    QString firstPrompt; // first line of the user's first prompt
    QString replacedBy;  // restored (context injection) as this session
    bool tracked = false; // recorded with prompt tracking (firstPrompt is reliable)
};

/**
 * SessionStore - Persists named session history per project root.
 *
 * Uses QSettings to store the mapping in ~/.config/kate-code.conf.
 * Each project root has a list of sessions (most recent first), capped at 20.
 * A project with no list yet falls back to the single session id stored by
 * older versions, which is carried over the next time the list is written.
 */
class SessionStore : public QObject
{
    Q_OBJECT

public:
    explicit SessionStore(QObject *parent = nullptr);
    ~SessionStore() override = default;

    // Add a session to the front of the list for a project root (capped at 20)
    void addSession(const QString &projectRoot, const QString &sessionId, const QString &name, const QString &note = QString());

    // Rename a session
    void renameSession(const QString &projectRoot, const QString &sessionId, const QString &newName);

    // Set or update the note for a session
    void setSessionNote(const QString &projectRoot, const QString &sessionId, const QString &note);

    // List all sessions for a project root, most recent first
    QList<SessionEntry> listSessions(const QString &projectRoot) const;

    // Check if any sessions exist for a project root
    bool hasSession(const QString &projectRoot) const;

    // Remove a specific session entry from the list
    void removeSession(const QString &projectRoot, const QString &sessionId);

    // Clear all sessions for a project root
    void clearSession(const QString &projectRoot);

    // Compatibility: get the most recent session ID (empty if none)
    QString getLastSession(const QString &projectRoot) const;

    // Compatibility: save a single session (adds to front of list with auto-name)
    void saveSession(const QString &projectRoot, const QString &sessionId);

    // Record a started session (moved to the front, keeping its title) and
    // the provider it runs with.
    void recordSession(const QString &projectRoot, const QString &sessionId, const QString &providerId);

    // Store the session's title; no-op for a session not in the list.
    void setSessionTitle(const QString &projectRoot, const QString &sessionId, const QString &title);

    // Store the user's first prompt (kept once set).
    void setFirstPrompt(const QString &projectRoot, const QString &sessionId, const QString &prompt);

    // A session restored into a new agent session by context injection: the
    // new one carries its first prompt, the old one is marked replaced.
    void markReplaced(const QString &projectRoot, const QString &oldSessionId, const QString &newSessionId);

    // The stored entry for a session (empty id if none).
    SessionEntry findSession(const QString &projectRoot, const QString &sessionId) const;

    // List all project roots that have saved sessions
    QStringList listAllProjectRoots() const;

private:
    QString normalizeKey(const QString &projectRoot) const;
    void writeAll(const QString &projectRoot, const QList<SessionEntry> &sessions);
    QString legacySessionId(const QString &key) const;

    QSettings m_settings;
    static constexpr int MaxSessions = 20;
};
