/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

// Windows: ReadDirectoryChangesW (overlapped) on each directory, a thread
// per directory.

#include "WinUtil.h"
#include "Watcher.h"

#include <atomic>

namespace helper
{

namespace
{
constexpr DWORD Filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION;

class DirWatch
{
public:
    DirWatch(HANDLE dir, std::string dirKey, const DirBackend::Callback &callback)
        : m_dir(dir)
        , m_dirKey(std::move(dirKey))
        , m_callback(callback)
    {
        m_ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~DirWatch()
    {
        if (m_thread.joinable()) {
            SetEvent(m_stopEvent);
            m_thread.join();
        }
        CloseHandle(m_ioEvent);
        CloseHandle(m_stopEvent);
        CloseHandle(m_dir);
    }

    // Starts the first read here, so that a failing directory is reported
    // by addDir().
    bool start(DWORD &error)
    {
        if (!m_ioEvent || !m_stopEvent || !issue()) {
            error = GetLastError();
            return false;
        }
        m_thread = std::thread([this] {
            run();
        });
        return true;
    }

private:
    bool issue()
    {
        ZeroMemory(&m_overlapped, sizeof m_overlapped);
        m_overlapped.hEvent = m_ioEvent;
        ResetEvent(m_ioEvent);
        return ReadDirectoryChangesW(m_dir, m_buffer, sizeof m_buffer, FALSE, Filter, nullptr, &m_overlapped, nullptr);
    }

    void run()
    {
        while (true) {
            HANDLE events[2] = {m_ioEvent, m_stopEvent};
            const DWORD which = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (which != WAIT_OBJECT_0) {
                CancelIoEx(m_dir, &m_overlapped);
                DWORD ignored = 0;
                GetOverlappedResult(m_dir, &m_overlapped, &ignored, TRUE);
                return;
            }
            DWORD bytes = 0;
            if (!GetOverlappedResult(m_dir, &m_overlapped, &bytes, FALSE)) {
                // The directory went away (or the share): report its files
                // once; nothing more comes from it.
                m_callback(m_dirKey, std::string());
                return;
            }
            if (bytes == 0) {
                m_callback(m_dirKey, std::string()); // buffer overflow: anything may have changed
            } else {
                parse(bytes);
            }
            if (!issue()) {
                m_callback(m_dirKey, std::string());
                return;
            }
        }
    }

    void parse(DWORD bytes)
    {
        const char *p = reinterpret_cast<const char *>(m_buffer);
        const char *end = p + bytes;
        while (p < end) {
            const auto *info = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(p);
            const std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            m_callback(m_dirKey, pathKey(fromWide(name)));
            if (!info->NextEntryOffset) {
                break;
            }
            p += info->NextEntryOffset;
        }
    }

    HANDLE m_dir;
    std::string m_dirKey;
    const DirBackend::Callback &m_callback;
    HANDLE m_ioEvent = nullptr;
    HANDLE m_stopEvent = nullptr;
    OVERLAPPED m_overlapped;
    // DWORD-aligned, at most 64 KiB (network shares).
    DWORD m_buffer[16 * 1024];
    std::thread m_thread;
};

class WindowsBackend : public DirBackend
{
public:
    explicit WindowsBackend(Callback callback)
        : m_callback(std::move(callback))
    {
    }

    ~WindowsBackend() override
    {
        std::map<std::string, std::unique_ptr<DirWatch>> watches;
        {
            std::lock_guard lock(m_mutex);
            watches.swap(m_watches);
        }
        watches.clear(); // joins the threads, outside m_mutex
    }

    AddResult addDir(const std::string &dirKey, const std::string &dir, FsError &error) override
    {
        const std::wstring native = nativePath(dir);
        HANDLE h = CreateFileW(native.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            error.name = (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) ? "notFound" : code == ERROR_ACCESS_DENIED ? "permissionDenied" : "io";
            error.message = windowsErrorMessage(code);
            return AddResult::Failed;
        }
        auto watch = std::make_unique<DirWatch>(h, dirKey, m_callback);
        DWORD code = 0;
        if (!watch->start(code)) {
            // Some file systems (some network shares, FAT on old systems)
            // give no notifications: poll those.
            if (code == ERROR_INVALID_FUNCTION || code == ERROR_NOT_SUPPORTED) {
                return AddResult::UsePolling;
            }
            error.name = "io";
            error.message = windowsErrorMessage(code);
            return AddResult::Failed;
        }
        std::lock_guard lock(m_mutex);
        m_watches[dirKey] = std::move(watch);
        return AddResult::Ok;
    }

    void removeDir(const std::string &dirKey) override
    {
        std::unique_ptr<DirWatch> watch;
        {
            std::lock_guard lock(m_mutex);
            const auto it = m_watches.find(dirKey);
            if (it == m_watches.end()) {
                return;
            }
            watch = std::move(it->second);
            m_watches.erase(it);
        }
        watch.reset(); // joins its thread
    }

private:
    Callback m_callback;
    std::mutex m_mutex;
    std::map<std::string, std::unique_ptr<DirWatch>> m_watches;
};
}

std::unique_ptr<DirBackend> createDirBackend(DirBackend::Callback callback)
{
    return std::make_unique<WindowsBackend>(std::move(callback));
}

}
