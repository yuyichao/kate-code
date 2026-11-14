#pragma once

#include "RemoteLocation.h"

#include <QHash>
#include <QPointer>
#include <QStringList>
#include <QWidget>

#include <optional>

namespace KParts
{
class ReadOnlyPart;
}

class QKeyEvent;

/**
 * A shell on a remote host, for a tool view next to Kate's own terminal:
 * Konsole's terminal part running ssh (KateRemote::sshTerminalCommand()).
 * A failed or dropped connection (ssh exits with 255) stays on screen and
 * Enter reconnects; finished() when the remote shell ends.
 */
class RemoteTerminal : public QWidget
{
    Q_OBJECT

public:
    explicit RemoteTerminal(const QStringList &command, QWidget *parent = nullptr);
    ~RemoteTerminal() override;

    // Loads Konsole's part and starts the command; false (with *error) if
    // the part is not available.
    bool start(QString *error);

Q_SIGNALS:
    // The program ended (the part is gone); queued.
    void finished();

private Q_SLOTS:
    // Konsole's part asks before keys reach it: it takes all of them while
    // focused, like Kate's own terminal.
    void overrideShortcut(QKeyEvent *event, bool &override);

private:
    QStringList m_command;
    QPointer<KParts::ReadOnlyPart> m_part;
};

class QLabel;
class QStackedWidget;

/**
 * The "Remote Terminal" tool view, next to Kate's terminal: one shell per
 * remote folder on an ssh host, showing the one of the current folder.
 * Shown, it starts that folder's shell if there is none; a shell that ends
 * is removed (currentEnded() for the shown one).
 */
class RemoteTerminalPanel : public QWidget
{
    Q_OBJECT

public:
    explicit RemoteTerminalPanel(QWidget *parent = nullptr);

    // The remote folder of the active project or document, if on an ssh
    // host (sftp or fish).
    void setLocation(const std::optional<KateRemote::Location> &location);

Q_SIGNALS:
    void currentEnded();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void startTerminal();
    void updatePage();

    QStackedWidget *m_stack;
    QLabel *m_label;
    std::optional<KateRemote::Location> m_location;
    QString m_error; // starting the current location's terminal failed
    QHash<QString, RemoteTerminal *> m_terminals; // by folder URL
};
