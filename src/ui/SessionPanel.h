#pragma once

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
        QString location; // e.g. "Claude Code: kate-code"
        QString status;   // e.g. "Connected"
        QString detail;   // e.g. "$0.42"
        QString iconName; // theme icon for the status
        bool active = false;
    };

    explicit SessionPanel(QWidget *parent = nullptr);

    void setEntries(const QList<Entry> &entries);

Q_SIGNALS:
    void newSessionRequested();
    void entryActivated(int index);

private:
    QListWidget *m_list;
    int m_entryCount = 0;
};
