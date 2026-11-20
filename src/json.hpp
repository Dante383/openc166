// SPDX-License-Identifier: MIT
// A small JSON reader for configuration files (board definitions). It
// accepts standard JSON plus // and /* */ comments, and reports errors with
// a line and column.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oc166::json {

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Value {
public:
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Kind kind() const { return kind_; }
    bool is_null() const { return kind_ == Kind::Null; }
    bool is_bool() const { return kind_ == Kind::Bool; }
    bool is_number() const { return kind_ == Kind::Number; }
    bool is_string() const { return kind_ == Kind::String; }
    bool is_array() const { return kind_ == Kind::Array; }
    bool is_object() const { return kind_ == Kind::Object; }

    bool as_bool() const;
    double as_number() const;
    // The number as an integer; throws if it has a fraction.
    std::int64_t as_int() const;
    const std::string& as_string() const;
    const std::vector<Value>& as_array() const;
    // Members in file order.
    const std::vector<std::pair<std::string, Value>>& as_object() const;

    // An object's member, or nullptr.
    const Value* find(std::string_view key) const;
    // Where the value starts in the text: "line 3, column 7".
    std::string where() const;

    static Value make_null(int line, int column);
    static Value make_bool(bool v, int line, int column);
    static Value make_number(double v, bool integral, std::int64_t i, int line, int column);
    static Value make_string(std::string v, int line, int column);
    static Value make_array(std::vector<Value> v, int line, int column);
    static Value make_object(std::vector<std::pair<std::string, Value>> v, int line, int column);

private:
    Kind kind_ = Kind::Null;
    bool bool_ = false;
    double number_ = 0;
    bool integral_ = false;
    std::int64_t int_ = 0;
    std::string string_;
    std::shared_ptr<std::vector<Value>> array_;
    std::shared_ptr<std::vector<std::pair<std::string, Value>>> object_;
    int line_ = 0;
    int column_ = 0;
};

// Parses a whole document. Throws json::Error with "line L, column C: ...".
Value parse(std::string_view text);

}  // namespace oc166::json
