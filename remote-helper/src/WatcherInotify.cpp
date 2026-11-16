/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

// Linux: one inotify instance, a watch per directory.

#include "Watcher.h"

#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace helper
{

namespace
{
constexpr uint32_t DirMask = IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF
    | IN_MOVE_SELF | IN_ONLYDIR;

class InotifyBackend : public DirBackend
{
public:
    explicit InotifyBackend(Callback callback)
        : m_callback(std::move(callback))
    {
        m_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
        if (m_fd >= 0 && pipe(m_wakePipe) == 0) {
            fcntl(m_wakePipe[0], F_SETFD, FD_CLOEXEC);
            fcntl(m_wakePipe[1], F_SETFD, FD_CLOEXEC);
            m_thread = std::thread([this] {
                run();
            });
        }
    }

    ~InotifyBackend() override
    {
        if (m_thread.joinable()) {
            const char c = 0;
            (void)!::write(m_wakePipe[1], &c, 1);
            m_thread.join();
        }
        for (const int fd : {m_fd, m_wakePipe[0], m_wakePipe[1]}) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    }

    bool usable() const { return m_thread.joinable(); }

    AddResult addDir(const std::string &dirKey, const std::string &dir, FsError &error) override
    {
        const int wd = inotify_add_watch(m_fd, dir.c_str(), DirMask);
        if (wd < 0) {
            const int err = errno;
            if (err == ENOSPC || err == EMFILE || err == ENOMEM) {
                return AddResult::UsePolling; // fs.inotify.max_user_watches
            }
            error.name = err == ENOENT ? "notFound" : err == ENOTDIR ? "notDirectory" : (err == EACCES || err == EPERM) ? "permissionDenied" : "io";
            error.message = std::strerror(err);
            return AddResult::Failed;
        }
        std::lock_guard lock(m_mutex);
        // Several spellings of a directory (a symlink to it) share its wd.
        m_dirsByWd[wd].insert(dirKey);
        m_wdByDir[dirKey] = wd;
        return AddResult::Ok;
    }

    void removeDir(const std::string &dirKey) override
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_wdByDir.find(dirKey);
        if (it == m_wdByDir.end()) {
            return;
        }
        const int wd = it->second;
        m_wdByDir.erase(it);
        auto &dirs = m_dirsByWd[wd];
        dirs.erase(dirKey);
        if (dirs.empty()) {
            m_dirsByWd.erase(wd);
            inotify_rm_watch(m_fd, wd);
        }
    }

private:
    void run()
    {
        // Not on the stack: musl's threads have small stacks (128 KiB).
        // (operator new's storage is aligned for inotify_event.)
        const size_t bufferSize = 64 * 1024;
        std::vector<char> storage(bufferSize);
        char *buffer = storage.data();
        while (true) {
            pollfd fds[2] = {{m_fd, POLLIN, 0}, {m_wakePipe[0], POLLIN, 0}};
            if (::poll(fds, 2, -1) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;
            }
            if (fds[1].revents) {
                return;
            }
            const ssize_t n = ::read(m_fd, buffer, bufferSize);
            if (n <= 0) {
                continue;
            }
            std::vector<std::pair<std::string, std::string>> events;
            for (char *p = buffer; p < buffer + n;) {
                const auto *event = reinterpret_cast<const struct inotify_event *>(p);
                p += sizeof(struct inotify_event) + event->len;
                std::lock_guard lock(m_mutex);
                if (event->mask & IN_Q_OVERFLOW) {
                    for (const auto &[wd, dirs] : m_dirsByWd) {
                        for (const std::string &dir : dirs) {
                            events.push_back({dir, std::string()});
                        }
                    }
                    continue;
                }
                const auto it = m_dirsByWd.find(event->wd);
                if (it == m_dirsByWd.end()) {
                    continue;
                }
                const bool whole = event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED);
                const std::string name = (!whole && event->len) ? std::string(event->name) : std::string();
                for (const std::string &dir : it->second) {
                    events.push_back({dir, name});
                }
                if (event->mask & IN_IGNORED) {
                    // The directory is gone; its files were reported above.
                    for (const std::string &dir : it->second) {
                        m_wdByDir.erase(dir);
                    }
                    m_dirsByWd.erase(it);
                }
            }
            for (const auto &[dir, name] : events) {
                m_callback(dir, name);
            }
        }
    }

    Callback m_callback;
    int m_fd = -1;
    int m_wakePipe[2] = {-1, -1};
    std::thread m_thread;
    std::mutex m_mutex;
    std::map<int, std::set<std::string>> m_dirsByWd;
    std::map<std::string, int> m_wdByDir;
};
}

std::unique_ptr<DirBackend> createDirBackend(DirBackend::Callback callback)
{
    auto backend = std::make_unique<InotifyBackend>(std::move(callback));
    if (!backend->usable()) {
        return nullptr; // no inotify instance left: poll everything
    }
    return backend;
}

}
