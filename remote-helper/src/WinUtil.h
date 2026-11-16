/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#pragma once

#ifndef _WIN32
#error "Windows only"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <string>

namespace helper
{

inline std::wstring toWide(const std::string &utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring wide(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), wide.data(), n);
    return wide;
}

inline std::string fromWide(const std::wstring &wide)
{
    if (wide.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), utf8.data(), n, nullptr, nullptr);
    return utf8;
}

inline std::string windowsErrorMessage(DWORD code)
{
    wchar_t *buffer = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                                   reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    std::wstring text = n ? std::wstring(buffer, n) : L"Error " + std::to_wstring(code);
    if (buffer) {
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ' || text.back() == L'.')) {
        text.pop_back();
    }
    return fromWide(text);
}

// A path for the W APIs: '\' separators, \\?\ for long paths.
std::wstring nativePath(const std::string &path);

}
