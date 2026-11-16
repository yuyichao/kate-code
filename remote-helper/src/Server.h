/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#pragma once

#include "Json.h"

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

namespace helper
{

class Watcher;

constexpr int ProtocolVersion = 1;
// "linux-x86_64", "darwin-aarch64", "windows-x86_64", ...
const char *platformName();
const char *helperVersion();

// The protocol (PROTOCOL.md) on a pair of streams: requests from in,
// answered in order; notifications from other threads in between.
class Server
{
public:
    Server(std::FILE *in, std::FILE *out);
    ~Server();

    // Until end of input (or a broken output); the exit code.
    int run();

    // One request (a parsed line) -> its response, or a null Json for a
    // notification (no answer).
    Json handle(const Json &message);

    // Lines longer than this are refused (PROTOCOL.md: at least 128 MiB).
    // Less on 32-bit hosts: a line, its parsed copy and the decoded data
    // must fit in the address space together.
    static constexpr size_t MaxLine = sizeof(void *) >= 8 ? size_t(512) << 20 : size_t(192) << 20;

private:
    void send(const Json &message);
    Json dispatch(const std::string &method, const Json &params);

    std::FILE *m_in;
    std::FILE *m_out;
    std::mutex m_outMutex;
    bool m_outBroken = false;
    std::unique_ptr<Watcher> m_watcher;
};

}
