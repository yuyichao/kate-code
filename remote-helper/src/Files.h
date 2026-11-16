/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#pragma once

#include <cstdint>
#include <optional>
#include <string>

// The host's file system, for the protocol's file methods (see
// PROTOCOL.md). Paths are UTF-8; FilesPosix.cpp and FilesWindows.cpp
// implement it.
namespace helper
{

struct FsError {
    std::string name; // PROTOCOL.md's error names: notFound, permissionDenied, ...
    std::string message;
};

struct FileInfo {
    enum class Type { File, Directory, Other };
    Type type = Type::Other;
    uint64_t size = 0;
    int64_t mtimeNs = 0; // since the Unix epoch
    std::optional<uint32_t> mode; // POSIX permission bits
};

// What tells whether a file changed (polling).
struct FileStamp {
    bool exists = false;
    uint64_t size = 0;
    int64_t mtimeNs = 0;
    int64_t ctimeNs = 0; // POSIX: also changes on a rename over the file
    uint64_t id = 0; // inode / file index
    bool operator==(const FileStamp &other) const
    {
        return exists == other.exists && size == other.size && mtimeNs == other.mtimeNs && ctimeNs == other.ctimeNs && id == other.id;
    }
    bool operator!=(const FileStamp &other) const { return !(*this == other); }
};

// POSIX "/..."; Windows "C:\...", "C:/...", "\\server\share..." (also with '/').
bool isAbsolutePath(const std::string &path);
bool isSeparator(char c);

// "/a/b" -> ("/a", "b"); "/b" -> ("/", "b"); Windows "C:\b" -> ("C:\", "b").
// False when there is no file name (a root, a trailing separator).
bool splitPath(const std::string &path, std::string &dir, std::string &name);

// The form under which a directory / file name is compared: as is on POSIX;
// Windows: '\' separators, upper case (case-insensitive names).
std::string pathKey(const std::string &path);

bool statPath(const std::string &path, FileInfo &info, FsError &error);
FileStamp fileStamp(const std::string &path);

// The whole file; tooLarge above maxSize bytes.
bool readFile(const std::string &path, uint64_t maxSize, std::string &data, FsError &error);

// Replaces the file's contents (PROTOCOL.md "write": temporary file and
// rename, or in place).
bool writeFile(const std::string &path, const std::string &data, FsError &error);

}
