/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#include "Watcher.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace helper
{

Watcher::Watcher(Notify notify)
    : m_notify(std::move(notify))
{
    // KATE_CODE_HELPER_WATCH=poll: polling only (tests: the code path of
    // systems without a backend).
    const char *mode = std::getenv("KATE_CODE_HELPER_WATCH");
    if (!mode || std::strcmp(mode, "poll") != 0) {
        m_backend = createDirBackend([this](const std::string &dirKey, const std::string &nameKey) {
            onDirEvent(dirKey, nameKey);
        });
    }
    m_flushThread = std::thread([this] {
        flushLoop();
    });
    m_pollThread = std::thread([this] {
        pollLoop();
    });
}

Watcher::~Watcher()
{
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    m_flushThread.join();
    m_pollThread.join();
    m_backend.reset(); // stops its threads; callbacks after this point are gone
}

std::vector<std::pair<std::string, FsError>> Watcher::add(const std::vector<std::string> &paths)
{
    std::lock_guard op(m_opMutex);
    std::vector<std::pair<std::string, FsError>> failed;
    for (const std::string &path : paths) {
        {
            std::lock_guard lock(m_mutex);
            if (m_paths.count(path)) {
                continue;
            }
        }
        std::string dir, name;
        if (!isAbsolutePath(path) || !splitPath(path, dir, name)) {
            failed.push_back({path, {"io", "Not a file path"}});
            continue;
        }
        FileInfo info;
        FsError error;
        if (!statPath(dir, info, error)) {
            failed.push_back({path, error});
            continue;
        }
        if (info.type != FileInfo::Type::Directory) {
            failed.push_back({path, {"notDirectory", "Not a directory"}});
            continue;
        }
        const std::string dirKey = pathKey(dir);
        const std::string nameKey = pathKey(name);

        bool known = false;
        {
            std::lock_guard lock(m_mutex);
            known = m_dirs.count(dirKey) > 0;
        }
        bool polled = !m_backend;
        if (!known && m_backend) {
            switch (m_backend->addDir(dirKey, dir, error)) {
            case DirBackend::AddResult::Ok:
                break;
            case DirBackend::AddResult::UsePolling:
                polled = true;
                break;
            case DirBackend::AddResult::Failed:
                failed.push_back({path, error});
                continue;
            }
        }
        const FileStamp stamp = fileStamp(path);
        std::lock_guard lock(m_mutex);
        Dir &d = m_dirs[dirKey];
        if (!known) {
            d.polled = polled;
        }
        d.names[nameKey].insert(path);
        m_paths[path] = {dirKey, nameKey};
        if (d.polled) {
            m_polled[path] = stamp;
        }
    }
    return failed;
}

void Watcher::remove(const std::vector<std::string> &paths)
{
    std::lock_guard op(m_opMutex);
    std::vector<std::string> emptyDirs;
    {
        std::lock_guard lock(m_mutex);
        for (const std::string &path : paths) {
            const auto it = m_paths.find(path);
            if (it == m_paths.end()) {
                continue;
            }
            const auto [dirKey, nameKey] = it->second;
            m_paths.erase(it);
            m_polled.erase(path);
            m_pending.erase(path);
            const auto dirIt = m_dirs.find(dirKey);
            if (dirIt == m_dirs.end()) {
                continue;
            }
            auto &names = dirIt->second.names;
            const auto nameIt = names.find(nameKey);
            if (nameIt != names.end()) {
                nameIt->second.erase(path);
                if (nameIt->second.empty()) {
                    names.erase(nameIt);
                }
            }
            if (names.empty()) {
                if (!dirIt->second.polled) {
                    emptyDirs.push_back(dirKey);
                }
                m_dirs.erase(dirIt);
            }
        }
    }
    // Outside m_mutex: a backend may wait for its thread, which may be
    // waiting for m_mutex in onDirEvent().
    for (const std::string &dirKey : emptyDirs) {
        m_backend->removeDir(dirKey);
    }
}

void Watcher::onDirEvent(const std::string &dirKey, const std::string &nameKey)
{
    std::set<std::string> changed;
    {
        std::lock_guard lock(m_mutex);
        const auto dirIt = m_dirs.find(dirKey);
        if (dirIt == m_dirs.end()) {
            return;
        }
        if (nameKey.empty()) {
            for (const auto &[name, paths] : dirIt->second.names) {
                changed.insert(paths.begin(), paths.end());
            }
        } else {
            const auto nameIt = dirIt->second.names.find(nameKey);
            if (nameIt == dirIt->second.names.end()) {
                return;
            }
            changed = nameIt->second;
        }
    }
    report(changed);
}

void Watcher::report(const std::set<std::string> &paths)
{
    if (paths.empty()) {
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        m_pending.insert(paths.begin(), paths.end());
    }
    m_wake.notify_all();
}

void Watcher::flushLoop()
{
    std::unique_lock lock(m_mutex);
    while (true) {
        m_wake.wait(lock, [this] {
            return m_stopping || !m_pending.empty();
        });
        if (m_stopping) {
            return;
        }
        // Collect what follows (an editor's save is several events).
        m_wake.wait_for(lock, std::chrono::milliseconds(BatchMs), [this] {
            return m_stopping;
        });
        if (m_stopping) {
            return;
        }
        std::set<std::string> batch;
        batch.swap(m_pending);
        // Paths unwatched meanwhile were taken out of m_pending by remove().
        lock.unlock();
        m_notify(std::vector<std::string>(batch.begin(), batch.end()));
        lock.lock();
    }
}

void Watcher::pollLoop()
{
    std::unique_lock lock(m_mutex);
    while (true) {
        m_wake.wait_for(lock, std::chrono::milliseconds(PollMs), [this] {
            return m_stopping;
        });
        if (m_stopping) {
            return;
        }
        if (m_polled.empty()) {
            continue;
        }
        std::vector<std::string> paths;
        for (const auto &[path, stamp] : m_polled) {
            paths.push_back(path);
        }
        lock.unlock();
        std::vector<std::pair<std::string, FileStamp>> stamps;
        for (const std::string &path : paths) {
            stamps.push_back({path, fileStamp(path)});
        }
        lock.lock();
        std::set<std::string> changed;
        for (const auto &[path, stamp] : stamps) {
            const auto it = m_polled.find(path);
            if (it != m_polled.end() && it->second != stamp) {
                it->second = stamp;
                changed.insert(path);
            }
        }
        if (!changed.empty()) {
            m_pending.insert(changed.begin(), changed.end());
            m_wake.notify_all();
        }
    }
}

}
