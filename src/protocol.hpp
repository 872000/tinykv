// TinyKV — line-protocol helpers: parsing client lines into argv vectors.
#pragma once

#include <string>
#include <vector>

namespace tinykv {

// Split a protocol line on whitespace (space/tab/CR/LF), dropping empties.
std::vector<std::string> parse_line(const std::string& line);

}  // namespace tinykv
