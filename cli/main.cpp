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

constexpr const char* kUsage = R"usage(usage: openc166 [options] SOURCE...

Assembles Keil A166 (or TASKING a166) assembly for the Infineon C166/C167
into a raw binary, Intel HEX or Motorola S-record file. Several sources are
linked into one program, as L166 would: EXTERN symbols take the values that
other sources make PUBLIC or GLOBAL, and sections of the same name and class
are combined.

options:
  -o, --output FILE   output file (default: the first SOURCE with the extension of the format)
  -f, --format FMT    bin (raw binary), hex (Intel HEX), srec (S-records) or elf (ELF
                      with DWARF debug information); the default follows the output
                      extension: .hex .h86 .ihx give hex, .srec .s19 .s28 .s37 .mot .sx
                      give srec, .elf .axf give elf, anything else bin
  -l, --listing FILE  write a listing
  -g, --debug-info FILE
                      write debug information (JSON) for the simulator and debugger
      --cpu CPU       c166 (default) or c167; $MOD167 in the source also selects c167
      --dialect D     keil (default): Keil A166 sources; tasking: TASKING a166
                      sources, run through openc166's m166 preprocessor (C167 only)
  -c, --control TEXT  controls read before the sources, as on the command line of
                      the assembler, e.g. -c CASE -c "STDNAMES(reg.def)" (may be repeated)
      --origin ADDR   address of code outside sections, and the lowest address for
                      sections without AT (default 0)
      --section NAME=ADDR
                      put section NAME, which has no AT, at ADDR (may be repeated);
                      the other sections without AT go to the first free place
      --base ADDR     bin: address of the first byte of the file (default: lowest address used)
      --fill BYTE     bin: value of unused bytes inside the file (default 0FFh)
      --record-size N hex, srec: data bytes per record (default 16)
  -I, --incdir DIR    also search DIR for $INCLUDE files (may be repeated)
  -h, --help          show this help
      --version       show the version

Numbers may be written as 0C000h, 0xC000 or decimal.

exit status: 0 success, 1 assembly errors, 2 usage or file errors
)usage";

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

enum class Format { Bin, Hex, Srec, Elf };

std::string lower(std::string_view s) {
    std::string v(s);
    for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v;
}

std::string extension(const std::string& path) {
    std::size_t slash = path.find_last_of("/\\");
    std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return lower(path.substr(dot));
}

std::string replace_extension(const std::string& path, const std::string& ext) {
    std::size_t slash = path.find_last_of("/\\");
    std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path + ext;
    return path.substr(0, dot) + ext;
}

int run(int argc, char** argv) {
    oc166::AssembleOptions opt;
    std::vector<std::string> inputs;
    std::string output, listing, debug_info;
    std::optional<std::int64_t> base;
    std::uint8_t fill = 0xFF;
    std::optional<Format> format;
    int record_size = 16;

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
        } else if (is("-g", "--debug-info")) {
            debug_info = value();
        } else if (is("-f", "--format")) {
            std::string v = lower(value());
            if (v == "bin" || v == "binary")
                format = Format::Bin;
            else if (v == "hex" || v == "ihex")
                format = Format::Hex;
            else if (v == "srec" || v == "s19" || v == "mot")
                format = Format::Srec;
            else if (v == "elf")
                format = Format::Elf;
            else
                throw UsageError("unknown format '" + v + "' (use bin, hex, srec or elf)");
        } else if (is("", "--record-size")) {
            std::int64_t n = parse_number(value(), "record size");
            if (n < 1 || n > 255) throw UsageError("record size must be 1..255");
            record_size = static_cast<int>(n);
        } else if (is("", "--cpu")) {
            std::string v = lower(value());
            if (v == "c166" || v == "80c166")
                opt.cpu = oc166::isa::Cpu::C166;
            else if (v == "c167" || v == "80c167" || v == "st10")
                opt.cpu = oc166::isa::Cpu::C167;
            else
                throw UsageError("unknown CPU '" + v + "' (use c166 or c167)");
        } else if (is("", "--dialect")) {
            std::string v = lower(value());
            if (v == "keil" || v == "a166")
                opt.dialect = oc166::Dialect::Keil;
            else if (v == "tasking")
                opt.dialect = oc166::Dialect::Tasking;
            else
                throw UsageError("unknown dialect '" + v + "' (use keil or tasking)");
        } else if (is("-c", "--control")) {
            opt.controls.emplace_back(value());
        } else if (is("", "--origin")) {
            opt.origin = parse_number(value(), "origin");
        } else if (is("", "--section")) {
            std::string_view v = value();
            std::size_t eq = v.find('=');
            if (eq == std::string_view::npos || eq == 0)
                throw UsageError("--section needs NAME=ADDR, not '" + std::string(v) + "'");
            std::int64_t address = parse_number(v.substr(eq + 1), "section address");
            if (address > 0xFFFFFF) throw UsageError("section address must be below 1000000h");
            opt.section_addresses.emplace_back(std::string(v.substr(0, eq)), address);
        } else if (is("", "--base")) {
            base = parse_number(value(), "base address");
        } else if (is("-I", "--incdir")) {
            opt.include_dirs.emplace_back(value());
        } else if (a.size() > 2 && a.starts_with("-I")) {
            opt.include_dirs.emplace_back(a.substr(2));
        } else if (is("", "--fill")) {
            std::int64_t f = parse_number(value(), "fill byte");
            if (f > 0xFF) throw UsageError("fill byte must be 0..255");
            fill = static_cast<std::uint8_t>(f);
        } else if (a.size() > 1 && a[0] == '-') {
            throw UsageError("unknown option '" + std::string(a) + "'");
        } else {
            inputs.emplace_back(a);
        }
    }
    if (inputs.empty()) throw UsageError("no source file given");
    // S1, S2 or S3 records for the conventional extensions; 0 lets the
    // addresses decide.
    int srec_address_size = 0;
    if (!format) {
        std::string ext = extension(output);
        if (ext == ".hex" || ext == ".h86" || ext == ".ihx" || ext == ".ihex")
            format = Format::Hex;
        else if (ext == ".srec" || ext == ".s19" || ext == ".s28" || ext == ".s37" || ext == ".mot" || ext == ".sx")
            format = Format::Srec;
        else if (ext == ".elf" || ext == ".axf")
            format = Format::Elf;
        else
            format = Format::Bin;
    }
    if (*format == Format::Srec) {
        std::string ext = extension(output);
        srec_address_size = ext == ".s19" ? 2 : ext == ".s28" ? 3 : ext == ".s37" ? 4 : 0;
    }
    if (output.empty())
        output = replace_extension(inputs.front(), *format == Format::Hex    ? ".hex"
                                                   : *format == Format::Srec ? ".srec"
                                                   : *format == Format::Elf  ? ".elf"
                                                                             : ".bin");

    std::vector<oc166::Source> sources;
    for (const auto& input : inputs) sources.push_back({input, read_file(input)});
    opt.file_name = inputs.front();
    oc166::AssemblyResult result = oc166::assemble(sources, opt);

    for (const auto& d : result.diagnostics) std::cerr << oc166::format_diagnostic(d) << "\n";
    if (!listing.empty()) {
        std::string text = oc166::format_listing(result);
        write_file(listing, text.data(), text.size());
    }
    if (!debug_info.empty() && result.ok()) {
        std::string text = oc166::format_debug_info(result);
        write_file(debug_info, text.data(), text.size());
    }
    if (!result.ok()) {
        std::cerr << result.error_count() << " error(s), " << result.warning_count()
                  << " warning(s); no output written\n";
        return 1;
    }
    if (*format == Format::Elf) {
        std::vector<std::uint8_t> elf = oc166::format_elf(result);
        write_file(output, reinterpret_cast<const char*>(elf.data()), elf.size());
    } else if (*format == Format::Bin) {
        oc166::FlatImage image = oc166::flat_image(result, base, fill);
        write_file(output, reinterpret_cast<const char*>(image.bytes.data()), image.bytes.size());
    } else {
        std::string text = *format == Format::Hex
                               ? oc166::format_intel_hex(result, record_size)
                               : oc166::format_srec(result, record_size, srec_address_size);
        write_file(output, text.data(), text.size());
    }
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
