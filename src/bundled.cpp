// SPDX-License-Identifier: MIT
#include "bundled.hpp"

#include <cctype>
#include <cstddef>

namespace oc166 {
namespace {

struct EmbeddedFile {
    const char* name;
    const unsigned char* data;
    std::size_t size;
};

#include "openc166/bundled_files.inc"

bool same_name(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

}  // namespace

std::optional<BundledFile> bundled_file(std::string_view name) {
    for (const auto& f : kEmbedded)
        if (same_name(f.name, name))
            return BundledFile{f.name, std::string_view(reinterpret_cast<const char*>(f.data), f.size)};
    return std::nullopt;
}

}  // namespace oc166
