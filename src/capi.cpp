// SPDX-License-Identifier: MIT
// The C interface (include/openc166/openc166.h) over the C++ assembler.
// Every function catches all exceptions: none may cross into C.
#include "openc166/openc166.h"

#include "assembler.hpp"
#include "predef.hpp"

#include <cctype>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef OPENC166_VERSION
#error "OPENC166_VERSION must be defined"
#endif

struct oc166_options {
    oc166::AssembleOptions opt;
};

struct oc166_result {
    oc166::AssemblyResult res;
    std::vector<std::string> diagnostic_text;  // format_diagnostic() of each diagnostic
    std::optional<std::string> listing;
    oc166::FlatImage image;
    std::string intel_hex;
    std::string srec;
    std::string last_error;
};

namespace {

const char* symbol_kind(oc166::SymbolKind k) {
    switch (k) {
    case oc166::SymbolKind::Label: return "label";
    case oc166::SymbolKind::Equ: return "equ";
    case oc166::SymbolKind::Set: return "set";
    case oc166::SymbolKind::Bit: return "bit";
    case oc166::SymbolKind::Lit: return "lit";
    case oc166::SymbolKind::Extern: return "extern";
    }
    return "";
}

const char* label_type(oc166::LType t) {
    switch (t) {
    case oc166::LType::None: return "";
    case oc166::LType::Near: return "near";
    case oc166::LType::Far: return "far";
    case oc166::LType::Byte: return "byte";
    case oc166::LType::Word: return "word";
    case oc166::LType::Dword: return "dword";
    }
    return "";
}

void fill_symbol(const oc166::Symbol& s, oc166_symbol* out) {
    const oc166::Value& v = s.value;
    out->name = s.name.c_str();
    out->kind = symbol_kind(s.kind);
    out->label_type = label_type(v.ltype);
    out->value = v.num;
    out->bit = 0;
    out->known = v.known ? 1 : 0;
    out->text = s.text.c_str();
    out->line = s.line;
    if (s.kind == oc166::SymbolKind::Lit) {
        out->type = "";
        out->label_type = "";
        out->value = 0;
        out->known = 1;
        return;
    }
    switch (v.type) {
    case oc166::VType::Number: out->type = "number"; break;
    case oc166::VType::Gpr: out->type = "gpr"; break;
    case oc166::VType::GprB: out->type = "gpr_byte"; break;
    case oc166::VType::Sfr: out->type = "sfr"; break;
    case oc166::VType::Cc: out->type = "cc"; break;
    case oc166::VType::Bit:
        // Symbols never name bits of GPRs, so the word address is known.
        out->type = "bit";
        out->value = oc166::bit_word_address(v);
        out->bit = v.bit;
        break;
    }
}

// A result of assemble(), or NULL if out of memory.
template <class F>
oc166_result* new_result(const std::string& file_name, F assemble) {
    try {
        auto r = std::make_unique<oc166_result>();
        try {
            r->res = assemble();
        } catch (const std::bad_alloc&) {
            return nullptr;
        } catch (const std::exception& e) {
            // A bug in openc166: report it instead of losing the whole result.
            r->res.diagnostics.push_back(
                {oc166::Severity::Error, "OC0", std::string("internal error: ") + e.what(), file_name, 0, 0});
        }
        r->diagnostic_text.reserve(r->res.diagnostics.size());
        for (const auto& d : r->res.diagnostics) r->diagnostic_text.push_back(oc166::format_diagnostic(d));
        return r.release();
    } catch (...) {
        return nullptr;
    }
}

// Runs 'f' and returns the text it builds into 'slot', or NULL with
// last_error set.
template <class F>
const char* text_output(oc166_result* r, std::string& slot, F f) {
    try {
        r->last_error.clear();
        slot = f();
        return slot.c_str();
    } catch (const std::exception& e) {
        try {
            r->last_error = e.what();
        } catch (...) {
        }
    } catch (...) {
    }
    return nullptr;
}

}  // namespace

extern "C" {

int oc166_api_version(void) { return OC166_API_VERSION; }

const char* oc166_version(void) { return OPENC166_VERSION; }

oc166_options* oc166_options_new(void) { return new (std::nothrow) oc166_options; }

void oc166_options_free(oc166_options* opts) { delete opts; }

int oc166_options_set_cpu(oc166_options* opts, const char* cpu) {
    if (!opts || !cpu) return -1;
    std::string v(cpu);
    for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "c166" || v == "80c166")
        opts->opt.cpu = oc166::isa::Cpu::C166;
    else if (v == "c167" || v == "80c167" || v == "st10")
        opts->opt.cpu = oc166::isa::Cpu::C167;
    else
        return -1;
    return 0;
}

int oc166_options_set_dialect(oc166_options* opts, const char* dialect) {
    if (!opts || !dialect) return -1;
    std::string v(dialect);
    for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "keil" || v == "a166")
        opts->opt.dialect = oc166::Dialect::Keil;
    else if (v == "tasking")
        opts->opt.dialect = oc166::Dialect::Tasking;
    else
        return -1;
    return 0;
}

int oc166_options_add_control(oc166_options* opts, const char* controls) {
    if (!opts || !controls) return -1;
    try {
        opts->opt.controls.emplace_back(controls);
        return 0;
    } catch (...) {
        return -1;
    }
}

int oc166_options_set_origin(oc166_options* opts, int64_t origin) {
    if (!opts || origin < 0) return -1;
    opts->opt.origin = origin;
    return 0;
}

int oc166_options_add_include_dir(oc166_options* opts, const char* dir) {
    if (!opts || !dir) return -1;
    try {
        opts->opt.include_dirs.emplace_back(dir);
        return 0;
    } catch (...) {
        return -1;
    }
}

int oc166_options_set_section_address(oc166_options* opts, const char* name, int64_t address) {
    if (!opts || !name || !*name || address < 0 || address > 0xFFFFFF) return -1;
    try {
        opts->opt.section_addresses.emplace_back(name, address);
        return 0;
    } catch (...) {
        return -1;
    }
}

oc166_result* oc166_assemble(const oc166_options* opts, const char* source, size_t length, const char* file_name) {
    if (!source && length != 0) return nullptr;
    try {
        oc166::AssembleOptions opt = opts ? opts->opt : oc166::AssembleOptions{};
        if (file_name) opt.file_name = file_name;
        return new_result(opt.file_name,
                          [&] { return oc166::assemble(std::string_view(source ? source : "", length), opt); });
    } catch (...) {
        return nullptr;
    }
}

oc166_result* oc166_assemble_many(const oc166_options* opts, size_t count, const char* const* sources,
                                  const size_t* lengths, const char* const* file_names) {
    if (count == 0 || !sources || !lengths) return nullptr;
    try {
        const oc166::AssembleOptions opt = opts ? opts->opt : oc166::AssembleOptions{};
        std::vector<oc166::Source> list(count);
        for (size_t i = 0; i < count; ++i) {
            if (!sources[i] && lengths[i] != 0) return nullptr;
            const char* name = file_names ? file_names[i] : nullptr;
            list[i].file_name = name ? name : "module" + std::to_string(i + 1) + ".a66";
            list[i].text.assign(sources[i] ? sources[i] : "", lengths[i]);
        }
        return new_result(list.front().file_name, [&] { return oc166::assemble(list, opt); });
    } catch (...) {
        return nullptr;
    }
}

void oc166_result_free(oc166_result* r) { delete r; }

size_t oc166_result_error_count(const oc166_result* r) { return r ? r->res.error_count() : 0; }

size_t oc166_result_warning_count(const oc166_result* r) { return r ? r->res.warning_count() : 0; }

const char* oc166_result_cpu(const oc166_result* r) {
    if (!r) return "";
    return r->res.cpu == oc166::isa::Cpu::C167 ? "c167" : "c166";
}

size_t oc166_result_diagnostic_count(const oc166_result* r) { return r ? r->res.diagnostics.size() : 0; }

int oc166_result_diagnostic(const oc166_result* r, size_t index, oc166_diagnostic* out) {
    if (!r || !out || index >= r->res.diagnostics.size()) return -1;
    const oc166::Diagnostic& d = r->res.diagnostics[index];
    out->is_error = d.severity == oc166::Severity::Error ? 1 : 0;
    out->code = d.code.c_str();
    out->message = d.message.c_str();
    out->file = d.file.c_str();
    out->line = d.line;
    out->column = d.column;
    out->text = r->diagnostic_text[index].c_str();
    return 0;
}

size_t oc166_result_chunk_count(const oc166_result* r) { return r ? r->res.chunks.size() : 0; }

int oc166_result_chunk(const oc166_result* r, size_t index, oc166_chunk* out) {
    if (!r || !out || index >= r->res.chunks.size()) return -1;
    const oc166::Chunk& c = r->res.chunks[index];
    out->address = c.address;
    out->data = c.bytes.data();
    out->size = c.bytes.size();
    return 0;
}

int oc166_result_image(oc166_result* r, int has_base, int64_t base, uint8_t fill, oc166_image* out) {
    if (!r || !out) return -1;
    try {
        r->last_error.clear();
        r->image = oc166::flat_image(r->res, has_base ? std::optional<std::int64_t>(base) : std::nullopt, fill);
        out->base = r->image.base;
        out->data = r->image.bytes.data();
        out->size = r->image.bytes.size();
        return 0;
    } catch (const std::exception& e) {
        try {
            r->last_error = e.what();
        } catch (...) {
        }
        return -1;
    } catch (...) {
        return -1;
    }
}

const char* oc166_result_intel_hex(oc166_result* r, int record_size) {
    if (!r) return nullptr;
    return text_output(r, r->intel_hex, [&] { return oc166::format_intel_hex(r->res, record_size); });
}

const char* oc166_result_srec(oc166_result* r, int record_size, int address_size) {
    if (!r) return nullptr;
    return text_output(r, r->srec, [&] { return oc166::format_srec(r->res, record_size, address_size); });
}

const char* oc166_result_last_error(const oc166_result* r) { return r ? r->last_error.c_str() : ""; }

const char* oc166_result_listing(oc166_result* r) {
    if (!r) return nullptr;
    try {
        if (!r->listing) r->listing = oc166::format_listing(r->res);
        return r->listing->c_str();
    } catch (...) {
        return nullptr;
    }
}

size_t oc166_result_module_count(const oc166_result* r) { return r ? r->res.modules.size() : 0; }

const char* oc166_result_module(const oc166_result* r, size_t index) {
    if (!r || index >= r->res.modules.size()) return nullptr;
    return r->res.modules[index].c_str();
}

size_t oc166_result_symbol_count(const oc166_result* r) { return r ? r->res.symbols.size() : 0; }

int oc166_result_symbol(const oc166_result* r, size_t index, oc166_symbol* out) {
    if (!r || !out || index >= r->res.symbols.size()) return -1;
    fill_symbol(r->res.symbols[index], out);
    return 0;
}

int oc166_result_symbol_module(const oc166_result* r, size_t index) {
    if (!r || index >= r->res.symbols.size()) return -1;
    return r->res.symbols[index].module;
}

int oc166_result_symbol_exported(const oc166_result* r, size_t index) {
    if (!r || index >= r->res.symbols.size()) return -1;
    return r->res.symbols[index].exported ? 1 : 0;
}

size_t oc166_result_section_count(const oc166_result* r) { return r ? r->res.sections.size() : 0; }

int oc166_result_section(const oc166_result* r, size_t index, oc166_section* out) {
    if (!r || !out || index >= r->res.sections.size()) return -1;
    const oc166::SectionInfo& s = r->res.sections[index];
    out->name = s.name.c_str();
    out->type = s.type.c_str();
    out->align = s.align.c_str();
    out->address = s.address;
    out->size = s.size;
    switch (s.placement) {
    case oc166::SectionInfo::Placement::At: out->placement = "at"; break;
    case oc166::SectionInfo::Placement::Given: out->placement = "given"; break;
    case oc166::SectionInfo::Placement::Located: out->placement = "located"; break;
    }
    return 0;
}

const char* oc166_result_section_class(const oc166_result* r, size_t index) {
    if (!r || index >= r->res.sections.size()) return nullptr;
    return r->res.sections[index].class_name.c_str();
}

const char* oc166_result_section_group(const oc166_result* r, size_t index) {
    if (!r || index >= r->res.sections.size()) return nullptr;
    return r->res.sections[index].group.c_str();
}

int oc166_result_section_bits(const oc166_result* r, size_t index, int* first_bit, int64_t* bit_count) {
    if (!r || index >= r->res.sections.size()) return -1;
    const oc166::SectionInfo& s = r->res.sections[index];
    if (first_bit) *first_bit = s.bit;
    if (bit_count) *bit_count = s.bit_size;
    return 0;
}

}  // extern "C"
