// TinyKV — command dispatch: argv in, one-line protocol response out.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "kvstore.hpp"

namespace tinykv {

// Executes one parsed command against the store and returns the response
// line (without the trailing newline). Sets shutdown_flag on SHUTDOWN.
std::string execute(KVStore& store, const std::vector<std::string>& argv,
                    std::atomic<bool>& shutdown_flag);

}  // namespace tinykv
