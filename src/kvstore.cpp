// TinyKV — KVStore implementation: expiry, append-only log and snapshots.
#include "kvstore.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <utility>

#include "commands.hpp"
#include "protocol.hpp"

namespace tinykv {
namespace fs = std::filesystem;

namespace {

bool parse_int64_strict(const std::string& s, long long& out) {
    if (s.empty()) {
        return false;
    }
    std::size_t pos = 0;
    try {
        out = std::stoll(s, &pos);
    } catch (const std::exception&) {
        return false;
    }
    return pos == s.size();
}

// Glob matcher supporting '*' wildcards (used by KEYS).
bool glob_match(const char* pat, const char* str) {
    const char* star = nullptr;
    const char* ss = str;
    while (*str != '\0') {
        if (*pat == '*') {
            star = pat++;
            ss = str;
        } else if (*pat == *str) {
            ++pat;
            ++str;
        } else if (star != nullptr) {
            pat = star + 1;
            str = ++ss;
        } else {
            return false;
        }
    }
    while (*pat == '*') {
        ++pat;
    }
    return *pat == '\0';
}

std::string read_text_line(std::FILE* f, bool& ok) {
    std::string line;
    int c;
    ok = false;
    while ((c = std::fgetc(f)) != EOF) {
        if (c == '\n') {
            ok = true;
            return line;
        }
        line.push_back(static_cast<char>(c));
    }
    return line;  // EOF: ok stays false
}

}  // namespace

KVStore::KVStore(const std::string& data_dir) : data_dir_(data_dir) {
    std::error_code ec;
    fs::create_directories(data_dir_, ec);
    load_snapshot();
    replay_aof();
    aof_ = std::fopen((data_dir_ + "/appendonly.aof").c_str(), "a");
}

KVStore::~KVStore() {
    if (aof_ != nullptr) {
        std::fclose(aof_);
        aof_ = nullptr;
    }
}

void KVStore::set_raw(const std::string& key, const std::string& value) {
    data_.insert(key, value);
    expiries_.erase(key);  // SET clears any previous TTL, like Redis
}

void KVStore::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    set_raw(key, value);
    aof_append("SET " + key + " " + value);
}

bool KVStore::get(const std::string& key, std::string& value_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (expired_locked(key)) {
        return false;
    }
    return data_.find(key, value_out);
}

long long KVStore::del(const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lock(mutex_);
    long long removed = 0;
    std::string line = "DEL";
    for (const std::string& key : keys) {
        line += " " + key;
        if (expired_locked(key)) {
            continue;
        }
        if (data_.erase(key)) {
            expiries_.erase(key);
            ++removed;
        }
    }
    aof_append(line);
    return removed;
}

long long KVStore::exists(const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lock(mutex_);
    long long count = 0;
    for (const std::string& key : keys) {
        if (expired_locked(key)) {
            continue;
        }
        std::string ignored;
        if (data_.find(key, ignored)) {
            ++count;
        }
    }
    return count;
}

long long KVStore::expire(const std::string& key, long long seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (expired_locked(key)) {
        return 0;
    }
    std::string ignored;
    if (!data_.find(key, ignored)) {
        return 0;
    }
    expiries_.insert(key, std::to_string(now_ms() + seconds * 1000));
    aof_append("EXPIRE " + key + " " + std::to_string(seconds));
    return 1;
}

long long KVStore::ttl(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (expired_locked(key)) {
        return -2;
    }
    std::string ignored;
    if (!data_.find(key, ignored)) {
        return -2;
    }
    std::string exp;
    if (!expiries_.find(key, exp)) {
        return -1;
    }
    long long exp_ms = 0;
    if (!parse_int64_strict(exp, exp_ms)) {
        return -1;
    }
    long long remaining_ms = exp_ms - now_ms();
    if (remaining_ms < 0) {
        return -2;
    }
    return (remaining_ms + 999) / 1000;  // round up to whole seconds
}

std::vector<std::string> KVStore::keys(const std::string& pattern) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> all;
    data_.for_each([&](const std::string& k, const std::string&) { all.push_back(k); });
    std::vector<std::string> out;
    for (const std::string& k : all) {
        if (expired_locked(k)) {
            continue;
        }
        if (glob_match(pattern.c_str(), k.c_str())) {
            out.push_back(k);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool KVStore::incr(const std::string& key, long long& result_out,
                   std::string& err_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    long long current = 0;
    std::string raw;
    if (!expired_locked(key) && data_.find(key, raw)) {
        if (!parse_int64_strict(raw, current)) {
            err_out = "value is not an integer";
            return false;
        }
    }
    current += 1;
    set_raw(key, std::to_string(current));
    aof_append("SET " + key + " " + std::to_string(current));
    result_out = current;
    return true;
}

long long KVStore::dbsize() {
    std::lock_guard<std::mutex> lock(mutex_);
    sweep_locked();
    return static_cast<long long>(data_.size());
}

void KVStore::sweep_expired() {
    std::lock_guard<std::mutex> lock(mutex_);
    sweep_locked();
}

void KVStore::sweep_locked() {
    std::vector<std::string> all;
    data_.for_each([&](const std::string& k, const std::string&) { all.push_back(k); });
    for (const std::string& k : all) {
        expired_locked(k);
    }
}

bool KVStore::expired_locked(const std::string& key) {
    std::string exp;
    if (!expiries_.find(key, exp)) {
        return false;
    }
    long long exp_ms = 0;
    if (!parse_int64_strict(exp, exp_ms)) {
        expiries_.erase(key);
        return false;
    }
    if (now_ms() >= exp_ms) {
        data_.erase(key);
        expiries_.erase(key);
        return true;
    }
    return false;
}

void KVStore::aof_append(const std::string& line) {
    if (replaying_ || aof_ == nullptr) {
        return;
    }
    std::fputs((line + "\n").c_str(), aof_);
    std::fflush(aof_);
}

// ---- persistence -----------------------------------------------------------

bool KVStore::save() {
    std::lock_guard<std::mutex> lock(mutex_);
    sweep_locked();

    const std::string tmp_path = data_dir_ + "/snapshot.tkv.tmp";
    const std::string snap_path = data_dir_ + "/snapshot.tkv";
    std::FILE* f = std::fopen(tmp_path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    std::fprintf(f, "TKVSNAP1\n%zu\n", data_.size());
    bool ok = true;
    data_.for_each([&](const std::string& k, const std::string& v) {
        if (!ok) {
            return;
        }
        std::string exp;
        long long exp_ms = -1;
        if (expiries_.find(k, exp)) {
            parse_int64_strict(exp, exp_ms);
        }
        std::fprintf(f, "%zu\n", k.size());
        if (std::fwrite(k.data(), 1, k.size(), f) != k.size()) ok = false;
        std::fprintf(f, "\n%zu\n", v.size());
        if (std::fwrite(v.data(), 1, v.size(), f) != v.size()) ok = false;
        std::fprintf(f, "\n%lld\n", exp_ms);
    });
    std::fclose(f);
    if (!ok) {
        return false;
    }
    std::error_code ec;
    fs::rename(tmp_path, snap_path, ec);
    if (ec) {
        return false;
    }
    // The snapshot now covers everything: truncate the AOF.
    if (aof_ != nullptr) {
        std::fclose(aof_);
    }
    aof_ = std::fopen((data_dir_ + "/appendonly.aof").c_str(), "w");
    return aof_ != nullptr;
}

bool KVStore::load_snapshot() {
    const std::string snap_path = data_dir_ + "/snapshot.tkv";
    std::FILE* f = std::fopen(snap_path.c_str(), "rb");
    if (f == nullptr) {
        return true;  // no snapshot yet: not an error
    }
    bool ok = false;
    std::string magic = read_text_line(f, ok);
    if (!ok || magic != "TKVSNAP1") {
        std::fclose(f);
        return false;
    }
    std::string count_line = read_text_line(f, ok);
    std::size_t count = 0;
    if (!ok) {
        std::fclose(f);
        return false;
    }
    try {
        count = static_cast<std::size_t>(std::stoull(count_line));
    } catch (const std::exception&) {
        std::fclose(f);
        return false;
    }
    const long long now = now_ms();
    for (std::size_t i = 0; i < count; ++i) {
        std::string klen_line = read_text_line(f, ok);
        if (!ok) break;
        std::size_t klen = 0;
        try {
            klen = static_cast<std::size_t>(std::stoull(klen_line));
        } catch (const std::exception&) { break; }
        std::string key(klen, '\0');
        if (klen > 0 && std::fread(key.data(), 1, klen, f) != klen) break;
        if (std::fgetc(f) != '\n') break;  // consume separator newline
        std::string vlen_line = read_text_line(f, ok);
        if (!ok) break;
        std::size_t vlen = 0;
        try {
            vlen = static_cast<std::size_t>(std::stoull(vlen_line));
        } catch (const std::exception&) { break; }
        std::string value(vlen, '\0');
        if (vlen > 0 && std::fread(value.data(), 1, vlen, f) != vlen) break;
        if (std::fgetc(f) != '\n') break;
        std::string exp_line = read_text_line(f, ok);
        if (!ok) break;
        long long exp_ms = -1;
        parse_int64_strict(exp_line, exp_ms);
        if (exp_ms != -1 && exp_ms <= now) {
            continue;  // already expired while we were away
        }
        data_.insert(key, value);
        if (exp_ms != -1) {
            expiries_.insert(key, std::to_string(exp_ms));
        }
    }
    std::fclose(f);
    return true;
}

void KVStore::replay_aof() {
    const std::string aof_path = data_dir_ + "/appendonly.aof";
    std::ifstream in(aof_path);
    if (!in.is_open()) {
        return;
    }
    std::atomic<bool> dummy_flag{false};
    std::string line;
    replaying_ = true;
    while (std::getline(in, line)) {
        std::vector<std::string> argv = parse_line(line);
        if (argv.empty()) {
            continue;
        }
        execute(*this, argv, dummy_flag);  // re-applies SET/DEL/EXPIRE
    }
    replaying_ = false;
}

}  // namespace tinykv
