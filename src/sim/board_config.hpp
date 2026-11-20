// SPDX-License-Identifier: MIT
// Board definitions in JSON: which chip, which devices on which chip
// selects, the configuration read at reset, and the default inputs.
// docs/boards.md describes the format.
#pragma once

#include "sim/machine.hpp"

#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace oc166::sim {

class BoardError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct BoardConfig {
    Board board;
    std::string description;
    // Inputs and options the board sets on its machine (apply_board()).
    std::map<int, std::uint16_t> adc;
    std::optional<std::uint16_t> ssc_reply;
    std::optional<bool> watchdog_frozen;
    std::optional<bool> serial_echo;
};

struct BoardSource {
    // What "$IMAGE" in the board file stands for: the image the user gave.
    std::vector<std::uint8_t> image;
    // Directory that relative file names in the board file start from.
    std::string base_dir;
    // Reads a file; the default reads from the file system.
    std::function<std::optional<std::vector<std::uint8_t>>(const std::string& path)> read_file;
};

// Builds a board from JSON text. Throws BoardError naming the line and
// column of the problem.
BoardConfig load_board(std::string_view json_text, const BoardSource& source, const std::string& origin = "board");

// A board by name (a board file built into openc166: "5wy19", "c167",
// "c167cs", ...), by the path of a .json file, or as JSON text. 'image' is
// what "$IMAGE" stands for.
BoardConfig load_board(const std::string& name_or_path, std::vector<std::uint8_t> image);

// Sets the board's inputs and options on a machine built from it.
void apply_board(Machine& m, const BoardConfig& config);

// The names of the built-in board files.
std::vector<std::string> builtin_boards();

// The built-in chips: "c166", "c167", "c167cr", "c167cs".
std::vector<std::string> builtin_chips();

}  // namespace oc166::sim
