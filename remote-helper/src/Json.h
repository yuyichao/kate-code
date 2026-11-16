/*
    SPDX-License-Identifier: MIT
    SPDX-FileCopyrightText: 2026 Kate Code contributors
*/

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace helper
{

// A JSON value: just what the protocol needs (objects keep sorted keys;
// numbers are doubles, integers up to 2^53 exact).
class Json
{
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json() = default;
    Json(std::nullptr_t) { }
    Json(bool value);
    Json(int value);
    Json(int64_t value);
    Json(uint64_t value);
    Json(double value);
    Json(const char *value);
    Json(std::string value);
    Json(Array value);
    Json(Object value);

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    bool toBool() const { return m_bool; }
    double toNumber() const { return m_number; }
    const std::string &toString() const { return m_string; }
    const Array &toArray() const;
    const Object &toObject() const;

    // Object member, or nullptr (also for non-objects).
    const Json *find(const std::string &key) const;
    // Sets an object member (turns a null value into an object).
    Json &set(const std::string &key, Json value);
    // Appends to an array (turns a null value into an array).
    void push(Json value);

    // Compact JSON on one line (non-ASCII characters as UTF-8; control
    // characters escaped).
    std::string dump() const;
    void dumpTo(std::string &out) const;

    // nullopt for anything but exactly one JSON value (whitespace around
    // it allowed); nesting is limited.
    static std::optional<Json> parse(const std::string &text);

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0;
    std::string m_string;
    std::shared_ptr<Array> m_array;
    std::shared_ptr<Object> m_object;
};

}
