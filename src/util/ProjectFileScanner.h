#pragma once

#include <QString>
#include <QStringList>

#include <functional>

/**
 * ProjectFileScanner - Lists files under a directory for @-completion.
 *
 * Pure filesystem code with no widget access, so it can run on a worker
 * thread. The walk is bounded: it skips common build/VCS directories, does
 * not descend into symlinked directories (no cycles), stops after maxFiles
 * entries and checks the cancel callback for every entry.
 */
namespace ProjectFileScanner
{
// Default cap on the number of files returned.
constexpr int DefaultMaxFiles = 50000;

struct Result {
    QStringList files;      // paths relative to the scanned root
    bool truncated = false; // hit maxFiles before finishing
    bool cancelled = false; // isCancelled() returned true
};

Result scan(const QString &root,
            int maxFiles = DefaultMaxFiles,
            const std::function<bool()> &isCancelled = {});
}
