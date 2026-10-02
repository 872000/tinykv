// TinyKV — command dispatch: argv in, one-line protocol response out.
//
// Response wire format (one line each):
//   +OK            success
//   -ERR <message> error
//   :<integer>     integer reply
//   $<string>      string reply (may be empty)
//   (nil)          missing value
#include "commands.hpp"

#include <algorithm>
#include <cctype>

#include "protocol.hpp"

namespace tinykv {

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string wrong_arity(const std::string& cmd) {
    return "-ERR wrong number of arguments for '" + cmd + "'";
}

std::string join_from(const std::vector<std::string>& argv, std::size_t from) {
    std::string out;
    for (std::size_t i = from; i < argv.size(); ++i) {
        if (i > from) {
            out.push_back(' ');
        }
        out += argv[i];
    }
    return out;
}

bool parse_i64_arg(const std::string& s, long long& out, std::string& err) {
    if (s.empty()) {
        err = "-ERR value is not an integer or out of range";
        return false;
    }
    std::size_t pos = 0;
    try {
        out = std::stoll(s, &pos);
    } catch (const std::exception&) {
        err = "-ERR value is not an integer or out of range";
        return false;
    }
    if (pos != s.size()) {
        err = "-ERR value is not an integer or out of range";
        return false;
    }
    return true;
}

}  // namespace

std::string execute(KVStore& store, const std::vector<std::string>& argv,
                    std::atomic<bool>& shutdown_flag) {
    if (argv.empty()) {
        return "-ERR empty command";
    }
    const std::string cmd = upper(argv[0]);

    if (cmd == "PING") {
        return "+PONG";
    }
    if (cmd == "SET") {
        if (argv.size() < 3) {
            return wrong_arity("set");
        }
        store.set(argv[1], join_from(argv, 2));
        return "+OK";
    }
    if (cmd == "GET") {
        if (argv.size() != 2) {
            return wrong_arity("get");
        }
        std::string value;
        if (!store.get(argv[1], value)) {
            return "(nil)";
        }
        return "$" + value;
    }
    if (cmd == "DEL") {
        if (argv.size() < 2) {
            return wrong_arity("del");
        }
        std::vector<std::string> keys(argv.begin() + 1, argv.end());
        return ":" + std::to_string(store.del(keys));
    }
    if (cmd == "EXISTS") {
        if (argv.size() < 2) {
            return wrong_arity("exists");
        }
        std::vector<std::string> keys(argv.begin() + 1, argv.end());
        return ":" + std::to_string(store.exists(keys));
    }
    if (cmd == "EXPIRE") {
        if (argv.size() != 3) {
            return wrong_arity("expire");
        }
        long long seconds = 0;
        std::string err;
        if (!parse_i64_arg(argv[2], seconds, err) || seconds < 0) {
            return "-ERR value is not an integer or out of range";
        }
        return ":" + std::to_string(store.expire(argv[1], seconds));
    }
    if (cmd == "TTL") {
        if (argv.size() != 2) {
            return wrong_arity("ttl");
        }
        return ":" + std::to_string(store.ttl(argv[1]));
    }
    if (cmd == "KEYS") {
        if (argv.size() != 2) {
            return wrong_arity("keys");
        }
        std::vector<std::string> ks = store.keys(argv[1]);
        if (ks.empty()) {
            return "(nil)";
        }
        return "$" + join_from(ks, 0);
    }
    if (cmd == "INCR") {
        if (argv.size() != 2) {
            return wrong_arity("incr");
        }
        long long result = 0;
        std::string err;
        if (!store.incr(argv[1], result, err)) {
            return "-ERR " + err;
        }
        return ":" + std::to_string(result);
    }
    if (cmd == "DBSIZE") {
        return ":" + std::to_string(store.dbsize());
    }
    if (cmd == "SAVE") {
        return store.save() ? "+OK" : "-ERR snapshot failed";
    }
    if (cmd == "SHUTDOWN") {
        shutdown_flag.store(true);
        return "+OK";
    }
    return "-ERR unknown command '" + argv[0] + "'";
}

}  // namespace tinykv
