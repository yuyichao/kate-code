/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#include "Files.h"
#include "WinUtil.h"

#include <atomic>

namespace helper
{

namespace
{
FsError winError(DWORD code)
{
    FsError error;
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_INVALID_DRIVE:
        error.name = "notFound";
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_WRITE_PROTECT:
        error.name = "permissionDenied";
        break;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_USER_MAPPED_FILE:
        error.name = "busy";
        break;
    case ERROR_DIRECTORY:
        error.name = "notDirectory";
        break;
    default:
        error.name = "io";
    }
    error.message = windowsErrorMessage(code);
    return error;
}

FsError lastError()
{
    return winError(GetLastError());
}

int64_t fileTimeNs(const FILETIME &t)
{
    const int64_t ticks = int64_t((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime);
    return (ticks - 116444736000000000LL) * 100; // 100 ns ticks since 1601
}

class Handle
{
public:
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE)
        : m_h(h)
    {
    }
    ~Handle() { reset(); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    void reset(HANDLE h = INVALID_HANDLE_VALUE)
    {
        if (m_h != INVALID_HANDLE_VALUE) {
            CloseHandle(m_h);
        }
        m_h = h;
    }
    HANDLE get() const { return m_h; }
    bool valid() const { return m_h != INVALID_HANDLE_VALUE; }

private:
    HANDLE m_h;
};

// Opened for attributes only, following links; directories too.
HANDLE openForInfo(const std::wstring &path)
{
    return CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

bool isDirectoryPath(const std::wstring &path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool writeAll(HANDLE h, const std::string &data)
{
    size_t done = 0;
    while (done < data.size()) {
        const DWORD chunk = DWORD(std::min<size_t>(data.size() - done, 1 << 30));
        DWORD written = 0;
        if (!WriteFile(h, data.data() + done, chunk, &written, nullptr)) {
            return false;
        }
        done += written;
    }
    return true;
}

bool writeInPlace(const std::wstring &path, const std::string &data, FsError &error)
{
    // OPEN_ALWAYS + SetEndOfFile, not CREATE_ALWAYS: that fails on hidden
    // or system files and resets attributes.
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        error = lastError();
        return false;
    }
    if (!writeAll(file.get(), data) || !SetEndOfFile(file.get()) || !FlushFileBuffers(file.get())) {
        error = lastError();
        return false;
    }
    return true;
}

// The final path of an existing file (through symlinks / junctions).
std::wstring finalPath(HANDLE h, const std::wstring &fallback)
{
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD n = GetFinalPathNameByHandleW(h, buffer.data(), DWORD(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (n == 0) {
            return fallback;
        }
        if (n < buffer.size()) {
            buffer.resize(n);
            return buffer; // "\\?\C:\..." or "\\?\UNC\..."
        }
        buffer.resize(n + 1);
    }
}

std::wstring parentOf(const std::wstring &path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring nameOf(const std::wstring &path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}
}

std::wstring nativePath(const std::string &path)
{
    std::wstring wide = toWide(path);
    for (wchar_t &c : wide) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    // Long paths need the \\?\ form (which is taken literally: no '/', '.'
    // or '..' handling; the separators were converted above).
    if (wide.size() >= 240 && wide.compare(0, 4, L"\\\\?\\") != 0) {
        if (wide.size() >= 2 && wide[0] == L'\\' && wide[1] == L'\\') {
            wide = L"\\\\?\\UNC\\" + wide.substr(2);
        } else {
            wide = L"\\\\?\\" + wide;
        }
    }
    return wide;
}

std::string pathKey(const std::string &path)
{
    std::wstring wide = toWide(path);
    for (wchar_t &c : wide) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    if (!wide.empty()) {
        CharUpperBuffW(wide.data(), DWORD(wide.size()));
    }
    return fromWide(wide);
}

bool statPath(const std::string &path, FileInfo &info, FsError &error)
{
    Handle h(openForInfo(nativePath(path)));
    BY_HANDLE_FILE_INFORMATION data;
    if (!h.valid() || !GetFileInformationByHandle(h.get(), &data)) {
        error = lastError();
        return false;
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        info.type = FileInfo::Type::Directory;
    } else {
        info.type = GetFileType(h.get()) == FILE_TYPE_DISK ? FileInfo::Type::File : FileInfo::Type::Other;
    }
    info.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    info.mtimeNs = fileTimeNs(data.ftLastWriteTime);
    info.mode.reset();
    return true;
}

FileStamp fileStamp(const std::string &path)
{
    FileStamp stamp;
    Handle h(openForInfo(nativePath(path)));
    BY_HANDLE_FILE_INFORMATION data;
    if (!h.valid() || !GetFileInformationByHandle(h.get(), &data)) {
        return stamp;
    }
    stamp.exists = true;
    stamp.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    stamp.mtimeNs = fileTimeNs(data.ftLastWriteTime);
    stamp.ctimeNs = fileTimeNs(data.ftCreationTime);
    stamp.id = (uint64_t(data.nFileIndexHigh) << 32) | data.nFileIndexLow;
    return stamp;
}

bool readFile(const std::string &path, uint64_t maxSize, std::string &data, FsError &error)
{
    const std::wstring native = nativePath(path);
    Handle file(CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        const DWORD code = GetLastError();
        if (code == ERROR_ACCESS_DENIED && isDirectoryPath(native)) {
            error = {"isDirectory", "Is a directory"};
        } else {
            error = winError(code);
        }
        return false;
    }
    if (GetFileType(file.get()) != FILE_TYPE_DISK) {
        error = {"io", "Not a regular file"};
        return false;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file.get(), &size)) {
        error = lastError();
        return false;
    }
    if (uint64_t(size.QuadPart) > maxSize) {
        error = {"tooLarge", "The file is larger than " + std::to_string(maxSize) + " bytes"};
        return false;
    }
    data.clear();
    if (uint64_t(size.QuadPart) >= data.max_size()) { // a 32-bit helper
        error = {"tooLarge", "The file is too large for this helper's memory"};
        return false;
    }
    data.reserve(size_t(size.QuadPart));
    char buffer[65536];
    while (true) {
        DWORD n = 0;
        if (!ReadFile(file.get(), buffer, sizeof buffer, &n, nullptr)) {
            error = lastError();
            return false;
        }
        if (n == 0) {
            break;
        }
        data.append(buffer, n);
        if (data.size() > maxSize) {
            error = {"tooLarge", "The file is larger than " + std::to_string(maxSize) + " bytes"};
            return false;
        }
    }
    return true;
}

bool writeFile(const std::string &path, const std::string &data, FsError &error)
{
    std::wstring target = nativePath(path);
    bool exists = false;
    {
        Handle h(openForInfo(target));
        BY_HANDLE_FILE_INFORMATION info;
        if (h.valid() && GetFileInformationByHandle(h.get(), &info)) {
            if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                error = {"isDirectory", "Is a directory"};
                return false;
            }
            exists = true;
            target = finalPath(h.get(), target);
            // A replace would detach the other names of a hard-linked file.
            if (info.nNumberOfLinks > 1) {
                h.reset();
                return writeInPlace(target, data, error);
            }
        } else if (!h.valid()) {
            const DWORD code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND) {
                error = winError(code);
                return false;
            }
        }
    }

    const std::wstring dir = parentOf(target);
    if (dir.empty()) {
        error = {"io", "Not a file path"};
        return false;
    }
    static std::atomic<unsigned> counter{0};
    std::wstring temp;
    Handle file;
    for (int attempt = 0; attempt < 100 && !file.valid(); ++attempt) {
        temp = dir + L"\\." + nameOf(target) + L".kate-code-" + std::to_wstring(GetCurrentProcessId()) + L"-"
            + std::to_wstring(counter++) + L"-" + std::to_wstring(GetTickCount64());
        file.reset(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.valid() && GetLastError() != ERROR_FILE_EXISTS) {
            break;
        }
    }
    if (!file.valid()) {
        const DWORD code = GetLastError();
        if (exists && code == ERROR_ACCESS_DENIED) {
            return writeInPlace(target, data, error); // directory not writable
        }
        error = winError(code);
        return false;
    }
    if (!writeAll(file.get(), data) || !FlushFileBuffers(file.get())) {
        error = lastError();
        file.reset();
        DeleteFileW(temp.c_str());
        return false;
    }
    file.reset();

    if (!exists) {
        if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = lastError();
            DeleteFileW(temp.c_str());
            return false;
        }
        return true;
    }
    // ReplaceFileW keeps the file's attributes, ACL and creation time.
    if (ReplaceFileW(target.c_str(), temp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
        return true;
    }
    const DWORD code = GetLastError();
    if (code == ERROR_UNABLE_TO_MOVE_REPLACEMENT) {
        // Without a backup name this means the old file is gone and the new
        // contents are still under the temporary name: move them in place.
        if (MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        error = lastError();
        error.message += " (the new contents are in " + fromWide(temp) + ")";
        return false;
    }
    DeleteFileW(temp.c_str());
    // Open without delete sharing, or a file we may write but not replace.
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED || code == ERROR_UNABLE_TO_REMOVE_REPLACED) {
        return writeInPlace(target, data, error);
    }
    error = winError(code);
    return false;
}

}
