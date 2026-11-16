/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#include "Files.h"

namespace helper
{

bool isSeparator(char c)
{
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

bool isAbsolutePath(const std::string &path)
{
#ifdef _WIN32
    const auto letter = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    if (path.size() >= 3 && letter(path[0]) && path[1] == ':' && isSeparator(path[2])) {
        return true;
    }
    // UNC: \\server\share
    return path.size() >= 5 && isSeparator(path[0]) && isSeparator(path[1]) && !isSeparator(path[2]);
#else
    return !path.empty() && path[0] == '/';
#endif
}

bool splitPath(const std::string &path, std::string &dir, std::string &name)
{
    if (path.empty() || isSeparator(path.back())) {
        return false;
    }
    size_t slash = path.size();
    while (slash > 0 && !isSeparator(path[slash - 1])) {
        --slash;
    }
    if (slash == 0) {
        return false;
    }
    name = path.substr(slash);
    if (name == "." || name == "..") {
        return false;
    }
    // Keep the separator for roots ("/", "C:\"), drop it otherwise.
    size_t dirEnd = slash - 1;
    while (dirEnd > 0 && isSeparator(path[dirEnd - 1])) {
        --dirEnd;
    }
#ifdef _WIN32
    if (dirEnd == 2 && path[1] == ':') {
        dirEnd = 3;
    }
    // A UNC share root (\\server\share) has no parent to watch from here:
    // "\\server" is not a directory.
    if (path.size() >= 2 && isSeparator(path[0]) && isSeparator(path[1])) {
        size_t separators = 0;
        for (size_t i = 2; i < dirEnd; ++i) {
            separators += isSeparator(path[i]) ? 1 : 0;
        }
        if (separators == 0) {
            return false;
        }
    }
#endif
    if (dirEnd == 0) {
        dirEnd = 1; // "/"
    }
    dir = path.substr(0, dirEnd);
    return !name.empty();
}

}
