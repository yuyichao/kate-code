/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#include "Codec.h"

#include <algorithm>
#include <cstring>

namespace helper
{

namespace
{
uint32_t rotl(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

const char Base64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64Value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}
}

Sha1::Sha1()
    : m_h{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u}
{
}

void Sha1::block(const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | uint32_t(p[4 * i + 3]);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = m_h[0], b = m_h[1], c = m_h[2], d = m_h[3], e = m_h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const uint32_t t = rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = t;
    }
    m_h[0] += a;
    m_h[1] += b;
    m_h[2] += c;
    m_h[3] += d;
    m_h[4] += e;
}

void Sha1::add(const void *data, size_t size)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    m_length += size;
    if (m_buffered) {
        const size_t n = std::min(size, sizeof m_buffer - m_buffered);
        std::memcpy(m_buffer + m_buffered, p, n);
        m_buffered += n;
        p += n;
        size -= n;
        if (m_buffered < sizeof m_buffer) {
            return;
        }
        block(m_buffer);
        m_buffered = 0;
    }
    while (size >= 64) {
        block(p);
        p += 64;
        size -= 64;
    }
    std::memcpy(m_buffer, p, size);
    m_buffered = size;
}

std::string Sha1::hexDigest()
{
    const uint64_t bits = m_length * 8;
    const uint8_t pad = 0x80;
    add(&pad, 1);
    const uint8_t zero = 0;
    while (m_buffered != 56) {
        add(&zero, 1);
    }
    uint8_t length[8];
    for (int i = 0; i < 8; ++i) {
        length[i] = uint8_t(bits >> (56 - 8 * i));
    }
    add(length, 8);
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (const uint32_t v : m_h) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            out += hex[(v >> shift) & 0xF];
        }
    }
    return out;
}

std::string gitBlobDigest(const std::string &data)
{
    Sha1 sha;
    const std::string header = "blob " + std::to_string(data.size());
    sha.add(header.data(), header.size() + 1); // with the terminating NUL
    sha.add(data);
    return sha.hexDigest();
}

std::string base64Encode(const std::string &data)
{
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const uint32_t v = uint32_t(uint8_t(data[i])) << 16 | uint32_t(uint8_t(data[i + 1])) << 8 | uint8_t(data[i + 2]);
        out += Base64Alphabet[v >> 18];
        out += Base64Alphabet[(v >> 12) & 63];
        out += Base64Alphabet[(v >> 6) & 63];
        out += Base64Alphabet[v & 63];
    }
    if (i + 1 == data.size()) {
        const uint32_t v = uint32_t(uint8_t(data[i])) << 16;
        out += Base64Alphabet[v >> 18];
        out += Base64Alphabet[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == data.size()) {
        const uint32_t v = uint32_t(uint8_t(data[i])) << 16 | uint32_t(uint8_t(data[i + 1])) << 8;
        out += Base64Alphabet[v >> 18];
        out += Base64Alphabet[(v >> 12) & 63];
        out += Base64Alphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

std::optional<std::string> base64Decode(const std::string &text)
{
    if (text.size() % 4) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        int v[4];
        int padding = 0;
        for (int j = 0; j < 4; ++j) {
            const unsigned char c = static_cast<unsigned char>(text[i + j]);
            if (c == '=' && last && j >= 2) {
                v[j] = 0;
                ++padding;
                continue;
            }
            if (padding) {
                return std::nullopt; // data after '='
            }
            v[j] = base64Value(c);
            if (v[j] < 0) {
                return std::nullopt;
            }
        }
        const uint32_t n = uint32_t(v[0]) << 18 | uint32_t(v[1]) << 12 | uint32_t(v[2]) << 6 | uint32_t(v[3]);
        out += char(n >> 16);
        if (padding < 2) {
            out += char((n >> 8) & 0xFF);
        } else if (n & 0xFFFF) {
            return std::nullopt; // non-canonical
        }
        if (padding < 1) {
            out += char(n & 0xFF);
        } else if (n & 0xFF) {
            return std::nullopt;
        }
    }
    return out;
}

}
