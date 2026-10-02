// TinyKV — the key/value store: expiry, persistence hooks and thread safety
// layered on top of the hand-rolled HashMap.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "hashmap.hpp"

namespace tinykv {

class KVStore {
public:
    using Clock = std::chrono::steady_clock;

    explicit KVStore(const std::string& data_dir);
    ~KVStore();

    KVStore(const KVStore&) = delete;
    KVStore& operator=(const KVStore&) = delete;

    void set(const std::string& key, const std::string& value);
    bool get(const std::string& key, std::string& value_out);
    long long del(const std::vector<std::string>& keys);
    long long exists(const std::vector<std::string>& keys);
    // Returns 1 when the timeout was set, 0 when the key does not exist.
    long long expire(const std::string& key, long long seconds);
    // -2: key missing, -1: no timeout, otherwise seconds remaining.
    long long ttl(const std::string& key);
    std::vector<std::string> keys(const std::string& pattern);
    // Returns false (and fills err) when the value is not an integer.
    bool incr(const std::string& key, long long& result_out, std::string& err_out);
    long long dbsize();

    // Write a snapshot now (also truncates the append-only log).
    bool save();
    // Drop keys whose TTL has passed.
    void sweep_expired();

private:
    void set_raw(const std::string& key, const std::string& value);
    // True when the key was expired (and has now been purged). Lock held.
    bool expired_locked(const std::string& key);
    void sweep_locked();
    void aof_append(const std::string& line);
    bool load_snapshot();
    void replay_aof();

    static long long now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   Clock::now().time_since_epoch())
            .count();
    }

    HashMap data_;      // key -> value
    HashMap expiries_;  // key -> absolute expiry, ms since steady-clock epoch
    std::string data_dir_;
    std::FILE* aof_ = nullptr;
    bool replaying_ = false;  // suppress AOF appends while replaying the log
    mutable std::mutex mutex_;
};

}  // namespace tinykv
