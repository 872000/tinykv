// TinyKV — self-contained test runner (no external test framework).
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "commands.hpp"
#include "hashmap.hpp"
#include "kvstore.hpp"
#include "protocol.hpp"

namespace fs = std::filesystem;
using tinykv::HashMap;
using tinykv::KVStore;

static int g_pass = 0;
static int g_fail = 0;
static const char* g_current = "";

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++g_pass;                                                            \
        } else {                                                                 \
            ++g_fail;                                                            \
            std::cout << "  FAIL [" << g_current << "] line " << __LINE__ << ": " \
                      << #cond << "\n";                                          \
        }                                                                        \
    } while (0)

#define RUN(name)                                        \
    do {                                                 \
        g_current = #name;                                \
        std::cout << "  " << #name << "\n";               \
        name();                                          \
    } while (0)

namespace {

std::string fresh_dir() {
    static int n = 0;
    auto p = fs::temp_directory_path() /
             ("tinykv_test_" + std::to_string(::getpid()) + "_" +
              std::to_string(n++));
    fs::create_directories(p);
    return p.string();
}

std::string exec_cmd(KVStore& store, const std::string& line) {
    std::atomic<bool> flag{false};
    return tinykv::execute(store, tinykv::parse_line(line), flag);
}

// ---- hash table ------------------------------------------------------------

void hashmap_insert_find() {
    HashMap m;
    CHECK(m.insert("a", "1"));
    CHECK(m.insert("b", "2"));
    std::string v;
    CHECK(m.find("a", v) && v == "1");
    CHECK(m.find("b", v) && v == "2");
    CHECK(!m.find("zzz", v));
    CHECK(m.size() == 2);
}

void hashmap_overwrite() {
    HashMap m;
    CHECK(m.insert("k", "old"));
    CHECK(!m.insert("k", "new"));  // overwrite returns false
    std::string v;
    CHECK(m.find("k", v) && v == "new");
    CHECK(m.size() == 1);
}

void hashmap_erase() {
    HashMap m;
    m.insert("a", "1");
    CHECK(m.erase("a"));
    CHECK(!m.erase("a"));  // already gone
    std::string v;
    CHECK(!m.find("a", v));
    CHECK(m.size() == 0);
}

void hashmap_resize_stress() {
    HashMap m;
    const int N = 5000;
    for (int i = 0; i < N; ++i) {
        m.insert("key" + std::to_string(i), "val" + std::to_string(i));
    }
    CHECK(m.size() == static_cast<std::size_t>(N));
    CHECK(m.bucket_count() > 16);  // must have grown past the initial table
    bool all_ok = true;
    std::string v;
    for (int i = 0; i < N; ++i) {
        if (!m.find("key" + std::to_string(i), v) ||
            v != "val" + std::to_string(i)) {
            all_ok = false;
            break;
        }
    }
    CHECK(all_ok);  // every key survives rehashing
    // Deleting half still leaves the rest reachable.
    for (int i = 0; i < N; i += 2) {
        m.erase("key" + std::to_string(i));
    }
    CHECK(m.size() == static_cast<std::size_t>(N / 2));
    bool rest_ok = true;
    for (int i = 1; i < N; i += 2) {
        if (!m.find("key" + std::to_string(i), v)) {
            rest_ok = false;
            break;
        }
    }
    CHECK(rest_ok);
}

void hashmap_for_each() {
    HashMap m;
    m.insert("x", "1");
    m.insert("y", "2");
    m.insert("z", "3");
    int count = 0;
    m.for_each([&](const std::string&, const std::string&) { ++count; });
    CHECK(count == 3);
}

// ---- store basics ----------------------------------------------------------

void store_set_get() {
    KVStore s(fresh_dir());
    s.set("name", "tinykv");
    std::string v;
    CHECK(s.get("name", v) && v == "tinykv");
    CHECK(!s.get("missing", v));
    s.set("name", "v2");
    CHECK(s.get("name", v) && v == "v2");
}

void store_del_exists() {
    KVStore s(fresh_dir());
    s.set("a", "1");
    s.set("b", "2");
    CHECK(s.exists({"a", "b", "nope"}) == 2);
    CHECK(s.del({"a", "nope"}) == 1);
    CHECK(s.exists({"a"}) == 0);
    CHECK(s.exists({"b"}) == 1);
}

void store_expire_ttl() {
    KVStore s(fresh_dir());
    s.set("sess", "abc");
    CHECK(s.ttl("sess") == -1);       // persistent
    CHECK(s.ttl("missing") == -2);    // missing
    CHECK(s.expire("missing", 10) == 0);
    CHECK(s.expire("sess", 100) == 1);
    long long t = s.ttl("sess");
    CHECK(t > 0 && t <= 100);
}

void store_expiry_actually_expires() {
    KVStore s(fresh_dir());
    s.set("temp", "x");
    CHECK(s.expire("temp", 1) == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    std::string v;
    CHECK(!s.get("temp", v));   // lazily purged on access
    CHECK(s.ttl("temp") == -2);
    CHECK(s.exists({"temp"}) == 0);
}

void store_incr() {
    KVStore s(fresh_dir());
    long long r = 0;
    std::string err;
    CHECK(s.incr("counter", r, err) && r == 1);  // missing -> starts at 0
    CHECK(s.incr("counter", r, err) && r == 2);
    s.set("notint", "abc");
    CHECK(!s.incr("notint", r, err));
    CHECK(err == "value is not an integer");
}

void store_keys_pattern() {
    KVStore s(fresh_dir());
    s.set("user:1", "a");
    s.set("user:2", "b");
    s.set("order:1", "c");
    auto users = s.keys("user:*");
    CHECK(users.size() == 2);
    auto all = s.keys("*");
    CHECK(all.size() == 3);
    auto none = s.keys("zzz*");
    CHECK(none.empty());
}

void store_keys_exact_and_mid_wildcard() {
    KVStore s(fresh_dir());
    s.set("user:1", "a");
    s.set("user:2", "b");
    s.set("order:1", "c");
    // A pattern with no wildcard matches exactly one key.
    auto exact = s.keys("user:1");
    CHECK(exact.size() == 1 && exact[0] == "user:1");
    // A '*' in the middle of the pattern still anchors both ends.
    auto mid = s.keys("u*r:1");
    CHECK(mid.size() == 1 && mid[0] == "user:1");
}

void store_sweep() {
    KVStore s(fresh_dir());
    s.set("a", "1");
    s.set("gone", "2");
    s.expire("gone", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    s.sweep_expired();
    CHECK(s.dbsize() == 1);
}

// ---- persistence -----------------------------------------------------------

void persistence_snapshot_roundtrip() {
    std::string dir = fresh_dir();
    {
        KVStore s(dir);
        s.set("k1", "v1");
        s.set("k2", "value with spaces");
        s.set("ttlkey", "t");
        s.expire("ttlkey", 3600);
        CHECK(s.save());
    }
    {
        KVStore s2(dir);  // fresh store: loads snapshot on startup
        std::string v;
        CHECK(s2.get("k1", v) && v == "v1");
        CHECK(s2.get("k2", v) && v == "value with spaces");
        CHECK(s2.get("ttlkey", v) && v == "t");
        CHECK(s2.ttl("ttlkey") > 0);  // TTL survived the round-trip
        CHECK(s2.dbsize() == 3);
    }
    fs::remove_all(dir);
}

void persistence_aof_replay() {
    std::string dir = fresh_dir();
    {
        KVStore s(dir);
        s.set("a", "1");
        s.set("b", "2");
        s.del({"b"});  // no snapshot: only the AOF records this
    }
    {
        KVStore s2(dir);  // replays the append-only log
        std::string v;
        CHECK(s2.get("a", v) && v == "1");
        CHECK(!s2.get("b", v));
    }
    fs::remove_all(dir);
}

// ---- protocol --------------------------------------------------------------

void protocol_parse() {
    auto t = tinykv::parse_line("SET mykey hello world");
    CHECK(t.size() == 4 && t[0] == "SET" && t[1] == "mykey" && t[3] == "world");
    CHECK(tinykv::parse_line("").empty());
    CHECK(tinykv::parse_line("   ").empty());
    auto t2 = tinykv::parse_line("GET\tkey\r");
    CHECK(t2.size() == 2 && t2[0] == "GET" && t2[1] == "key");
}

// ---- end-to-end command dispatch -------------------------------------------

void execute_end_to_end() {
    KVStore s(fresh_dir());
    CHECK(exec_cmd(s, "PING") == "+PONG");
    CHECK(exec_cmd(s, "SET greeting hello world") == "+OK");
    CHECK(exec_cmd(s, "GET greeting") == "$hello world");
    CHECK(exec_cmd(s, "GET nope") == "(nil)");
    CHECK(exec_cmd(s, "EXISTS greeting nope") == ":1");
    CHECK(exec_cmd(s, "INCR n") == ":1");
    CHECK(exec_cmd(s, "INCR n") == ":2");
    CHECK(exec_cmd(s, "TTL n") == ":-1");
    CHECK(exec_cmd(s, "EXPIRE n 50") == ":1");
    CHECK(exec_cmd(s, "DBSIZE") == ":2");
    CHECK(exec_cmd(s, "KEYS g*") == "$greeting");
    CHECK(exec_cmd(s, "DEL greeting") == ":1");
    CHECK(exec_cmd(s, "SAVE") == "+OK");
    CHECK(exec_cmd(s, "BOGUS") == "-ERR unknown command 'BOGUS'");
    CHECK(exec_cmd(s, "GET") == "-ERR wrong number of arguments for 'get'");
    CHECK(exec_cmd(s, "SET onlykey") == "-ERR wrong number of arguments for 'set'");
    CHECK(exec_cmd(s, "SET greeting hello world") == "+OK");
    CHECK(exec_cmd(s, "INCR greeting") ==
          "-ERR value is not an integer");  // "hello world"
}

}  // namespace

int main() {
    std::cout << "tinykv tests\n";
    RUN(hashmap_insert_find);
    RUN(hashmap_overwrite);
    RUN(hashmap_erase);
    RUN(hashmap_resize_stress);
    RUN(hashmap_for_each);
    RUN(store_set_get);
    RUN(store_del_exists);
    RUN(store_expire_ttl);
    RUN(store_expiry_actually_expires);
    RUN(store_incr);
    RUN(store_keys_pattern);
    RUN(store_keys_exact_and_mid_wildcard);
    RUN(store_sweep);
    RUN(persistence_snapshot_roundtrip);
    RUN(persistence_aof_replay);
    RUN(protocol_parse);
    RUN(execute_end_to_end);

    std::cout << "\n" << g_pass << " assertions passed, " << g_fail
              << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
