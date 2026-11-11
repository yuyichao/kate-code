#include "ProjectFileScanner.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

namespace ProjectFileScanner
{

Result scan(const QString &root, int maxFiles, const std::function<bool()> &isCancelled)
{
    // Directories that are never useful for @-completion and can be huge.
    static const QSet<QString> ignoredDirs = {
        QStringLiteral(".git"),
        QStringLiteral(".hg"),
        QStringLiteral(".svn"),
        QStringLiteral("node_modules"),
        QStringLiteral("build"),
        QStringLiteral("dist"),
        QStringLiteral("target"),
        QStringLiteral(".idea"),
        QStringLiteral(".vscode"),
        QStringLiteral("__pycache__"),
        QStringLiteral(".pytest_cache"),
        QStringLiteral(".tox"),
        QStringLiteral("venv"),
        QStringLiteral(".venv"),
        QStringLiteral("env"),
    };

    Result result;
    if (root.isEmpty() || !QFileInfo(root).isDir()) {
        return result;
    }

    // Iterative depth-first walk, one directory at a time, so pruning and
    // cancellation happen per entry rather than per subtree.
    struct Pending {
        QString path;
        QString relative;
    };
    QList<Pending> stack{{QDir(root).absolutePath(), QString()}};

    while (!stack.isEmpty()) {
        const Pending dir = stack.takeLast();
        QList<Pending> subdirs;

        QDirIterator it(dir.path, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        while (it.hasNext()) {
            if (isCancelled && isCancelled()) {
                result.cancelled = true;
                return result;
            }

            it.next();
            const QFileInfo info = it.fileInfo();
            const QString name = info.fileName();
            const QString relative = dir.relative.isEmpty() ? name : dir.relative + QLatin1Char('/') + name;

            if (info.isDir()) {
                // Symlinked directories can form cycles or point outside
                // the project, so they are listed but not entered.
                if (!info.isSymLink() && !ignoredDirs.contains(name)) {
                    subdirs.append({info.absoluteFilePath(), relative});
                }
            } else if (info.isFile()) {
                if (result.files.size() >= maxFiles) {
                    result.truncated = true;
                    return result;
                }
                result.files.append(relative);
            }
        }

        // Visit subdirectories in name order (the stack pops from the end).
        std::sort(subdirs.begin(), subdirs.end(), [](const Pending &a, const Pending &b) {
            return a.relative > b.relative;
        });
        stack.append(subdirs);
    }

    result.files.sort();
    return result;
}

}
