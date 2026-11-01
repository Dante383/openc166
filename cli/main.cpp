// SPDX-License-Identifier: MIT
// openc166 command-line tool.
#include "assembler.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr const char* kUsage = R"(usage: openc166 [options] SOURCE

Assembles Keil A166 assembly for the Infineon C166/C167 into a raw binary.

options:
  -o, --output FILE   binary output (default: SOURCE with extension .bin)
  -l, --listing FILE  write a listing
      --cpu CPU       c166 (default) or c167; $MOD167 in the source also selects c167
      --origin ADDR   address of code outside sections and of sections without AT (default 0)
      --base ADDR     address of the first byte of the binary (default: lowest address used)
      --fill BYTE     value of unused bytes inside the binary (default 0FFh)
  -h, --help          show this help
      --version       show the version

Numbers may be written as 0C000h, 0xC000 or decimal.

exit status: 0 success, 1 assembly errors, 2 usage or file errors
)";

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::int64_t parse_number(std::string_view s, const char* what) {
    std::string t(s);
    int base = 10;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
        t = t.substr(2);
        base = 16;
    } else if (t.size() > 1 && (t.back() == 'h' || t.back() == 'H')) {
        t.pop_back();
        base = 16;
    }
    std::size_t used = 0;
    long long v = 0;
    try {
        v = std::stoll(t, &used, base);
    } catch (const std::exception&) {
        used = 0;
    }
    if (t.empty() || used != t.size() || v < 0)
        throw UsageError(std::string("invalid ") + what + " '" + std::string(s) + "'");
    return v;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open '" + path + "'");
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_file(const std::string& path, const char* data, std::size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(data, static_cast<std::streamsize>(size)))
        throw std::runtime_error("cannot write '" + path + "'");
}

std::string replace_extension(const std::string& path, const std::string& ext) {
    std::size_t slash = path.find_last_of("/\\");
    std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path + ext;
    return path.substr(0, dot) + ext;
}

int run(int argc, char** argv) {
    oc166::AssembleOptions opt;
    std::string input, output, listing;
    std::optional<std::int64_t> base;
    std::uint8_t fill = 0xFF;

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto value = [&]() -> std::string_view {
            if (auto eq = a.find('='); a.starts_with("--") && eq != std::string_view::npos) return a.substr(eq + 1);
            if (i + 1 >= argc) throw UsageError("option " + std::string(a) + " needs a value");
            return argv[++i];
        };
        auto is = [&](std::string_view s, std::string_view l = {}) {
            return a == s || (!l.empty() && (a == l || (a.starts_with(l) && a.size() > l.size() && a[l.size()] == '=')));
        };
        if (is("-h", "--help")) {
            std::cout << kUsage;
            return 0;
        } else if (a == "--version") {
            std::cout << "openc166 " << OPENC166_VERSION << "\n";
            return 0;
        } else if (is("-o", "--output")) {
            output = value();
        } else if (is("-l", "--listing")) {
            listing = value();
        } else if (is("", "--cpu")) {
            std::string v(value());
            for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v == "c166" || v == "80c166")
                opt.cpu = oc166::isa::Cpu::C166;
            else if (v == "c167" || v == "80c167" || v == "st10")
                opt.cpu = oc166::isa::Cpu::C167;
            else
                throw UsageError("unknown CPU '" + v + "' (use c166 or c167)");
        } else if (is("", "--origin")) {
            opt.origin = parse_number(value(), "origin");
        } else if (is("", "--base")) {
            base = parse_number(value(), "base address");
        } else if (is("", "--fill")) {
            std::int64_t f = parse_number(value(), "fill byte");
            if (f > 0xFF) throw UsageError("fill byte must be 0..255");
            fill = static_cast<std::uint8_t>(f);
        } else if (a.size() > 1 && a[0] == '-') {
            throw UsageError("unknown option '" + std::string(a) + "'");
        } else if (input.empty()) {
            input = a;
        } else {
            throw UsageError("only one source file may be given");
        }
    }
    if (input.empty()) throw UsageError("no source file given");
    if (output.empty()) output = replace_extension(input, ".bin");

    opt.file_name = input;
    std::string source = read_file(input);
    oc166::AssemblyResult result = oc166::assemble(source, opt);

    for (const auto& d : result.diagnostics) std::cerr << oc166::format_diagnostic(d) << "\n";
    if (!listing.empty()) {
        std::string text = oc166::format_listing(result);
        write_file(listing, text.data(), text.size());
    }
    if (!result.ok()) {
        std::cerr << result.error_count() << " error(s), " << result.warning_count()
                  << " warning(s); no binary written\n";
        return 1;
    }
    oc166::FlatImage image = oc166::flat_image(result, base, fill);
    write_file(output, reinterpret_cast<const char*>(image.bytes.data()), image.bytes.size());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const UsageError& e) {
        std::cerr << "openc166: " << e.what() << "\n\n" << kUsage;
    } catch (const std::exception& e) {
        std::cerr << "openc166: " << e.what() << "\n";
    }
    return 2;
}
