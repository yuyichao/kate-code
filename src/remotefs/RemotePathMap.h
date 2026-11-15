#pragma once

#include <QJsonObject>
#include <QString>

namespace KateRemote
{

/**
 * Path translation between Kate's view of a remote host (its directory
 * under the mount root, <root>/<scheme>/<authority>) and the host's own
 * paths, for an agent running on the host: everything on the Kate side uses
 * local paths, the agent sees remote ones.
 *
 * A remote absolute path P is hostDir + P locally (the mount mirrors the
 * whole remote file system), so the mapping is total for absolute paths.
 * An empty map (no host directory) is the identity.
 *
 * Windows hosts: Windows' sftp server shows drive C: as /C:, so the agent's
 * C:\a\b (or C:/a/b) is hostDir/C:/a/b locally. Exact local paths become
 * C:\a\b; paths inside text become C:\a\b up to the next space or quote
 * (the rest, if the path has spaces, keeps '/', which Windows takes too);
 * file:// URIs become file:///C:/a/b.
 */
class RemotePathMap
{
public:
    enum class Style {
        Posix,
        Windows,
    };

    RemotePathMap() = default;
    // hostDir: <root>/<scheme>/<authority>, absolute and clean. drive (Windows):
    // the drive letter as the mount spells it (the folder's), used for paths
    // on that drive whatever case the agent writes.
    explicit RemotePathMap(const QString &hostDir, Style style = Style::Posix, QChar drive = QChar());

    Style style() const
    {
        return m_style;
    }
    QChar drive() const
    {
        return m_drive;
    }

    bool isIdentity() const
    {
        return m_hostDir.isEmpty();
    }
    QString hostDir() const
    {
        return m_hostDir;
    }

    // A remote absolute path (or file:// URI) as a local one, normalized
    // ("." and ".." resolved, ".." stopping at the remote root, so the
    // result is never above the host directory); anything else (relative
    // paths, other text) unchanged.
    QString toLocal(const QString &remote) const;
    // Every local path below the host directory in 'text' (plain or in a
    // file:// URI) as the remote path; the rest of the text unchanged.
    // wholePath: text is one path (Windows: separators converted to its
    // end, not only up to the next space).
    QString toRemote(const QString &text, bool wholePath = false) const;

    // A message from the agent: values of path-like keys ("path",
    // "file_path", "cwd", "uri", ...) at any depth, through toLocal().
    // Free text (titles, commands, messages) is left alone.
    QJsonObject incoming(const QJsonObject &message) const;
    // A message to the agent: every string at any depth through toRemote()
    // (as a whole path for the path-like keys).
    QJsonObject outgoing(const QJsonObject &message) const;

private:
    QString m_hostDir;
    Style m_style = Style::Posix;
    QChar m_drive;
    QString m_encodedHostDir; // as in a FullyEncoded file:// URI
};

}
