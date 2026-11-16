/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace helper
{

class Sha1
{
public:
    Sha1();
    void add(const void *data, size_t size);
    void add(const std::string &data) { add(data.data(), data.size()); }
    // Lower-case hex; the object is spent afterwards.
    std::string hexDigest();

private:
    void block(const uint8_t *p);

    std::array<uint32_t, 5> m_h;
    uint8_t m_buffer[64];
    size_t m_buffered = 0;
    uint64_t m_length = 0;
};

// SHA-1 of the data as a git blob ("blob <size>\0" + data), in hex.
std::string gitBlobDigest(const std::string &data);

std::string base64Encode(const std::string &data);
// nullopt for anything but canonical padded base64 (RFC 4648 alphabet).
std::optional<std::string> base64Decode(const std::string &text);

}
