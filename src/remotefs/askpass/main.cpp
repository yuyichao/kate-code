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

#include "RemoteLocation.h"

#include <KIO/AuthInfo>
#include <KLocalizedString>
#include <KPasswdServerClient>

#include <QCoreApplication>
#include <QFile>

#include <unistd.h>

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

    const auto target = KateRemote::parseSshAuthority(QString::fromLocal8Bit(qgetenv("KATE_REMOTE_AUTHORITY")));
    if (!target || !KateRemote::isLoginPasswordPrompt(prompt)) {
        return runFallback(prompt);
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
