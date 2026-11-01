// SPDX-License-Identifier: MIT
// Include files built into openc166 (the files in inc/ of the source
// tree), so that e.g. $INCLUDE(REG167.INC) works without extra files.
#pragma once

#include <optional>
#include <string_view>

namespace oc166 {

struct BundledFile {
    std::string_view name;  // as spelled in inc/, e.g. "REG167.INC"
    std::string_view text;
};

// The built-in file called 'name', ignoring case.
std::optional<BundledFile> bundled_file(std::string_view name);

}  // namespace oc166
