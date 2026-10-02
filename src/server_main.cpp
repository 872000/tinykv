// TinyKV — TCP server entry point (POSIX sockets, one thread per client).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "commands.hpp"
#include "kvstore.hpp"
#include "protocol.hpp"

namespace {

volatile std::sig_atomic_t g_interrupted = 0;

void on_signal(int) { g_interrupted = 1; }

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

void handle_client(int fd, tinykv::KVStore& store,
                   std::atomic<bool>& shutdown_flag) {
    std::string buf;
    char tmp[4096];
    while (!shutdown_flag.load()) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            break;
        }
        buf.append(tmp, static_cast<std::size_t>(n));
        std::size_t pos = 0;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            std::vector<std::string> argv = tinykv::parse_line(line);
            if (argv.empty()) {
                continue;
            }
            std::string resp = tinykv::execute(store, argv, shutdown_flag) + "\n";
            if (!send_all(fd, resp)) {
                close(fd);
                return;
            }
            if (shutdown_flag.load()) {
                break;
            }
        }
    }
    close(fd);
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [--port PORT] [--dir DATA_DIR]\n";
}

}  // namespace

int main(int argc, char** argv) {
    int port = 6379;
    std::string data_dir = "./tinykv-data";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if ((arg == "--dir" || arg == "-d") && i + 1 < argc) {
            data_dir = argv[++i];
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "socket() failed\n";
        return 1;
    }
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // localhost only
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind() failed on 127.0.0.1:" << port << "\n";
        close(listen_fd);
        return 1;
    }
    if (listen(listen_fd, 16) < 0) {
        std::cerr << "listen() failed\n";
        close(listen_fd);
        return 1;
    }

    tinykv::KVStore store(data_dir);
    std::atomic<bool> shutdown_flag{false};

    // Background sweeper: purge expired keys once a second.
    std::thread sweeper([&store, &shutdown_flag] {
        while (!shutdown_flag.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            store.sweep_expired();
        }
    });

    std::cout << "TinyKV listening on 127.0.0.1:" << port
              << " (data dir: " << data_dir << ")\n";

    while (!shutdown_flag.load() && !g_interrupted) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        timeval tv{0, 200000};  // 200 ms
        int ready = select(listen_fd + 1, &rfds, nullptr, nullptr, &tv);
        if (ready < 0) {
            break;
        }
        if (ready == 0) {
            continue;
        }
        int client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            continue;
        }
        std::thread(handle_client, client_fd, std::ref(store),
                    std::ref(shutdown_flag))
            .detach();
    }

    shutdown_flag.store(true);
    sweeper.join();
    store.save();
    close(listen_fd);
    std::cout << "TinyKV shut down (snapshot saved).\n";
    return 0;
}
