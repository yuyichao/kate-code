#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QStringList>

class ACPService;
class QTimer;

/**
 * ACPSessionLister - Lists the sessions an agent keeps (`session/list`),
 * e.g. Claude Code sessions started in a terminal, Zed or VS Code.
 *
 * Starts the provider's agent on its own (no chat; on the host for a remote
 * folder, like chats), sends `initialize`, then `session/list` (all pages,
 * up to a limit) when the agent advertises `sessionCapabilities.list`, and
 * stops the agent. Emits finished() once and deletes itself afterwards.
 * Paths come back as local paths (ACPService translates a remote agent's).
 */
class ACPSessionLister : public QObject
{
    Q_OBJECT

public:
    struct Session {
        QString sessionId;
        QString cwd;
        QString title;
        QDateTime updatedAt;
    };

    // workingDir: where the agent runs (a remote folder: on its host);
    // cwdFilter: only sessions of that directory (empty: all).
    ACPSessionLister(const QString &executable, const QStringList &args, const QString &workingDir, const QString &cwdFilter,
                     QObject *parent = nullptr);
    ~ACPSessionLister() override;

    void setMaxSessions(int max)
    {
        m_maxSessions = max;
    }
    void start();
    // The agent's exit code once it exited on its own (-1 otherwise); 255:
    // ssh failed (a remote folder).
    int exitCode() const
    {
        return m_exitCode;
    }

Q_SIGNALS:
    // error: why the list is missing or incomplete (empty when complete);
    // unavailable: the agent is not installed (here or on the host) or
    // cannot list sessions, which is not worth reporting.
    void finished(const QList<ACPSessionLister::Session> &sessions, const QString &error, bool unavailable);

private:
    void onResponse(int id, const QJsonObject &result, const QJsonObject &error);
    void requestPage(const QString &cursor);
    void finish(const QString &error, bool unavailable = false);

    ACPService *m_service;
    QTimer *m_timeout;
    QString m_workingDir;
    QString m_cwdFilter;
    int m_maxSessions = 200;
    int m_initializeId = -1;
    int m_listId = -1;
    int m_pages = 0;
    QList<Session> m_sessions;
    bool m_finished = false;
    bool m_missing = false; // a remote agent exited 127
    QString m_error; // ACPService's reason
    int m_exitCode = -1;
};
