// TinyKV — small CLI client: REPL plus one-shot --exec mode.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <iostream>
#include <string>

namespace {

bool send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// Read one '\n'-terminated line from the socket.
bool read_line(int fd, std::string& out) {
    out.clear();
    char c;
    while (true) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) {
            return false;
        }
        if (c == '\n') {
            return true;
        }
        if (c != '\r') {
            out.push_back(c);
        }
    }
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog
              << " [--host HOST] [--port PORT] [--exec \"COMMAND\"]\n"
                 "Without --exec, starts an interactive REPL.\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string exec_cmd;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--host" || arg == "-h") && i + 1 < argc) {
            host = argv[++i];
        } else if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if ((arg == "--exec" || arg == "-e") && i + 1 < argc) {
            exec_cmd = argv[++i];
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "socket() failed\n";
        return 1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "invalid host: " << host << "\n";
        close(fd);
        return 1;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "could not connect to " << host << ":" << port << "\n";
        close(fd);
        return 1;
    }

    auto run_one = [&](const std::string& cmd) -> bool {
        if (!send_all(fd, cmd + "\n")) {
            std::cerr << "send failed\n";
            return false;
        }
        std::string resp;
        if (!read_line(fd, resp)) {
            std::cerr << "connection closed by server\n";
            return false;
        }
        std::cout << resp << "\n";
        return true;
    };

    if (!exec_cmd.empty()) {
        bool ok = run_one(exec_cmd);
        close(fd);
        return ok ? 0 : 1;
    }

    std::cout << "tinykv> (Ctrl-D to quit)\n";
    std::string line;
    while (true) {
        std::cout << "tinykv> " << std::flush;
        if (!std::getline(std::cin, line)) {
            break;
        }
        if (line.empty()) {
            continue;
        }
        if (!run_one(line)) {
            break;
        }
    }
    close(fd);
    return 0;
}
