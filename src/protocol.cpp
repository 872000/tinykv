// TinyKV — line-protocol parsing.
#include "protocol.hpp"

namespace tinykv {

std::vector<std::string> parse_line(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

}  // namespace tinykv
