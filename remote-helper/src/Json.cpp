/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace helper
{

namespace
{
const Json::Array emptyArray;
const Json::Object emptyObject;
constexpr int MaxDepth = 64;

class Parser
{
public:
    explicit Parser(const std::string &text)
        : m_text(text)
    {
    }

    std::optional<Json> parseDocument()
    {
        skipSpace();
        Json value;
        if (!parseValue(value, 0)) {
            return std::nullopt;
        }
        skipSpace();
        if (m_pos != m_text.size()) {
            return std::nullopt;
        }
        return value;
    }

private:
    void skipSpace()
    {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++m_pos;
        }
    }

    bool literal(const char *word)
    {
        size_t i = 0;
        for (; word[i]; ++i) {
            if (m_pos + i >= m_text.size() || m_text[m_pos + i] != word[i]) {
                return false;
            }
        }
        m_pos += i;
        return true;
    }

    bool parseValue(Json &out, int depth)
    {
        if (depth > MaxDepth || m_pos >= m_text.size()) {
            return false;
        }
        const char c = m_text[m_pos];
        switch (c) {
        case 'n':
            if (!literal("null")) {
                return false;
            }
            out = Json();
            return true;
        case 't':
            if (!literal("true")) {
                return false;
            }
            out = Json(true);
            return true;
        case 'f':
            if (!literal("false")) {
                return false;
            }
            out = Json(false);
            return true;
        case '"': {
            std::string s;
            if (!parseString(s)) {
                return false;
            }
            out = Json(std::move(s));
            return true;
        }
        case '[':
            return parseArray(out, depth);
        case '{':
            return parseObject(out, depth);
        default:
            return parseNumber(out);
        }
    }

    bool parseNumber(Json &out)
    {
        const size_t start = m_pos;
        if (m_pos < m_text.size() && m_text[m_pos] == '-') {
            ++m_pos;
        }
        auto digits = [this] {
            const size_t from = m_pos;
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') {
                ++m_pos;
            }
            return m_pos - from;
        };
        if (m_pos < m_text.size() && m_text[m_pos] == '0') {
            ++m_pos;
        } else if (digits() == 0) {
            return false;
        }
        if (m_pos < m_text.size() && m_text[m_pos] == '.') {
            ++m_pos;
            if (digits() == 0) {
                return false;
            }
        }
        if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E')) {
            ++m_pos;
            if (m_pos < m_text.size() && (m_text[m_pos] == '+' || m_text[m_pos] == '-')) {
                ++m_pos;
            }
            if (digits() == 0) {
                return false;
            }
        }
        // strtod depends on the C locale's decimal point; the helper never
        // calls setlocale(), so it is ".".
        const std::string number = m_text.substr(start, m_pos - start);
        char *end = nullptr;
        const double value = std::strtod(number.c_str(), &end);
        if (end != number.c_str() + number.size() || !std::isfinite(value)) {
            return false;
        }
        out = Json(value);
        return true;
    }

    bool hex4(unsigned &out)
    {
        if (m_pos + 4 > m_text.size()) {
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_pos++];
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out |= unsigned(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= unsigned(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= unsigned(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    static void appendUtf8(std::string &s, unsigned cp)
    {
        if (cp < 0x80) {
            s += char(cp);
        } else if (cp < 0x800) {
            s += char(0xC0 | (cp >> 6));
            s += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += char(0xE0 | (cp >> 12));
            s += char(0x80 | ((cp >> 6) & 0x3F));
            s += char(0x80 | (cp & 0x3F));
        } else {
            s += char(0xF0 | (cp >> 18));
            s += char(0x80 | ((cp >> 12) & 0x3F));
            s += char(0x80 | ((cp >> 6) & 0x3F));
            s += char(0x80 | (cp & 0x3F));
        }
    }

    bool parseString(std::string &out)
    {
        ++m_pos; // opening quote
        while (m_pos < m_text.size()) {
            const unsigned char c = static_cast<unsigned char>(m_text[m_pos]);
            if (c == '"') {
                ++m_pos;
                return true;
            }
            if (c < 0x20) {
                return false;
            }
            if (c != '\\') {
                // Copy a run of plain bytes at once (file contents are long).
                const size_t from = m_pos;
                while (m_pos < m_text.size()) {
                    const unsigned char d = static_cast<unsigned char>(m_text[m_pos]);
                    if (d == '"' || d == '\\' || d < 0x20) {
                        break;
                    }
                    ++m_pos;
                }
                out.append(m_text, from, m_pos - from);
                continue;
            }
            ++m_pos;
            if (m_pos >= m_text.size()) {
                return false;
            }
            const char e = m_text[m_pos++];
            switch (e) {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                unsigned cp = 0;
                if (!hex4(cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp < 0xDC00) {
                    unsigned low = 0;
                    if (m_pos + 2 > m_text.size() || m_text[m_pos] != '\\' || m_text[m_pos + 1] != 'u') {
                        return false;
                    }
                    m_pos += 2;
                    if (!hex4(low) || low < 0xDC00 || low >= 0xE000) {
                        return false;
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp < 0xE000) {
                    return false;
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool parseArray(Json &out, int depth)
    {
        ++m_pos;
        Json::Array array;
        skipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == ']') {
            ++m_pos;
            out = Json(std::move(array));
            return true;
        }
        while (true) {
            skipSpace();
            Json item;
            if (!parseValue(item, depth + 1)) {
                return false;
            }
            array.push_back(std::move(item));
            skipSpace();
            if (m_pos >= m_text.size()) {
                return false;
            }
            const char c = m_text[m_pos++];
            if (c == ']') {
                break;
            }
            if (c != ',') {
                return false;
            }
        }
        out = Json(std::move(array));
        return true;
    }

    bool parseObject(Json &out, int depth)
    {
        ++m_pos;
        Json::Object object;
        skipSpace();
        if (m_pos < m_text.size() && m_text[m_pos] == '}') {
            ++m_pos;
            out = Json(std::move(object));
            return true;
        }
        while (true) {
            skipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != '"') {
                return false;
            }
            std::string key;
            if (!parseString(key)) {
                return false;
            }
            skipSpace();
            if (m_pos >= m_text.size() || m_text[m_pos] != ':') {
                return false;
            }
            ++m_pos;
            skipSpace();
            Json value;
            if (!parseValue(value, depth + 1)) {
                return false;
            }
            object[std::move(key)] = std::move(value);
            skipSpace();
            if (m_pos >= m_text.size()) {
                return false;
            }
            const char c = m_text[m_pos++];
            if (c == '}') {
                break;
            }
            if (c != ',') {
                return false;
            }
        }
        out = Json(std::move(object));
        return true;
    }

    const std::string &m_text;
    size_t m_pos = 0;
};

void dumpString(std::string &out, const std::string &s)
{
    out += '"';
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += ch;
            }
        }
    }
    out += '"';
}
}

Json::Json(bool value)
    : m_type(Type::Bool)
    , m_bool(value)
{
}

Json::Json(int value)
    : m_type(Type::Number)
    , m_number(value)
{
}

Json::Json(int64_t value)
    : m_type(Type::Number)
    , m_number(double(value))
{
}

Json::Json(uint64_t value)
    : m_type(Type::Number)
    , m_number(double(value))
{
}

Json::Json(double value)
    : m_type(Type::Number)
    , m_number(value)
{
}

Json::Json(const char *value)
    : m_type(Type::String)
    , m_string(value)
{
}

Json::Json(std::string value)
    : m_type(Type::String)
    , m_string(std::move(value))
{
}

Json::Json(Array value)
    : m_type(Type::Array)
    , m_array(std::make_shared<Array>(std::move(value)))
{
}

Json::Json(Object value)
    : m_type(Type::Object)
    , m_object(std::make_shared<Object>(std::move(value)))
{
}

const Json::Array &Json::toArray() const
{
    return m_array ? *m_array : emptyArray;
}

const Json::Object &Json::toObject() const
{
    return m_object ? *m_object : emptyObject;
}

const Json *Json::find(const std::string &key) const
{
    if (!m_object) {
        return nullptr;
    }
    const auto it = m_object->find(key);
    return it == m_object->end() ? nullptr : &it->second;
}

Json &Json::set(const std::string &key, Json value)
{
    if (m_type != Type::Object) {
        *this = Json(Object());
    } else if (m_object.use_count() > 1) {
        m_object = std::make_shared<Object>(*m_object);
    }
    return (*m_object)[key] = std::move(value);
}

void Json::push(Json value)
{
    if (m_type != Type::Array) {
        *this = Json(Array());
    } else if (m_array.use_count() > 1) {
        m_array = std::make_shared<Array>(*m_array);
    }
    m_array->push_back(std::move(value));
}

std::string Json::dump() const
{
    std::string out;
    dumpTo(out);
    return out;
}

void Json::dumpTo(std::string &out) const
{
    switch (m_type) {
    case Type::Null:
        out += "null";
        break;
    case Type::Bool:
        out += m_bool ? "true" : "false";
        break;
    case Type::Number: {
        char buf[32];
        if (std::nearbyint(m_number) == m_number && std::fabs(m_number) < 9007199254740992.0) {
            std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(m_number));
        } else {
            std::snprintf(buf, sizeof buf, "%.17g", m_number);
        }
        out += buf;
        break;
    }
    case Type::String:
        dumpString(out, m_string);
        break;
    case Type::Array: {
        out += '[';
        bool first = true;
        for (const Json &item : toArray()) {
            if (!first) {
                out += ',';
            }
            first = false;
            item.dumpTo(out);
        }
        out += ']';
        break;
    }
    case Type::Object: {
        out += '{';
        bool first = true;
        for (const auto &[key, value] : toObject()) {
            if (!first) {
                out += ',';
            }
            first = false;
            dumpString(out, key);
            out += ':';
            value.dumpTo(out);
        }
        out += '}';
        break;
    }
    }
}

std::optional<Json> Json::parse(const std::string &text)
{
    return Parser(text).parseDocument();
}

}
