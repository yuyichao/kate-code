/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#include "Server.h"

#include "Codec.h"
#include "Files.h"
#include "Version.h"
#include "Watcher.h"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace helper
{

namespace
{
constexpr uint64_t DefaultMaxSize = 64u << 20;

// What a handler fails with: a JSON-RPC error.
struct Failure {
    int code;
    std::string message;
    Json data;
};

Failure invalidParams(const std::string &message)
{
    return {-32602, message, Json()};
}

Failure fsFailure(const FsError &error, const std::string &path)
{
    Json data;
    data.set("error", error.name);
    data.set("path", path);
    return {1, error.message, data};
}

const std::string &requireString(const Json &params, const char *key)
{
    const Json *value = params.find(key);
    if (!value || !value->isString()) {
        throw invalidParams(std::string("\"") + key + "\" must be a string");
    }
    return value->toString();
}

const std::string &requirePath(const Json &params, const char *key)
{
    const std::string &path = requireString(params, key);
    if (!isAbsolutePath(path)) {
        throw invalidParams(std::string("\"") + key + "\" must be an absolute path");
    }
    return path;
}

std::vector<std::string> requirePaths(const Json &params)
{
    const Json *value = params.find("paths");
    if (!value || !value->isArray()) {
        throw invalidParams("\"paths\" must be an array of strings");
    }
    std::vector<std::string> paths;
    for (const Json &item : value->toArray()) {
        if (!item.isString() || !isAbsolutePath(item.toString())) {
            throw invalidParams("\"paths\" must hold absolute paths");
        }
        paths.push_back(item.toString());
    }
    return paths;
}

bool sameDigest(const std::string &a, const std::string &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) {
            return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
        };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

Json methodStat(const Json &params)
{
    const std::string &path = requirePath(params, "path");
    FileInfo info;
    FsError error;
    if (!statPath(path, info, error)) {
        throw fsFailure(error, path);
    }
    Json result;
    result.set("type", info.type == FileInfo::Type::File ? "file" : info.type == FileInfo::Type::Directory ? "directory" : "other");
    result.set("size", info.size);
    result.set("mtimeNs", std::to_string(info.mtimeNs));
    if (info.mode) {
        result.set("mode", int(*info.mode));
    }
    return result;
}

Json methodRead(const Json &params)
{
    const std::string &path = requirePath(params, "path");
    uint64_t maxSize = DefaultMaxSize;
    if (const Json *value = params.find("maxSize"); value && !value->isNull()) {
        if (!value->isNumber() || value->toNumber() < 0 || std::floor(value->toNumber()) != value->toNumber()) {
            throw invalidParams("\"maxSize\" must be a non-negative integer");
        }
        // (A double of 2^64 or more does not convert.)
        maxSize = value->toNumber() >= 18446744073709551615.0 ? UINT64_MAX : uint64_t(value->toNumber());
    }
    std::string data;
    FsError error;
    if (!readFile(path, maxSize, data, error)) {
        throw fsFailure(error, path);
    }
    Json result;
    result.set("size", uint64_t(data.size()));
    result.set("digest", gitBlobDigest(data));
    result.set("data", base64Encode(data));
    return result;
}

Json methodWrite(const Json &params)
{
    const std::string &path = requirePath(params, "path");
    const std::optional<std::string> data = base64Decode(requireString(params, "data"));
    if (!data) {
        throw invalidParams("\"data\" must be base64");
    }
    if (const Json *expect = params.find("expectDigest")) {
        if (!expect->isNull() && !expect->isString()) {
            throw invalidParams("\"expectDigest\" must be a string or null");
        }
        std::string current;
        FsError error;
        const bool exists = readFile(path, UINT64_MAX, current, error);
        if (!exists && error.name != "notFound") {
            throw fsFailure(error, path);
        }
        if (expect->isNull() && exists) {
            throw fsFailure({"changed", "The file exists"}, path);
        }
        if (expect->isString() && !exists) {
            throw fsFailure({"changed", "The file does not exist"}, path);
        }
        if (expect->isString() && !sameDigest(gitBlobDigest(current), expect->toString())) {
            throw fsFailure({"changed", "The file changed"}, path);
        }
    }
    FsError error;
    if (!writeFile(path, *data, error)) {
        throw fsFailure(error, path);
    }
    Json result;
    result.set("digest", gitBlobDigest(*data));
    return result;
}

Json methodDigest(const Json &params)
{
    Json digests = Json(Json::Array());
    for (const std::string &path : requirePaths(params)) {
        Json item;
        item.set("path", path);
        std::string data;
        FsError error;
        if (readFile(path, UINT64_MAX, data, error)) {
            item.set("digest", gitBlobDigest(data));
        } else {
            item.set("error", error.name);
        }
        digests.push(std::move(item));
    }
    Json result;
    result.set("digests", std::move(digests));
    return result;
}

// What is available (fread() would wait for a full buffer).
long readSome(std::FILE *in, char *buffer, size_t size)
{
    while (true) {
#ifdef _WIN32
        const int n = _read(_fileno(in), buffer, unsigned(size));
#else
        const ssize_t n = ::read(fileno(in), buffer, size);
#endif
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return long(n);
    }
}

Json outOfMemoryData()
{
    Json data;
    data.set("error", "io");
    return data;
}

Json errorResponse(const Json &id, int code, const std::string &message, const Json &data = Json())
{
    Json error;
    error.set("code", code);
    error.set("message", message);
    if (!data.isNull()) {
        error.set("data", data);
    }
    Json response;
    response.set("jsonrpc", "2.0");
    response.set("id", id);
    response.set("error", std::move(error));
    return response;
}
}

const char *helperVersion()
{
    return HELPER_VERSION;
}

const char *platformName()
{
#if defined(_WIN32)
#define HELPER_OS "windows"
#elif defined(__APPLE__)
#define HELPER_OS "darwin"
#elif defined(__linux__)
#define HELPER_OS "linux"
#elif defined(__FreeBSD__)
#define HELPER_OS "freebsd"
#elif defined(__OpenBSD__)
#define HELPER_OS "openbsd"
#elif defined(__NetBSD__)
#define HELPER_OS "netbsd"
#else
#define HELPER_OS "unknown"
#endif
#if defined(__x86_64__) || defined(_M_X64)
#define HELPER_ARCH "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define HELPER_ARCH "aarch64"
#elif defined(__i386__) || defined(_M_IX86)
#define HELPER_ARCH "x86"
#elif defined(__arm__)
#define HELPER_ARCH "arm"
#elif defined(__riscv) && __riscv_xlen == 64
#define HELPER_ARCH "riscv64"
#elif defined(__powerpc64__) && defined(__LITTLE_ENDIAN__)
#define HELPER_ARCH "ppc64le"
#elif defined(__s390x__)
#define HELPER_ARCH "s390x"
#elif defined(__loongarch64)
#define HELPER_ARCH "loongarch64"
#else
#define HELPER_ARCH "unknown"
#endif
    return HELPER_OS "-" HELPER_ARCH;
}

Server::Server(std::FILE *in, std::FILE *out)
    : m_in(in)
    , m_out(out)
{
}

Server::~Server() = default;

void Server::send(const Json &message)
{
    std::string line = message.dump();
    line += '\n';
    std::lock_guard lock(m_outMutex);
    if (m_outBroken) {
        return;
    }
    if (std::fwrite(line.data(), 1, line.size(), m_out) != line.size() || std::fflush(m_out) != 0) {
        m_outBroken = true;
    }
}

Json Server::dispatch(const std::string &method, const Json &params)
{
    if (method == "stat") {
        return methodStat(params);
    }
    if (method == "read") {
        return methodRead(params);
    }
    if (method == "write") {
        return methodWrite(params);
    }
    if (method == "digest") {
        return methodDigest(params);
    }
    if (method == "watch") {
        const std::vector<std::string> paths = requirePaths(params);
        if (!m_watcher) {
            m_watcher = std::make_unique<Watcher>([this](const std::vector<std::string> &changed) {
                Json list = Json(Json::Array());
                for (const std::string &path : changed) {
                    list.push(path);
                }
                Json p;
                p.set("paths", std::move(list));
                Json notification;
                notification.set("jsonrpc", "2.0");
                notification.set("method", "changed");
                notification.set("params", std::move(p));
                send(notification);
            });
        }
        Json failed = Json(Json::Array());
        for (const auto &[path, error] : m_watcher->add(paths)) {
            Json item;
            item.set("path", path);
            item.set("error", error.name);
            item.set("message", error.message);
            failed.push(std::move(item));
        }
        Json result;
        result.set("failed", std::move(failed));
        return result;
    }
    if (method == "unwatch") {
        const std::vector<std::string> paths = requirePaths(params);
        if (m_watcher) {
            m_watcher->remove(paths);
        }
        return Json(Json::Object());
    }
    throw Failure{-32601, "Method not found: " + method, Json()};
}

Json Server::handle(const Json &message)
{
    if (!message.isObject()) {
        return errorResponse(Json(), -32600, "Invalid request");
    }
    const Json *idValue = message.find("id");
    const Json *methodValue = message.find("method");
    const Json id = idValue ? *idValue : Json();
    if (!methodValue || !methodValue->isString() || (idValue && !idValue->isNumber() && !idValue->isString() && !idValue->isNull())) {
        return idValue ? errorResponse(id, -32600, "Invalid request") : Json();
    }
    if (!idValue) {
        return Json(); // a notification: none are defined for the client yet
    }
    const Json *paramsValue = message.find("params");
    const Json params = paramsValue ? *paramsValue : Json(Json::Object());
    try {
        if (!params.isObject()) {
            throw invalidParams("\"params\" must be an object");
        }
        Json response;
        response.set("jsonrpc", "2.0");
        response.set("id", id);
        response.set("result", dispatch(methodValue->toString(), params));
        return response;
    } catch (const Failure &failure) {
        return errorResponse(id, failure.code, failure.message, failure.data);
    } catch (const std::bad_alloc &) {
        return errorResponse(id, 1, "Out of memory", outOfMemoryData());
    } catch (const std::length_error &) { // a string beyond max_size() (32-bit)
        return errorResponse(id, 1, "Out of memory", outOfMemoryData());
    }
}

int Server::run()
{
    {
        Json capabilities = Json(Json::Array());
        for (const char *name : {"stat", "read", "write", "digest", "watch"}) {
            capabilities.push(name);
        }
        Json params;
        params.set("protocol", ProtocolVersion);
        params.set("version", helperVersion());
        params.set("platform", platformName());
        params.set("capabilities", std::move(capabilities));
        Json hello;
        hello.set("jsonrpc", "2.0");
        hello.set("method", "hello");
        hello.set("params", std::move(params));
        send(hello);
    }

    std::string buffer;
    size_t scanned = 0;
    bool discarding = false; // the rest of an over-long line
    char chunk[65536];
    while (true) {
        size_t newline;
        while ((newline = buffer.find('\n', scanned)) != std::string::npos) {
            std::string line;
            if (newline + 1 == buffer.size()) {
                line.swap(buffer); // the usual case: no copy of a long line
                line.pop_back();
            } else {
                try {
                    line = buffer.substr(0, newline);
                } catch (const std::bad_alloc &) {
                    if (!discarding) {
                        send(errorResponse(Json(), 1, "Out of memory", outOfMemoryData()));
                    }
                    discarding = true;
                }
                buffer.erase(0, newline + 1);
            }
            scanned = 0;
            if (discarding) {
                discarding = false;
                continue;
            }
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.find_first_not_of(" \t") == std::string::npos) {
                continue;
            }
            std::optional<Json> message;
            bool outOfMemory = false;
            try {
                message = Json::parse(line);
                line.clear();
                line.shrink_to_fit();
                const Json response = message ? handle(*message) : errorResponse(Json(), -32700, "Parse error");
                if (!response.isNull()) {
                    send(response);
                }
            } catch (const std::bad_alloc &) {
                outOfMemory = true;
            } catch (const std::length_error &) {
                outOfMemory = true;
            }
            if (outOfMemory) {
                // Parsing a huge line, or encoding a huge response (a 32-bit
                // host's address space): answer instead of aborting.
                line.clear();
                line.shrink_to_fit();
                const Json *idValue = (message && message->isObject()) ? message->find("id") : nullptr;
                const Json id = (idValue && (idValue->isString() || idValue->isNumber())) ? *idValue : Json();
                message.reset();
                send(errorResponse(id, 1, "Out of memory", outOfMemoryData()));
            }
        }
        scanned = buffer.size();
        if (buffer.size() > MaxLine) {
            if (!discarding) {
                send(errorResponse(Json(), -32600, "Line too long"));
            }
            discarding = true;
            buffer.clear();
            scanned = 0;
        }
        {
            std::lock_guard lock(m_outMutex);
            if (m_outBroken) {
                break;
            }
        }
        const long n = readSome(m_in, chunk, sizeof chunk);
        if (n <= 0) {
            break; // end of input (or an error): the session is over
        }
        try {
            buffer.append(chunk, size_t(n));
        } catch (const std::bad_alloc &) {
            // A line longer than the memory left (a 32-bit host): refused
            // like an over-long one.
            if (!discarding) {
                send(errorResponse(Json(), 1, "Out of memory", outOfMemoryData()));
            }
            discarding = true;
            buffer.clear();
            buffer.shrink_to_fit();
            scanned = 0;
            if (const char *end = static_cast<const char *>(std::memchr(chunk, '\n', size_t(n)))) {
                // The line ended in this chunk: keep what follows it.
                discarding = false;
                buffer.assign(end + 1, size_t(chunk + n - (end + 1)));
            }
        }
    }
    m_watcher.reset();
    std::lock_guard lock(m_outMutex);
    return m_outBroken ? 1 : 0;
}

}
