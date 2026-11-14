#include "RemoteTerminal.h"

#include <KLocalizedString>
#include <KParts/ReadOnlyPart>
#include <KPluginFactory>
#include <KPluginMetaData>
#include <kde_terminal_interface.h>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QLabel>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QShowEvent>
#include <QStackedWidget>
#include <QVBoxLayout>

RemoteTerminal::RemoteTerminal(const QStringList &command, const QString &authority, const QString &askpass, QWidget *parent)
    : QWidget(parent)
    , m_command(command)
    , m_authority(authority)
    , m_askpass(askpass)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
}

RemoteTerminal::~RemoteTerminal()
{
    // Deleted with us: not a finished program.
    if (m_part) {
        disconnect(m_part, nullptr, this, nullptr);
    }
}

bool RemoteTerminal::start(QString *error)
{
    const auto result = KPluginFactory::instantiatePlugin<KParts::ReadOnlyPart>(KPluginMetaData(QStringLiteral("kf6/parts/konsolepart")), this);
    auto *terminal = result.plugin ? qobject_cast<TerminalInterface *>(result.plugin) : nullptr;
    if (!terminal) {
        delete result.plugin;
        *error = i18n("The terminal needs Konsole's terminal part (install Konsole): %1", result.errorText);
        return false;
    }
    m_part = result.plugin;
    layout()->addWidget(m_part->widget());
    setFocusProxy(m_part->widget());
    connect(m_part, SIGNAL(overrideShortcut(QKeyEvent *, bool &)), this, SLOT(overrideShortcut(QKeyEvent *, bool &)));
    // The part deletes itself when the program exits.
    connect(m_part, &QObject::destroyed, this, &RemoteTerminal::finished, Qt::QueuedConnection);

    // Konsole expands $VARIABLES in a program's arguments (the remote
    // command's "$SHELL" would become the local one): the command goes in a
    // script, which removes itself once started.
    // ssh exits with 255 when it fails or the connection drops: keep its
    // message on screen and reconnect on Enter. So does a failure within
    // seconds (ssh not found, a remote refusing the command). Any other
    // status is the remote shell's own exit.
    QStringList quoted;
    for (const QString &arg : std::as_const(m_command)) {
        quoted << KateRemote::shellQuote(arg);
    }
    // Login passwords from KDE's password cache, like the mounts (the
    // helper's terminal mode asks everything else here); the state file
    // marks a cached answer within one connection attempt.
    QString askpass;
    if (!m_askpass.isEmpty()) {
        askpass = QStringLiteral(
                      "state=\"$0.askpass\"\n"
                      "trap 'rm -f -- \"$state\"' EXIT\n"
                      "trap 'exit 129' HUP\n"
                      "trap 'exit 143' TERM\n"
                      "export SSH_ASKPASS=%1 SSH_ASKPASS_REQUIRE=force KATE_REMOTE_ASKPASS_TTY=1\n"
                      "export KATE_REMOTE_AUTHORITY=%2 KATE_REMOTE_ASKPASS_STATE=\"$state\"\n")
                      .arg(KateRemote::shellQuote(m_askpass), KateRemote::shellQuote(m_authority));
    }
    const QString script = QStringLiteral(
                               "rm -f -- \"$0\"\n"
                               "%3"
                               "while :; do\n"
                               "    [ -z \"$state\" ] || rm -f -- \"$state\"\n"
                               "    start=$(date +%s)\n"
                               "    %1; status=$?\n"
                               "    if [ \"$status\" -ne 255 ]; then\n"
                               "        [ \"$status\" -ne 0 ] && [ $(($(date +%s) - start)) -lt 5 ] || exit \"$status\"\n"
                               "    fi\n"
                               "    printf '\\n%s ' %2\n"
                               "    read -r _ || exit \"$status\"\n"
                               "done\n")
                               .arg(quoted.join(QLatin1Char(' ')),
                                    KateRemote::shellQuote(i18n("[Connection closed. Press Enter to reconnect, Ctrl+D to close.]")),
                                    askpass);
    QTemporaryFile file(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/kate-remote-terminal-XXXXXX.sh"));
    file.setAutoRemove(false);
    if (!file.open() || file.write(script.toLocal8Bit()) < 0 || !file.flush()) {
        file.remove();
        *error = i18n("Cannot write the terminal's script: %1", file.errorString());
        return false;
    }
    // Konsole's part takes the whole argv (program name first).
    terminal->startProgram(QStringLiteral("/bin/sh"), {QStringLiteral("sh"), file.fileName()});
    return true;
}

void RemoteTerminal::overrideShortcut(QKeyEvent *, bool &override)
{
    override = true;
}

// kate-remote-fs-askpass, as the daemon uses it (empty when KdePasswords
// is off or the daemon is too old to say).
static QString askpassProgram()
{
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KateRemoteFS"),
                                                       QStringLiteral("/org/kde/KateRemoteFS"),
                                                       QStringLiteral("org.kde.KateRemoteFS"),
                                                       QStringLiteral("askpassProgram"));
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 3000);
    return reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty() ? reply.arguments().constFirst().toString() : QString();
}

static QString locationKey(const KateRemote::Location &location)
{
    return location.url().toString();
}

RemoteTerminalPanel::RemoteTerminalPanel(QWidget *parent)
    : QWidget(parent)
    , m_stack(new QStackedWidget(this))
    , m_label(new QLabel(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_stack);
    m_label->setAlignment(Qt::AlignCenter);
    m_label->setWordWrap(true);
    m_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_stack->addWidget(m_label);
    updatePage();
}

void RemoteTerminalPanel::setLocation(const std::optional<KateRemote::Location> &location)
{
    if (location == m_location) {
        return;
    }
    m_location = location;
    m_error.clear();
    if (isVisible()) {
        startTerminal();
    }
    updatePage();
}

void RemoteTerminalPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Not when the window is restored: only when the tab is shown.
    if (event->spontaneous()) {
        return;
    }
    startTerminal();
    updatePage();
    if (m_stack->currentWidget() != m_label) {
        m_stack->currentWidget()->setFocus();
    }
}

void RemoteTerminalPanel::startTerminal()
{
    if (!m_location || m_terminals.contains(locationKey(*m_location))) {
        return;
    }
    const QStringList command = KateRemote::sshTerminalCommand(*m_location, KateRemote::configuredSshCommand());
    if (command.isEmpty()) {
        m_error = i18n("ssh cannot log in to %1.", m_location->authority);
        return;
    }
    auto *terminal = new RemoteTerminal(command, m_location->authority, askpassProgram(), m_stack);
    if (!terminal->start(&m_error)) {
        delete terminal;
        return;
    }
    const QString key = locationKey(*m_location);
    terminal->setToolTip(m_location->url().toDisplayString());
    m_stack->addWidget(terminal);
    m_terminals.insert(key, terminal);
    connect(terminal, &RemoteTerminal::finished, this, [this, key, terminal] {
        m_terminals.remove(key);
        const bool current = m_stack->currentWidget() == terminal;
        terminal->deleteLater();
        if (current) {
            m_stack->setCurrentWidget(m_label);
            updatePage();
            Q_EMIT currentEnded();
        }
    });
    terminal->setFocus();
}

void RemoteTerminalPanel::updatePage()
{
    RemoteTerminal *terminal = m_location ? m_terminals.value(locationKey(*m_location)) : nullptr;
    if (terminal) {
        m_stack->setCurrentWidget(terminal);
        setFocusProxy(terminal);
        return;
    }
    setFocusProxy(nullptr);
    if (!m_location) {
        m_label->setText(i18n("The current project is not a remote folder on an ssh host (sftp:// or fish://).\n"
                              "Switch to one for a terminal on its computer here."));
    } else if (!m_error.isEmpty()) {
        m_label->setText(m_error);
    } else {
        m_label->setText(i18n("Terminal on %1 (opens when this panel is shown).", m_location->url().toDisplayString()));
    }
    m_stack->setCurrentWidget(m_label);
}
