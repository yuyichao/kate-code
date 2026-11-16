/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

// kate-code-helper: Kate Code's helper on the hosts of remote folders.
// See PROTOCOL.md.

#include "Server.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <csignal>
#endif

namespace
{
void usage(std::FILE *out)
{
    std::fprintf(out,
                 "Usage: kate-code-helper [--version]\n"
                 "\n"
                 "Kate Code's helper on the host of a remote folder: serves Kate Code's\n"
                 "protocol (newline-delimited JSON-RPC) on stdin/stdout until end of input.\n"
                 "Started by Kate Code over ssh; not meant to be run by hand.\n");
}
}

int main(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("kate-code-helper %s protocol %d\n", helper::helperVersion(), helper::ProtocolVersion);
        return 0;
    }
    if (argc == 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        usage(stdout);
        return 0;
    }
    if (argc != 1) {
        usage(stderr);
        return 2;
    }
#ifdef _WIN32
    // Bytes, not text: no CRLF translation, no ^Z as end of file.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#else
    // A closed stdout ends the session through write errors.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    helper::Server server(stdin, stdout);
    return server.run();
}
