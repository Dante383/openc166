// SPDX-License-Identifier: MIT
// Diagnostics. Codes follow Keil A166 numbering (e.g. "A62") where an
// equivalent Keil message exists, so results can be compared line by line.
#pragma once

#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace oc166 {

enum class Severity : unsigned char { Warning, Error };

struct Diagnostic {
    Severity severity = Severity::Error;
    std::string code;
    std::string message;
    std::string file;
    int line = 0;    // 1-based, 0 if not tied to a line
    int column = 0;  // 1-based, 0 if unknown
};

// Thrown to abandon the current statement.
class AsmError : public std::exception {
public:
    AsmError(std::string code, std::string message, int column = 0)
        : code_(std::move(code)), message_(std::move(message)), column_(column) {}
    const char* what() const noexcept override { return message_.c_str(); }
    const std::string& code() const { return code_; }
    const std::string& message() const { return message_; }
    int column() const { return column_; }

private:
    std::string code_;
    std::string message_;
    int column_;
};

// Collects warnings and non-fatal errors for one source line.
class Reporter {
public:
    void warning(std::string code, std::string message, int column = 0) {
        items_.push_back({Severity::Warning, std::move(code), std::move(message), {}, 0, column});
    }
    void error(std::string code, std::string message, int column = 0) {
        items_.push_back({Severity::Error, std::move(code), std::move(message), {}, 0, column});
    }
    std::vector<Diagnostic>& items() { return items_; }
    bool has_errors() const {
        for (const auto& d : items_)
            if (d.severity == Severity::Error) return true;
        return false;
    }

private:
    std::vector<Diagnostic> items_;
};

}  // namespace oc166
