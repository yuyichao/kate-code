#pragma once

#include <QColor>
#include <QIcon>
#include <QWidget>

class QListWidget;
class QListWidgetItem;

/**
 * SessionPanel - Side panel listing the chat sessions open in a Kate window.
 *
 * Chats themselves live in the editor area (one tab each); this panel shows
 * each one's title, connection state and usage, switches to a chat when its
 * entry is activated, and offers a button to open a new chat.
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

    explicit SessionPanel(QWidget *parent = nullptr);

    void setEntries(const QList<Entry> &entries);

Q_SIGNALS:
    void newSessionRequested();
    void entryActivated(int index);

private:
    QIcon withStatusBadge(const QIcon &icon, const QColor &color) const;

    QListWidget *m_list;
    int m_entryCount = 0;
};
