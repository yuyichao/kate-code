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
 * menu or Delete key). Buttons open a new chat and the session browser with
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
    };

    explicit SessionPanel(QWidget *parent = nullptr);

    void setEntries(const QList<Entry> &entries);
    // Saved sessions of the current location (heading names it, toolTip
    // gives the full path).
    void setResumable(const QString &heading, const QString &headingToolTip,
                      const QList<ResumableEntry> &entries);

Q_SIGNALS:
    void newSessionRequested();
    void browseRequested();
    // The resumable list shows all directories instead of the current one.
    void showAllDirectoriesChanged(bool showAll);
    void entryActivated(int index);
    void resumeRequested(const QString &sessionId, const QString &projectRoot);
    void deleteRequested(const QString &sessionId, const QString &projectRoot, const QString &transcriptPath);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum ItemKind { OpenItem = 1, ResumableItem };

    void rebuild();
    void addResumable();
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
};
