/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#pragma once

#include "Files.h"

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace helper
{

// A system's directory change notifications (inotify, ReadDirectoryChangesW),
// non-recursive. Reports through the callback given to createDirBackend():
// (directory key, file name key), the name empty for "anything in it".
class DirBackend
{
public:
    enum class AddResult { Ok, Failed, UsePolling };
    using Callback = std::function<void(const std::string &dirKey, const std::string &nameKey)>;

    virtual ~DirBackend() = default;
    // dir: the directory's path as given (UTF-8); dirKey: pathKey(dir).
    virtual AddResult addDir(const std::string &dirKey, const std::string &dir, FsError &error) = 0;
    virtual void removeDir(const std::string &dirKey) = 0;
};

// nullptr where there is none (then everything is polled).
std::unique_ptr<DirBackend> createDirBackend(DirBackend::Callback callback);

// The protocol's watch/unwatch (PROTOCOL.md): files watched through their
// directory's notifications (or polling), changes reported in batches.
class Watcher
{
public:
    using Notify = std::function<void(const std::vector<std::string> &paths)>;
    explicit Watcher(Notify notify);
    ~Watcher();

    std::vector<std::pair<std::string, FsError>> add(const std::vector<std::string> &paths);
    void remove(const std::vector<std::string> &paths);

    // How long changes are collected before they are reported.
    static constexpr int BatchMs = 100;
    static constexpr int PollMs = 500;

private:
    struct Dir {
        bool polled = false;
        std::map<std::string, std::set<std::string>> names; // name key -> client paths
    };

    void onDirEvent(const std::string &dirKey, const std::string &nameKey);
    void report(const std::set<std::string> &paths);
    void flushLoop();
    void pollLoop();

    Notify m_notify;
    std::unique_ptr<DirBackend> m_backend;

    std::mutex m_opMutex; // serializes add/remove (backend calls)
    std::mutex m_mutex; // the maps below, the pending set
    std::condition_variable m_wake;
    bool m_stopping = false;
    std::map<std::string, Dir> m_dirs; // dir key -> files
    std::map<std::string, std::pair<std::string, std::string>> m_paths; // client path -> (dir key, name key)
    std::map<std::string, FileStamp> m_polled; // client path -> last stamp
    std::set<std::string> m_pending;

    std::thread m_flushThread;
    std::thread m_pollThread;
};

}
