// Test helper: puts entries in (and reads them from) KDE's password cache, as
// KIO's sftp worker would after logging in.
//   kpasswdtool add <url> <user> <password>
//   kpasswdtool check <url> [user]     prints "<user> <password>" or exits 1
#include <KIO/AuthInfo>
#include <KPasswdServerClient>

#include <QCoreApplication>
#include <QThread>
#include <QUrl>

#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 3) {
        return 2;
    }
    KPasswdServerClient client;
    KIO::AuthInfo info;
    info.url = QUrl(args.at(2));
    if (args.at(1) == QLatin1String("add") && args.size() == 5) {
        info.username = args.at(3);
        info.password = args.at(4);
        client.addAuthInfo(info, 0);
        // addAuthInfo does not wait for the server: check it arrived.
        for (int i = 0; i < 50; ++i) {
            KIO::AuthInfo check;
            check.url = info.url;
            check.username = info.username;
            if (client.checkAuthInfo(&check, 0, 0) && check.password == info.password) {
                return 0;
            }
            QThread::msleep(100);
        }
        return 1;
    }
    if (args.at(1) == QLatin1String("check")) {
        if (args.size() > 3) {
            info.username = args.at(3);
        }
        if (!client.checkAuthInfo(&info, 0, 0)) {
            return 1;
        }
        printf("%s %s\n", qPrintable(info.username), qPrintable(info.password));
        return 0;
    }
    return 2;
}
