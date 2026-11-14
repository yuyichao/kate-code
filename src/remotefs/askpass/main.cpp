// kate-remote-fs-askpass: SSH_ASKPASS for the sshfs mounts of kate-remote-fs.
//
// Login password prompts are answered from KDE's password cache
// (kpasswdserver), where KIO's sftp worker keeps the passwords it logged in
// with, so a host browsed with KIO (e.g. in the Open Remote Folder dialog)
// is mounted without asking again. Otherwise KDE's login dialog asks, and the
// answer is cached for KIO as well. Every other prompt (key passphrases, host
// key confirmation, one-time codes) goes to the fallback askpass
// (ksshaskpass).
//
// Environment, set by kate-remote-fs:
//   KATE_REMOTE_AUTHORITY        the host as mounted ([user@]host[:port])
//   KATE_REMOTE_FALLBACK_ASKPASS askpass program for other prompts
//   KATE_REMOTE_ASKPASS_STATE    file marking that this mount attempt was
//                                answered already: asked again means the
//                                password was wrong, so the cache is skipped
//   KATE_REMOTE_ASKPASS_NO_DIALOG  never show the dialog (tests, headless)
//
// Terminal mode (KATE_REMOTE_ASKPASS_TTY set, by the Remote Terminal, whose
// ssh runs in a terminal): the login password still comes from the cache,
// silently; everything else (no cached password, a refused one, host keys,
// passphrases, one-time codes) is asked on the terminal (/dev/tty), as ssh
// itself would, and the cache is left alone.

#include "RemoteLocation.h"

#include <KIO/AuthInfo>
#include <KLocalizedString>
#include <KPasswdServerClient>

#include <QCoreApplication>
#include <QFile>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>

namespace
{
int runFallback(const QString &prompt)
{
    const QByteArray fallback = qgetenv("KATE_REMOTE_FALLBACK_ASKPASS");
    if (fallback.isEmpty()) {
        return 1;
    }
    const QByteArray promptBytes = prompt.toLocal8Bit();
    execl(fallback.constData(), fallback.constData(), promptBytes.constData(), static_cast<char *>(nullptr));
    perror("kate-remote-fs-askpass: cannot run the fallback askpass");
    return 1;
}

// The terminal while its echo is off, restored when interrupted.
int ttyFd = -1;
termios ttySaved;

void restoreTty(int)
{
    tcsetattr(ttyFd, TCSAFLUSH, &ttySaved);
    _exit(1);
}

// Asks on the controlling terminal, like ssh: without echo, except for
// yes/no questions (host keys).
int askOnTerminal(const QString &prompt, const QString &note = {})
{
    const int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return 1;
    }
    if (!note.isEmpty()) {
        const QByteArray text = note.toLocal8Bit() + '\n';
        (void)!write(fd, text.constData(), text.size());
    }
    const bool echo = prompt.contains(QLatin1String("yes/no"));
    bool restore = false;
    if (!echo && tcgetattr(fd, &ttySaved) == 0) {
        ttyFd = fd;
        struct sigaction action = {};
        action.sa_handler = restoreTty;
        for (int signal : {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP}) {
            sigaction(signal, &action, nullptr);
        }
        termios silent = ttySaved;
        silent.c_lflag &= ~ECHO;
        restore = tcsetattr(fd, TCSAFLUSH, &silent) == 0;
    }
    const QByteArray promptBytes = prompt.toLocal8Bit();
    (void)!write(fd, promptBytes.constData(), promptBytes.size());
    QByteArray line;
    char c;
    bool complete = false;
    while (read(fd, &c, 1) == 1) {
        if (c == '\n' || c == '\r') {
            complete = true;
            break;
        }
        line.append(c);
    }
    if (restore) {
        tcsetattr(fd, TCSAFLUSH, &ttySaved);
        (void)!write(fd, "\n", 1);
    }
    close(fd);
    if (!complete && line.isEmpty()) {
        return 1; // end of input
    }
    line.append('\n');
    fwrite(line.constData(), 1, line.size(), stdout);
    return 0;
}

// KIO's entry for the host, if it is for the user ssh logs in as.
bool cachedPassword(KPasswdServerClient &client, const KateRemote::SshTarget &target, const QString &user, QString *password)
{
    // KIO files it under the URL's user, or with no user when the URL had
    // none (the username then only in the entry, e.g. typed into its dialog).
    QList<KateRemote::SshTarget> keys{target};
    if (!target.user.isEmpty()) {
        KateRemote::SshTarget withoutUser = target;
        withoutUser.user.clear();
        keys << withoutUser;
    }
    for (const auto &key : std::as_const(keys)) {
        KIO::AuthInfo info;
        info.url = KateRemote::kioAuthUrl(key);
        info.username = user;
        if (client.checkAuthInfo(&info, 0, 0) && !info.password.isEmpty() && (user.isEmpty() || info.username == user)) {
            *password = info.password;
            return true;
        }
    }
    return false;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    KLocalizedString::setApplicationDomain("katecode");
    const QString prompt = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();

    const bool terminal = qEnvironmentVariableIsSet("KATE_REMOTE_ASKPASS_TTY");
    const auto target = KateRemote::parseSshAuthority(QString::fromLocal8Bit(qgetenv("KATE_REMOTE_AUTHORITY")));
    if (!target || !KateRemote::isLoginPasswordPrompt(prompt)) {
        return terminal ? askOnTerminal(prompt) : runFallback(prompt);
    }
    // The user ssh logs in as: from the host as mounted, else from the prompt.
    QString user = target->user;
    if (user.isEmpty()) {
        user = KateRemote::userFromPasswordPrompt(prompt);
    }

    const QString stateFile = QString::fromLocal8Bit(qgetenv("KATE_REMOTE_ASKPASS_STATE"));
    const bool retry = !stateFile.isEmpty() && QFile::exists(stateFile);

    KPasswdServerClient client;
    QString password;
    if (retry || !cachedPassword(client, *target, user, &password)) {
        if (terminal) {
            // The mounts' login (KIO's cache) is not changed from here.
            return askOnTerminal(prompt, retry ? i18n("[The password saved in KDE's password cache was not accepted.]") : QString());
        }
        if (retry) {
            // The cached password was wrong (for KIO as well). Drop it: with
            // an entry cached, kpasswdserver answers a query from a new
            // client with that entry instead of asking.
            client.removeAuthInfo(target->host, QStringLiteral("sftp"), user);
        }
        if (qEnvironmentVariableIsSet("KATE_REMOTE_ASKPASS_NO_DIALOG")) {
            return 1;
        }
        KIO::AuthInfo info;
        info.url = KateRemote::kioAuthUrl(*target);
        info.username = user;
        info.readOnly = !user.isEmpty(); // ssh has chosen the user already
        info.caption = i18n("SFTP Login");
        info.prompt = prompt.trimmed();
        info.comment = info.url.toDisplayString();
        info.commentLabel = i18n("Site:");
        info.keepPassword = true;
        const int result = client.queryAuthInfo(&info, retry ? i18n("Login failed. Please try again.") : QString(), 0, 0);
        if (result != 0 || !info.isModified() || info.password.isEmpty()) {
            return 1; // cancelled
        }
        password = info.password;
    }

    if (!stateFile.isEmpty()) {
        QFile mark(stateFile);
        (void)mark.open(QIODevice::WriteOnly);
    }
    const QByteArray answer = password.toUtf8() + '\n';
    fwrite(answer.constData(), 1, answer.size(), stdout);
    return 0;
}
