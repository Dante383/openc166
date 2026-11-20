// SPDX-License-Identifier: MIT
#include "json.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace oc166::json {

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : t_(text) {}

    Value document() {
        skip();
        Value v = value();
        skip();
        if (p_ < t_.size()) fail("unexpected text after the end of the document");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& msg) const {
        throw Error("line " + std::to_string(line_) + ", column " + std::to_string(col_) + ": " + msg);
    }

    char peek() const { return p_ < t_.size() ? t_[p_] : '\0'; }
    char next() {
        const char c = t_[p_++];
        if (c == '\n') {
            ++line_;
            col_ = 1;
        } else {
            ++col_;
        }
        return c;
    }

    // Whitespace and comments.
    void skip() {
        while (p_ < t_.size()) {
            const char c = t_[p_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                next();
            } else if (c == '/' && p_ + 1 < t_.size() && t_[p_ + 1] == '/') {
                while (p_ < t_.size() && t_[p_] != '\n') next();
            } else if (c == '/' && p_ + 1 < t_.size() && t_[p_ + 1] == '*') {
                next();
                next();
                while (p_ + 1 < t_.size() && !(t_[p_] == '*' && t_[p_ + 1] == '/')) next();
                if (p_ + 1 >= t_.size()) fail("unterminated comment");
                next();
                next();
            } else {
                break;
            }
        }
    }

    void expect(char c) {
        if (peek() != c) fail(std::string("expected '") + c + "'");
        next();
    }

    Value value() {
        const int line = line_, col = col_;
        const char c = peek();
        if (c == '{') return object(line, col);
        if (c == '[') return array(line, col);
        if (c == '"') return Value::make_string(string(), line, col);
        if (c == '-' || (c >= '0' && c <= '9')) return number(line, col);
        if (word("true")) return Value::make_bool(true, line, col);
        if (word("false")) return Value::make_bool(false, line, col);
        if (word("null")) return Value::make_null(line, col);
        if (p_ >= t_.size()) fail("unexpected end of the document");
        fail(std::string("unexpected character '") + c + "'");
    }

    bool word(std::string_view w) {
        if (t_.substr(p_, w.size()) != w) return false;
        for (std::size_t i = 0; i < w.size(); ++i) next();
        return true;
    }

    Value object(int line, int col) {
        expect('{');
        std::vector<std::pair<std::string, Value>> members;
        skip();
        if (peek() == '}') {
            next();
            return Value::make_object(std::move(members), line, col);
        }
        while (true) {
            skip();
            if (peek() != '"') fail("expected a member name in quotes");
            std::string key = string();
            for (const auto& m : members)
                if (m.first == key) fail("duplicate member \"" + key + "\"");
            skip();
            expect(':');
            skip();
            members.emplace_back(std::move(key), value());
            skip();
            if (peek() == ',') {
                next();
                skip();
                if (peek() == '}') fail("a comma before '}'");
                continue;
            }
            expect('}');
            return Value::make_object(std::move(members), line, col);
        }
    }

    Value array(int line, int col) {
        expect('[');
        std::vector<Value> items;
        skip();
        if (peek() == ']') {
            next();
            return Value::make_array(std::move(items), line, col);
        }
        while (true) {
            skip();
            items.push_back(value());
            skip();
            if (peek() == ',') {
                next();
                skip();
                if (peek() == ']') fail("a comma before ']'");
                continue;
            }
            expect(']');
            return Value::make_array(std::move(items), line, col);
        }
    }

    static void utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::uint32_t hex4() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = peek();
            int d = -1;
            if (c >= '0' && c <= '9') d = c - '0';
            if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            if (d < 0) fail("expected four hex digits after \\u");
            next();
            v = v * 16 + static_cast<std::uint32_t>(d);
        }
        return v;
    }

    std::string string() {
        expect('"');
        std::string out;
        while (true) {
            if (p_ >= t_.size()) fail("unterminated string");
            const char c = next();
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in a string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p_ >= t_.size()) fail("unterminated string");
            const char e = next();
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t cp = hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && word("\\u")) {
                    const std::uint32_t lo = hex4();
                    if (lo < 0xDC00 || lo >= 0xE000) fail("bad surrogate pair");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, cp);
                break;
            }
            default: fail(std::string("unknown escape \\") + e);
            }
        }
    }

    Value number(int line, int col) {
        const std::size_t start = p_;
        if (peek() == '-') next();
        if (peek() == '0') {
            next();
        } else if (peek() >= '1' && peek() <= '9') {
            while (peek() >= '0' && peek() <= '9') next();
        } else {
            fail("bad number");
        }
        bool integral = true;
        if (peek() == '.') {
            integral = false;
            next();
            if (!(peek() >= '0' && peek() <= '9')) fail("bad number");
            while (peek() >= '0' && peek() <= '9') next();
        }
        if (peek() == 'e' || peek() == 'E') {
            integral = false;
            next();
            if (peek() == '+' || peek() == '-') next();
            if (!(peek() >= '0' && peek() <= '9')) fail("bad number");
            while (peek() >= '0' && peek() <= '9') next();
        }
        const std::string text(t_.substr(start, p_ - start));
        const double d = std::strtod(text.c_str(), nullptr);
        std::int64_t i = 0;
        if (integral) {
            errno = 0;
            i = std::strtoll(text.c_str(), nullptr, 10);
            if (errno == ERANGE) integral = false;
        }
        return Value::make_number(d, integral, i, line, col);
    }

    std::string_view t_;
    std::size_t p_ = 0;
    int line_ = 1;
    int col_ = 1;
};

}  // namespace

bool Value::as_bool() const {
    if (kind_ != Kind::Bool) throw Error(where() + ": expected true or false");
    return bool_;
}

double Value::as_number() const {
    if (kind_ != Kind::Number) throw Error(where() + ": expected a number");
    return number_;
}

std::int64_t Value::as_int() const {
    if (kind_ != Kind::Number || !integral_) throw Error(where() + ": expected a whole number");
    return int_;
}

const std::string& Value::as_string() const {
    if (kind_ != Kind::String) throw Error(where() + ": expected a string");
    return string_;
}

const std::vector<Value>& Value::as_array() const {
    if (kind_ != Kind::Array) throw Error(where() + ": expected an array");
    return *array_;
}

const std::vector<std::pair<std::string, Value>>& Value::as_object() const {
    if (kind_ != Kind::Object) throw Error(where() + ": expected an object");
    return *object_;
}

const Value* Value::find(std::string_view key) const {
    if (kind_ != Kind::Object) return nullptr;
    for (const auto& [k, v] : *object_)
        if (k == key) return &v;
    return nullptr;
}

std::string Value::where() const {
    return "line " + std::to_string(line_) + ", column " + std::to_string(column_);
}

Value Value::make_null(int line, int column) {
    Value v;
    v.line_ = line;
    v.column_ = column;
    return v;
}

Value Value::make_bool(bool b, int line, int column) {
    Value v = make_null(line, column);
    v.kind_ = Kind::Bool;
    v.bool_ = b;
    return v;
}

Value Value::make_number(double d, bool integral, std::int64_t i, int line, int column) {
    Value v = make_null(line, column);
    v.kind_ = Kind::Number;
    v.number_ = d;
    v.integral_ = integral;
    v.int_ = i;
    return v;
}

Value Value::make_string(std::string s, int line, int column) {
    Value v = make_null(line, column);
    v.kind_ = Kind::String;
    v.string_ = std::move(s);
    return v;
}

Value Value::make_array(std::vector<Value> a, int line, int column) {
    Value v = make_null(line, column);
    v.kind_ = Kind::Array;
    v.array_ = std::make_shared<std::vector<Value>>(std::move(a));
    return v;
}

Value Value::make_object(std::vector<std::pair<std::string, Value>> o, int line, int column) {
    Value v = make_null(line, column);
    v.kind_ = Kind::Object;
    v.object_ = std::make_shared<std::vector<std::pair<std::string, Value>>>(std::move(o));
    return v;
}

Value parse(std::string_view text) { return Parser(text).document(); }

}  // namespace oc166::json
