#pragma once

#include <QColor>
#include <QIcon>
#include <QWidget>

class QListWidget;
class QListWidgetItem;

/**
 * SessionPanel - Side panel listing the chat sessions of a Kate window.
 *
 * Chats themselves live in the editor area (one tab each). The list shows
 * the open chats (title, connection state, cost; activating one switches to
 * it) and, below a heading, the saved sessions of the current project that
 * can be resumed (activating one opens it in a new chat) or deleted (context
 * menu or Delete key). Below them, collapsed by default, the sessions the
 * agents keep that were started elsewhere (another editor, a terminal):
 * listed by the agents when expanded, opened in a new chat with
 * session/load. Buttons open a new chat and the session browser with
 * summaries.
 */
class SessionPanel : public QWidget
{
    Q_OBJECT

public:
    struct Entry {
        QString title;    // session title
        QString status;   // e.g. "Connected"
        QString detail;   // e.g. "$0.42"
        QIcon icon;         // provider logo
        QColor statusColor; // badge drawn on the logo
        bool active = false;
    };

    struct ResumableEntry {
        QString sessionId;
        QString projectRoot; // directory the session ran in
        QString transcriptPath;
        QString title;
        QString detail;  // e.g. last activity time
        QString toolTip;
        QIcon icon;      // provider logo
        QString providerId; // started elsewhere: the agent that lists it
    };

    explicit SessionPanel(QWidget *parent = nullptr);

    void setEntries(const QList<Entry> &entries);
    // Saved sessions of the current location (heading names it, toolTip
    // gives the full path).
    void setResumable(const QString &heading, const QString &headingToolTip,
                      const QList<ResumableEntry> &entries);
    // Sessions started elsewhere; status (when not empty) is shown instead
    // of an empty list or after the entries ("Asking the agents…", errors).
    void setElsewhere(const QList<ResumableEntry> &entries, const QString &status);
    bool elsewhereExpanded() const
    {
        return m_elsewhereExpanded;
    }

Q_SIGNALS:
    void newSessionRequested();
    void browseRequested();
    // Reload the saved sessions (e.g. changed by another Kate instance).
    void refreshRequested();
    // The resumable list shows all directories instead of the current one.
    void showAllDirectoriesChanged(bool showAll);
    void entryActivated(int index);
    void resumeRequested(const QString &sessionId, const QString &projectRoot);
    void deleteRequested(const QString &sessionId, const QString &projectRoot, const QString &transcriptPath);
    // The "started elsewhere" section was expanded (list them) or collapsed.
    void elsewhereExpandedChanged(bool expanded);
    // The refresh button: also ask the agents again.
    void reloadRequested();
    void resumeElsewhereRequested(const QString &sessionId, const QString &projectRoot, const QString &providerId);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    enum ItemKind { OpenItem = 1, ResumableItem, ElsewhereHeadingItem, ElsewhereItem };

    void rebuild();
    void addResumable();
    void addElsewhere();
    void toggleElsewhere();
    QListWidgetItem *addHeading(const QString &text, const QString &toolTip, bool divider);
    void addHint(const QString &text);
    void showContextMenu(const QPoint &pos);
    void requestDelete(QListWidgetItem *item);
    QIcon withStatusBadge(const QIcon &icon, const QColor &color) const;

    QListWidget *m_list;
    QList<Entry> m_entries;
    QList<ResumableEntry> m_resumable;
    QString m_resumableHeading;
    QString m_resumableHeadingToolTip;
    QList<ResumableEntry> m_elsewhere;
    QString m_elsewhereStatus;
    bool m_elsewhereExpanded = false;
};
