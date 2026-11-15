#include "RemotePathMap.h"

#include <QJsonArray>
#include <QSet>
#include <QUrl>

namespace KateRemote
{

RemotePathMap::RemotePathMap(const QString &hostDir, Style style, QChar drive)
    : m_hostDir(hostDir)
    , m_style(style)
    , m_drive(drive)
{
    if (!m_hostDir.isEmpty()) {
        // "file://" + path, as QUrl::fromLocalFile() encodes it.
        m_encodedHostDir = QUrl::fromLocalFile(m_hostDir).toString(QUrl::FullyEncoded).mid(7);
    }
}

// The components of an absolute path, "." and ".." resolved (".." stops at
// the root).
static QStringList normalizedParts(QStringView path, bool windows)
{
    QStringList parts;
    const QString text = windows ? path.toString().replace(QLatin1Char('\\'), QLatin1Char('/')) : path.toString();
    for (const QString &part : text.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QLatin1String("..")) {
            if (!parts.isEmpty()) {
                parts.removeLast();
            }
        } else if (part != QLatin1String(".")) {
            parts << part;
        }
    }
    return parts;
}

static bool isDrivePath(QStringView path)
{
    return path.size() >= 2 && path.at(0).isLetter() && path.at(0).unicode() < 128 && path.at(1) == QLatin1Char(':')
        && (path.size() == 2 || path.at(2) == QLatin1Char('/') || path.at(2) == QLatin1Char('\\'));
}

QString RemotePathMap::toLocal(const QString &remote) const
{
    if (m_hostDir.isEmpty()) {
        return remote;
    }
    const bool windows = m_style == Style::Windows;
    QStringView path(remote);
    if (windows && path.startsWith(QLatin1Char('/')) && isDrivePath(path.mid(1))) {
        path = path.mid(1); // the sftp spelling, /C:/x
    }
    if (windows && isDrivePath(path)) {
        // ".." stops at the drive's root: never a path above the host
        // directory (local files).
        QChar drive = path.at(0).toUpper();
        if (!m_drive.isNull() && m_drive.toUpper() == drive) {
            drive = m_drive;
        }
        QStringList parts = normalizedParts(path.mid(2), true);
        parts.prepend(QString(drive) + QLatin1Char(':'));
        return m_hostDir + QLatin1Char('/') + parts.join(QLatin1Char('/'));
    }
    if (path.startsWith(QLatin1Char('/'))) {
        // ".." stops at the remote root ("/.." is "/" there): never a path
        // above the host directory (local files).
        const QStringList parts = normalizedParts(path, false);
        return parts.isEmpty() ? m_hostDir : m_hostDir + QLatin1Char('/') + parts.join(QLatin1Char('/'));
    }
    if (remote.startsWith(QLatin1String("file:///"))) {
        const QUrl url(remote);
        if (url.isValid() && url.isLocalFile()) {
            // QUrl gives /C:/x for file:///C:/x: the sftp spelling.
            QUrl local = QUrl::fromLocalFile(toLocal(url.path()));
            local.setQuery(url.query());
            local.setFragment(url.fragment());
            return local.toString();
        }
    }
    return remote;
}

// Characters that may continue a host directory name (an authority): a
// match followed by one of them is another host ("host" in "host2").
static bool continuesName(QChar c)
{
    return c.isLetterOrNumber() || QStringView(u"-._@:;%~+").contains(c);
}

// Ends a Windows path inside text (Windows paths can have neither).
static bool endsWindowsPath(QChar c)
{
    return c.isSpace() || QStringView(u"\"'`<>|*?").contains(c);
}

static QString replacePrefix(const QString &text, const QString &prefix, bool windows, bool wholePath)
{
    if (prefix.isEmpty() || !text.contains(prefix)) {
        return text;
    }
    QString result;
    qsizetype from = 0;
    qsizetype at;
    while ((at = text.indexOf(prefix, from)) >= 0) {
        qsizetype end = at + prefix.size();
        result += QStringView(text).mid(from, at - from);
        // Punctuation ending a sentence ("... in <dir>.") is no name.
        const bool punctuation = end < text.size() && QStringView(u".:;").contains(text.at(end)) && (end + 1 == text.size() || text.at(end + 1).isSpace());
        if ((at > 0 && continuesName(text.at(at - 1))) || (end < text.size() && continuesName(text.at(end)) && !punctuation)) {
            result += prefix;
        } else if (end < text.size() && text.at(end) == QLatin1Char('/')) {
            const bool uri = QStringView(result).endsWith(QLatin1String("file://"));
            if (windows && !uri && isDrivePath(QStringView(text).mid(end + 1))) {
                // <hostDir>/C:/a/b -> C:\a\b
                const bool whole = wholePath && at == 0;
                qsizetype stop = end + 1;
                while (stop < text.size() && (whole || !endsWindowsPath(text.at(stop)))) {
                    ++stop;
                }
                QString path = text.mid(end + 1, stop - end - 1);
                path.replace(QLatin1Char('/'), QLatin1Char('\\'));
                if (path.size() == 2) {
                    path += QLatin1Char('\\'); // C: alone is the drive's current directory
                }
                result += path;
                end = stop;
            }
            // else <hostDir>/x -> /x (the '/' stays).
        } else {
            // The host directory itself: the remote root.
            result += QLatin1Char('/');
        }
        from = end;
    }
    result += QStringView(text).mid(from);
    return result;
}

QString RemotePathMap::toRemote(const QString &text, bool wholePath) const
{
    if (m_hostDir.isEmpty()) {
        return text;
    }
    const bool windows = m_style == Style::Windows;
    QString result = replacePrefix(text, m_hostDir, windows, wholePath);
    if (m_encodedHostDir != m_hostDir) {
        result = replacePrefix(result, m_encodedHostDir, windows, wholePath);
    }
    return result;
}

static const QSet<QString> &pathKeys()
{
    static const QSet<QString> keys{
        QStringLiteral("path"),
        QStringLiteral("paths"),
        QStringLiteral("file"),
        QStringLiteral("file_path"),
        QStringLiteral("filePath"),
        QStringLiteral("notebook_path"),
        QStringLiteral("old_path"),
        QStringLiteral("new_path"),
        QStringLiteral("oldPath"),
        QStringLiteral("newPath"),
        QStringLiteral("cwd"),
        QStringLiteral("uri"),
    };
    return keys;
}

static QJsonValue mapIncoming(const RemotePathMap &map, const QJsonValue &value, bool pathValue);

static QJsonObject mapIncomingObject(const RemotePathMap &map, const QJsonObject &object)
{
    QJsonObject result;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        result.insert(it.key(), mapIncoming(map, it.value(), pathKeys().contains(it.key())));
    }
    return result;
}

static QJsonValue mapIncoming(const RemotePathMap &map, const QJsonValue &value, bool pathValue)
{
    if (value.isObject()) {
        return mapIncomingObject(map, value.toObject());
    }
    if (value.isArray()) {
        QJsonArray result;
        for (const QJsonValue &item : value.toArray()) {
            // "paths": [...] holds paths; arrays elsewhere hold objects.
            result.append(mapIncoming(map, item, pathValue));
        }
        return result;
    }
    if (pathValue && value.isString()) {
        return map.toLocal(value.toString());
    }
    return value;
}

QJsonObject RemotePathMap::incoming(const QJsonObject &message) const
{
    return m_hostDir.isEmpty() ? message : mapIncomingObject(*this, message);
}

static QJsonValue mapOutgoing(const RemotePathMap &map, const QJsonValue &value, bool pathValue)
{
    if (value.isObject()) {
        QJsonObject result;
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            result.insert(it.key(), mapOutgoing(map, it.value(), pathKeys().contains(it.key())));
        }
        return result;
    }
    if (value.isArray()) {
        QJsonArray result;
        for (const QJsonValue &item : value.toArray()) {
            result.append(mapOutgoing(map, item, pathValue));
        }
        return result;
    }
    if (value.isString()) {
        return map.toRemote(value.toString(), pathValue);
    }
    return value;
}

QJsonObject RemotePathMap::outgoing(const QJsonObject &message) const
{
    return m_hostDir.isEmpty() ? message : mapOutgoing(*this, message, false).toObject();
}

}
