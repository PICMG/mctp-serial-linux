// Tests for BcastMessenger: exercise setup, acceptConnection, sendMessage, recvMessage
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <sstream>

#include "BcastMessenger.hpp"
#include "mctp_random_messages_embedded.h"

extern "C" {
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/select.h>
}

static std::vector<std::vector<uint8_t>> parse_raw_frames(const std::string &path) {
    std::vector<std::vector<uint8_t>> out;
    std::ifstream f(path);
    if (!f.is_open()) return out;

    std::string line;
    while (std::getline(f, line)) {
        auto pos = line.find("\"raw_frame\"");
        if (pos == std::string::npos) continue;
        auto colon = line.find(':', pos);
        if (colon == std::string::npos) continue;
        auto qstart = line.find('"', colon);
        if (qstart == std::string::npos) continue;
        auto qend = line.rfind('"');
        if (qend == std::string::npos || qend <= qstart) continue;
        std::string frame = line.substr(qstart + 1, qend - (qstart + 1));
        std::istringstream ss(frame);
        std::string tok;
        std::vector<uint8_t> bytes;
        while (ss >> tok) {
            if (tok.size() >= 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
                try {
                    unsigned v = std::stoul(tok, nullptr, 16);
                    bytes.push_back(static_cast<uint8_t>(v & 0xff));
                } catch (...) {}
            }
        }
        if (!bytes.empty()) out.push_back(bytes);
    }
    return out;
}

static std::vector<std::vector<uint8_t>> parse_raw_frames_from_stream(std::istream &f) {
    std::vector<std::vector<uint8_t>> out;
    std::string line;
    while (std::getline(f, line)) {
        auto pos = line.find("\"raw_frame\"");
        if (pos == std::string::npos) continue;
        auto colon = line.find(':', pos);
        if (colon == std::string::npos) continue;
        auto qstart = line.find('"', colon);
        if (qstart == std::string::npos) continue;
        auto qend = line.rfind('"');
        if (qend == std::string::npos || qend <= qstart) continue;
        std::string frame = line.substr(qstart + 1, qend - (qstart + 1));
        std::istringstream ss(frame);
        std::string tok;
        std::vector<uint8_t> bytes;
        while (ss >> tok) {
            if (tok.size() >= 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
                try {
                    unsigned v = std::stoul(tok, nullptr, 16);
                    bytes.push_back(static_cast<uint8_t>(v & 0xff));
                } catch (...) {}
            }
        }
        if (!bytes.empty()) out.push_back(bytes);
    }
    return out;
}

static bool write_all_fd(int fd, const std::vector<uint8_t> &data) {
    size_t total = 0;
    const uint8_t *ptr = data.data();
    while (total < data.size()) {
        ssize_t w = ::send(fd, ptr + total, data.size() - total, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        total += static_cast<size_t>(w);
    }
    return true;
}

static bool read_exact_fd(int fd, size_t expected, std::vector<uint8_t> &out, int timeout_ms) {
    out.clear();
    auto start = std::chrono::steady_clock::now();
    while (out.size() < expected) {
        auto now = std::chrono::steady_clock::now();
        int elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
        if (elapsed >= timeout_ms) return false;
        int remaining = timeout_ms - elapsed;
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv;
        tv.tv_sec = remaining / 1000;
        tv.tv_usec = (remaining % 1000) * 1000;
        int r = select(fd + 1, &rf, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(fd, &rf)) {
            uint8_t buf[1024];
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0) {
                out.insert(out.end(), buf, buf + n);
            } else if (n == 0) {
                return false;
            } else {
                if (errno == EINTR) continue;
                return false;
            }
        } else if (r == 0) {
            continue;
        } else if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
    }
    return true;
}

// Exposed test function
bool BcastMessenger_transfer() {
    std::cout << "BcastMessenger transfer test starting\n";

    // Use the embedded JSON fixture for test messages (no external JSON file).
    std::vector<std::vector<uint8_t>> msgs;
    {
        std::string embedded = get_embedded_mctp_random_messages_json();
        std::istringstream iss(embedded);
        msgs = parse_raw_frames_from_stream(iss);
    }
    if (msgs.empty()) { std::cerr << "No embedded messages found for Bcast test\n"; return false; }

    // socket path
    std::string sock = "/tmp/test-bcast-" + std::to_string(::getpid()) + ".sock";

    BcastMessenger srv;
    if (!srv.open(sock)) { std::cerr << "setup failed\n"; return false; }

    // Create client socket and connect
    int client_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (client_fd < 0) { perror("socket"); return false; }
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path)-1);

    // Attempt connect; may need to retry a few times while server is ready
    bool connected = false;
    for (int i = 0; i < 20; ++i) {
        if (connect(client_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) { connected = true; break; }
        if (errno == ENOENT) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        // other errors may still be transient
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!connected) { perror("connect"); close(client_fd); return false; }

    // Ensure server accepts connection
    bool srv_connected = false;
    for (int i = 0; i < 50; ++i) {
        try { srv.acceptConnection(); } catch(...) {}
        if (srv.isConnected()) { srv_connected = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!srv_connected) { std::cerr << "server did not accept connection\n"; close(client_fd); return false; }

    bool all_ok = true;
    const int timeout_ms = 1000;

    // Bidirectional exchange: client -> server, then server -> client
    for (size_t i = 0; i < msgs.size(); ++i) {
        const auto &frame = msgs[i];

        // Client -> server: client sends, server should recv via recvMessage()
        if (!write_all_fd(client_fd, frame)) { std::cerr << "client write failed\n"; all_ok = false; break; }
        // wait for server to receive
        std::vector<uint8_t> srecv;
        bool got = false;
        for (int t = 0; t < 100 && !got; ++t) {
            srecv = srv.recvMessage();
            if (!srecv.empty()) got = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!got) { std::cerr << "server did not receive frame " << i << "\n"; all_ok = false; break; }
        if (srecv != frame) { std::cerr << "server frame mismatch " << i << "\n"; all_ok = false; break; }

        // small pause
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        // Server -> client
        srv.sendMessage(frame);
        std::vector<uint8_t> crecv;
        if (!read_exact_fd(client_fd, frame.size(), crecv, timeout_ms)) { std::cerr << "client did not receive server frame " << i << "\n"; all_ok = false; break; }
        if (crecv != frame) { std::cerr << "client frame mismatch " << i << "\n"; all_ok = false; break; }

        // small pause between messages
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    close(client_fd);
    std::cout << "BcastMessenger transfer test " << (all_ok ? "PASSED" : "FAILED") << "\n";
    return all_ok;
}
