/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#include "Files.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace helper
{

namespace
{
FsError errnoError(int err)
{
    FsError error;
    switch (err) {
    case ENOENT:
        error.name = "notFound";
        break;
    case EACCES:
    case EPERM:
    case EROFS:
        error.name = "permissionDenied";
        break;
    case EISDIR:
        error.name = "isDirectory";
        break;
    case ENOTDIR:
        error.name = "notDirectory";
        break;
    default:
        error.name = "io";
    }
    // The helper never calls setlocale(): English messages.
    error.message = std::strerror(err);
    return error;
}

int64_t timespecNs(const struct timespec &t)
{
    return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}

#ifdef __APPLE__
#define HELPER_MTIM(st) (st).st_mtimespec
#define HELPER_CTIM(st) (st).st_ctimespec
#else
#define HELPER_MTIM(st) (st).st_mtim
#define HELPER_CTIM(st) (st).st_ctim
#endif

mode_t currentUmask()
{
    // Read once, before any other thread runs (statics are initialized on
    // first use; the server calls writeFile from its main thread only).
    static const mode_t mask = [] {
        const mode_t m = umask(022);
        umask(m);
        return m;
    }();
    return mask;
}

bool writeAll(int fd, const std::string &data)
{
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        done += size_t(n);
    }
    return true;
}

bool writeInPlace(const std::string &path, const std::string &data, FsError &error)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) {
        error = errnoError(errno);
        return false;
    }
    if (!writeAll(fd, data) || ::fsync(fd) != 0) {
        error = errnoError(errno);
        ::close(fd);
        return false;
    }
    if (::close(fd) != 0) {
        error = errnoError(errno);
        return false;
    }
    return true;
}

// The file a write replaces: a symlink's final target.
std::string resolveTarget(const std::string &path)
{
    char *resolved = ::realpath(path.c_str(), nullptr);
    if (!resolved) {
        return path; // missing (or a dangling link: written through)
    }
    std::string target(resolved);
    std::free(resolved);
    return target;
}
}

std::string pathKey(const std::string &path)
{
    return path;
}

bool statPath(const std::string &path, FileInfo &info, FsError &error)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        error = errnoError(errno);
        return false;
    }
    info.type = S_ISREG(st.st_mode) ? FileInfo::Type::File : S_ISDIR(st.st_mode) ? FileInfo::Type::Directory : FileInfo::Type::Other;
    info.size = uint64_t(st.st_size);
    info.mtimeNs = timespecNs(HELPER_MTIM(st));
    info.mode = uint32_t(st.st_mode & 07777);
    return true;
}

FileStamp fileStamp(const std::string &path)
{
    FileStamp stamp;
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return stamp;
    }
    stamp.exists = true;
    stamp.size = uint64_t(st.st_size);
    stamp.mtimeNs = timespecNs(HELPER_MTIM(st));
    stamp.ctimeNs = timespecNs(HELPER_CTIM(st));
    stamp.id = uint64_t(st.st_ino) ^ (uint64_t(st.st_dev) << 32);
    return stamp;
}

bool readFile(const std::string &path, uint64_t maxSize, std::string &data, FsError &error)
{
    // O_NONBLOCK: opening a FIFO must not wait for a writer.
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        error = errnoError(errno);
        return false;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        error = errnoError(errno);
        ::close(fd);
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        ::close(fd);
        error = errnoError(EISDIR);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        ::close(fd);
        error = {"io", "Not a regular file"};
        return false;
    }
    if (uint64_t(st.st_size) > maxSize) {
        ::close(fd);
        error = {"tooLarge", "The file is larger than " + std::to_string(maxSize) + " bytes"};
        return false;
    }
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    data.clear();
    if (uint64_t(st.st_size) >= data.max_size()) { // a 32-bit helper
        ::close(fd);
        error = {"tooLarge", "The file is too large for this helper's memory"};
        return false;
    }
    data.reserve(size_t(st.st_size));
    char buffer[65536];
    while (true) {
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = errnoError(errno);
            ::close(fd);
            return false;
        }
        if (n == 0) {
            break;
        }
        data.append(buffer, size_t(n));
        if (data.size() > maxSize) { // grew while reading
            ::close(fd);
            error = {"tooLarge", "The file is larger than " + std::to_string(maxSize) + " bytes"};
            return false;
        }
    }
    ::close(fd);
    return true;
}

bool writeFile(const std::string &path, const std::string &data, FsError &error)
{
    const std::string target = resolveTarget(path);
    struct stat st;
    const bool exists = ::stat(target.c_str(), &st) == 0;
    if (!exists && errno != ENOENT) {
        error = errnoError(errno);
        return false;
    }
    if (exists && S_ISDIR(st.st_mode)) {
        error = errnoError(EISDIR);
        return false;
    }
    if (exists && !S_ISREG(st.st_mode)) {
        error = {"io", "Not a regular file"};
        return false;
    }
    // A rename would detach the other names of a hard-linked file.
    if (exists && st.st_nlink > 1) {
        return writeInPlace(target, data, error);
    }

    std::string dir, name;
    if (!splitPath(target, dir, name)) {
        error = {"io", "Not a file path"};
        return false;
    }
    std::string temp = (dir == "/" ? dir : dir + "/") + "." + name + ".kate-code-XXXXXX";
    std::string tempBuffer = temp; // mkstemp writes into it
    const int fd = ::mkstemp(tempBuffer.data());
    if (fd < 0) {
        const int err = errno;
        // A directory we may not create files in, with a file we may write.
        if (exists && (err == EACCES || err == EPERM || err == EROFS)) {
            return writeInPlace(target, data, error);
        }
        error = errnoError(err);
        return false;
    }
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    temp = tempBuffer;
    auto failWith = [&](int err) {
        error = errnoError(err);
        ::close(fd);
        ::unlink(temp.c_str());
        return false;
    };
    if (exists) {
        // Owner first: changing it may clear the set-id bits.
        if (st.st_uid != ::geteuid() || st.st_gid != ::getegid()) {
            if (::fchown(fd, st.st_uid, st.st_gid) != 0) {
                // Not allowed to give the file its owner back: keep the
                // file (and its owner) by writing into it.
                ::close(fd);
                ::unlink(temp.c_str());
                return writeInPlace(target, data, error);
            }
        }
        if (::fchmod(fd, st.st_mode & 07777) != 0) {
            return failWith(errno);
        }
    } else if (::fchmod(fd, 0666 & ~currentUmask()) != 0) {
        return failWith(errno);
    }
    if (!writeAll(fd, data) || ::fsync(fd) != 0) {
        return failWith(errno);
    }
    if (::close(fd) != 0) {
        error = errnoError(errno);
        ::unlink(temp.c_str());
        return false;
    }
    if (::rename(temp.c_str(), target.c_str()) != 0) {
        error = errnoError(errno);
        ::unlink(temp.c_str());
        return false;
    }
    return true;
}

}
