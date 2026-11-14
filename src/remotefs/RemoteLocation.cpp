#include "RemoteLocation.h"

#include <KConfig>
#include <KConfigGroup>

#include <QDir>
#include <QRegularExpression>

#include <vector>

namespace KateRemote
{

QString configFileName()
{
    return QStringLiteral("kate-remoterc");
}

QString defaultMountRoot()
{
    return QDir::homePath() + QStringLiteral("/.kate-remote");
}

QString cleanMountRoot(const QString &setting)
{
    QString path = setting.trimmed();
    if (path == QLatin1String("~")) {
        path = QDir::homePath();
    } else if (path.startsWith(QLatin1String("~/"))) {
        path = QDir::homePath() + path.mid(1);
    }
    if (path.isEmpty() || QDir::isRelativePath(path)) {
        return defaultMountRoot();
    }
    path = QDir::cleanPath(path);
    // The root itself or the home directory would shadow everything below it.
    if (path == QLatin1String("/") || path == QDir::cleanPath(QDir::homePath())) {
        return defaultMountRoot();
    }
    return path;
}

QString configuredMountRoot()
{
    KConfig config(configFileName(), KConfig::SimpleConfig);
    return cleanMountRoot(KConfigGroup(&config, QStringLiteral("General")).readEntry("MountRoot", QString()));
}

bool isSupportedScheme(const QString &scheme)
{
    return scheme == QLatin1String("sftp");
}

QString SshTarget::destination() const
{
    QString result;
    if (!user.isEmpty()) {
        result = user + QLatin1Char('@');
    }
    if (host.contains(QLatin1Char(':'))) {
        result += QLatin1Char('[') + host + QLatin1Char(']');
    } else {
        result += host;
    }
    return result;
}

std::optional<SshTarget> parseSshAuthority(const QString &authority)
{
    // user: no leading '-' (it would read as an option)
    // host: name/alias with no leading '-' or '.', or a bracketed IPv6 address
    static const QRegularExpression re(QStringLiteral("^(?:([A-Za-z0-9_][A-Za-z0-9._-]*)@)?"
                                                      "(?:([A-Za-z0-9_][A-Za-z0-9._-]*)|\\[([0-9A-Fa-f:.]+)\\])"
                                                      "(?::([0-9]{1,5}))?$"));
    if (authority.isEmpty() || authority.size() > 255) {
        return std::nullopt;
    }
    const auto match = re.match(authority);
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    SshTarget target;
    target.user = match.captured(1);
    target.host = match.hasCaptured(2) ? match.captured(2) : match.captured(3);
    if (match.hasCaptured(3) && !target.host.contains(QLatin1Char(':'))) {
        return std::nullopt;
    }
    if (match.hasCaptured(4)) {
        const int port = match.captured(4).toInt();
        if (port < 1 || port > 65535) {
            return std::nullopt;
        }
        target.port = port;
    }
    return target;
}

QUrl kioAuthUrl(const SshTarget &target)
{
    QUrl url;
    url.setScheme(QStringLiteral("sftp"));
    url.setHost(target.host);
    if (target.port > 0 && target.port != 22) {
        url.setPort(target.port);
    }
    if (!target.user.isEmpty()) {
        url.setUserName(target.user);
    }
    return url;
}

bool isLoginPasswordPrompt(const QString &prompt)
{
    const QString text = prompt.trimmed().toLower();
    if (text.contains(QLatin1String("passphrase")) || text.contains(QLatin1String("new password")) || text.contains(QLatin1String("retype"))
        || text.contains(QLatin1String("old password")) || text.contains(QLatin1String("current password"))) {
        return false;
    }
    static const QRegularExpression passwordPrompt(QStringLiteral("password( for [^:]*)?:$"));
    return passwordPrompt.match(text).hasMatch();
}

QString userFromPasswordPrompt(const QString &prompt)
{
    // "user@host's password: " or "(user@host) Password: "
    static const QRegularExpression userAtHost(QStringLiteral("^\\(?([^@()\\s]+)@[^\\s']+"));
    const auto match = userAtHost.match(prompt.trimmed());
    return match.hasMatch() ? match.captured(1) : QString();
}

bool isValidAuthority(const QString &scheme, const QString &authority)
{
    if (scheme == QLatin1String("sftp")) {
        return parseSshAuthority(authority).has_value();
    }
    return false;
}

QUrl Location::url() const
{
    QUrl url;
    url.setScheme(scheme);
    url.setAuthority(authority);
    url.setPath(path.isEmpty() ? QStringLiteral("/") : path);
    return url;
}

std::optional<Location> locationForPath(const QString &localPath, const QString &mountRoot)
{
    if (localPath.isEmpty() || QDir::isRelativePath(localPath)) {
        return std::nullopt;
    }
    const QString root = QDir::cleanPath(mountRoot);
    const QString path = QDir::cleanPath(localPath);
    if (!path.startsWith(root + QLatin1Char('/'))) {
        return std::nullopt;
    }
    const QString rest = path.mid(root.size() + 1);
    const qsizetype schemeEnd = rest.indexOf(QLatin1Char('/'));
    if (schemeEnd <= 0) {
        return std::nullopt;
    }
    Location location;
    location.scheme = rest.left(schemeEnd);
    const qsizetype authorityEnd = rest.indexOf(QLatin1Char('/'), schemeEnd + 1);
    location.authority = rest.mid(schemeEnd + 1, authorityEnd < 0 ? -1 : authorityEnd - schemeEnd - 1);
    location.path = authorityEnd < 0 ? QStringLiteral("/") : rest.mid(authorityEnd);
    if (!isSupportedScheme(location.scheme) || !isValidAuthority(location.scheme, location.authority)) {
        return std::nullopt;
    }
    return location;
}

QString pathForLocation(const Location &location, const QString &mountRoot)
{
    QString path = QDir::cleanPath(mountRoot) + QLatin1Char('/') + location.scheme + QLatin1Char('/') + location.authority;
    const QString remote = QDir::cleanPath(location.path.isEmpty() ? QStringLiteral("/") : location.path);
    if (remote != QLatin1String("/")) {
        path += remote.startsWith(QLatin1Char('/')) ? remote : QLatin1Char('/') + remote;
    }
    return path;
}

std::optional<Location> locationForUrl(const QUrl &url)
{
    if (!url.isValid() || !isSupportedScheme(url.scheme()) || url.host().isEmpty()) {
        return std::nullopt;
    }
    // Rebuilt from the parts so a password never ends up in a path.
    QString authority;
    if (!url.userName().isEmpty()) {
        authority = url.userName() + QLatin1Char('@');
    }
    const QString host = url.host();
    authority += host.contains(QLatin1Char(':')) ? QLatin1Char('[') + host + QLatin1Char(']') : host;
    if (url.port() != -1) {
        authority += QLatin1Char(':') + QString::number(url.port());
    }
    if (!isValidAuthority(url.scheme(), authority)) {
        return std::nullopt;
    }
    Location location;
    location.scheme = url.scheme();
    location.authority = authority;
    location.path = QDir::cleanPath(url.path().isEmpty() ? QStringLiteral("/") : url.path());
    if (!location.path.startsWith(QLatin1Char('/'))) {
        location.path.prepend(QLatin1Char('/'));
    }
    return location;
}

std::optional<QUrl> urlForInput(const QString &text, const QString &mountRoot, InputError *error)
{
    const auto fail = [error](InputError e) -> std::optional<QUrl> {
        *error = e;
        return std::nullopt;
    };
    *error = InputError::None;
    const QString input = text.trimmed();
    if (input.isEmpty()) {
        return fail(InputError::Empty);
    }

    std::optional<Location> location;
    if (input.startsWith(QLatin1Char('/')) || input.startsWith(QLatin1Char('~'))) {
        const QString path = input.startsWith(QLatin1Char('~')) ? QDir::homePath() + input.mid(1) : input;
        location = locationForPath(path, mountRoot);
        if (!location) {
            return fail(InputError::LocalPath);
        }
    } else if (input.contains(QLatin1String("://"))) {
        location = locationForUrl(QUrl(input));
        if (!location) {
            return fail(InputError::Unsupported);
        }
    } else {
        // scp style: [user@]host:/path
        static const QRegularExpression scpStyle(QStringLiteral("^([^/:]+):(.*)$"));
        const auto match = scpStyle.match(input);
        if (!match.hasMatch()) {
            return fail(InputError::Syntax);
        }
        const QString path = match.captured(2);
        if (!path.isEmpty() && !path.startsWith(QLatin1Char('/'))) {
            return fail(InputError::RelativePath);
        }
        QUrl url(QStringLiteral("sftp://") + match.captured(1));
        url.setPath(path.isEmpty() ? QStringLiteral("/") : path);
        location = locationForUrl(url);
        if (!location) {
            return fail(InputError::Unsupported);
        }
    }
    if (location->path == QLatin1String("/")) {
        return fail(InputError::RemoteRoot);
    }
    return location->url();
}

static std::vector<std::string> splitPath(const std::string &path)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) {
            end = path.size();
        }
        std::string part = path.substr(start, end - start);
        if (!part.empty() && part != ".") {
            parts.push_back(std::move(part));
        }
        start = end + 1;
    }
    return parts;
}

std::string hostSymlinkTarget(const std::string &linkDir, const std::string &target)
{
    if (target.empty()) {
        return target;
    }
    const std::vector<std::string> dir = splitPath(linkDir);
    const std::vector<std::string> parts = splitPath(target);
    const bool absolute = target.front() == '/';
    if (!absolute) {
        size_t depth = dir.size();
        bool escapes = false;
        for (const auto &part : parts) {
            if (part == "..") {
                if (depth == 0) {
                    escapes = true;
                    break;
                }
                --depth;
            } else {
                ++depth;
            }
        }
        if (!escapes) {
            return target;
        }
    }

    // Resolve against the remote root, where ".." of "/" is "/".
    std::vector<std::string> resolved = absolute ? std::vector<std::string>() : dir;
    for (const auto &part : parts) {
        if (part == "..") {
            if (!resolved.empty()) {
                resolved.pop_back();
            }
        } else {
            resolved.push_back(part);
        }
    }

    std::string result;
    for (size_t i = 0; i < dir.size(); ++i) {
        result += "../";
    }
    for (const auto &part : resolved) {
        result += part;
        result += '/';
    }
    if (!result.empty()) {
        result.pop_back();
    }
    return result.empty() ? std::string(".") : result;
}

}
