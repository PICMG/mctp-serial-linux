/**
 * @file bridge_stress.cpp
 * @brief A short stress test for the MctpBridge using a virtual serial link.
 * 
 * Standalone stress test for the real MctpBridge object.
 * Run this program under sudo on a host with MCTP serial line discipline
 * and AF_MCTP support. This program creates a VirtualSerial, starts the
 * real bridge (requesting interface name "testbridge"), then floods traffic
 * in both directions and verifies delivery. It is intended for interactive
 * debugging and should NOT be wired into the main test-runner.
 * 
 * @author Doug Sandy
 * 
 * @license MIT No Attribution (MIT-0)
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED.
 */
#include "MctpFramer.hpp"
#include "MctpBridge.hpp"
#include "LinuxMctpSerial.hpp"
#include "MctpNetlink_impl.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <csignal>
#include <random>

extern "C" {
    #include <sys/socket.h>
    #include <sys/un.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <stdlib.h>
    #include <arpa/inet.h>
    #include <sys/epoll.h>
    #include <linux/mctp.h>
}

static int nullmodem_master_fd = -1;
static int bcast_fd = -1;
static MctpBridge bridge;          // the unit under test
static MctpFramer serial_framer;   // framer for simulating TTY connection for the bridge
static LinuxMctpSerial linux_mctp; // linux MCTP interface - acts as system master

/**
 * @brief Cleans up and closes all open resources.
 */
void restore_and_close() {
    bridge.close();
    serial_framer.close();
    linux_mctp.close();
    if (nullmodem_master_fd >= 0) { close(nullmodem_master_fd); nullmodem_master_fd = -1; }
    if (bcast_fd >= 0) { close(bcast_fd); bcast_fd = -1; }
}

/**
 * @brief Signal handler for clean shutdown on SIGINT and SIGTERM.
 */
static void handle_sig(int) { 
    restore_and_close(); 
}

/**
 * @brief Creates a random payload with a given header and maximum length.
 * 
 * @param header The header bytes to prepend to the payload.
 * @param max_length The maximum length of the random payload.
 * @return std::vector<uint8_t> The generated random payload.
 */
std::vector<uint8_t> createRandomPayload(std::vector<uint8_t> header,unsigned int max_length) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<int> len_dist(1, max_length);
    std::uniform_int_distribution<int> byte_dist(0, 255);

    int len = len_dist(gen);
    std::vector<uint8_t> v = header;
    v.reserve(len);
    for (int i = 0; i < len; ++i)
        v.push_back(static_cast<uint8_t>(byte_dist(gen)));
    return v;
}

bool runTests() {
    int epfd = epoll_create1(0);
    if (epfd == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    int linux_fd = linux_mctp.getIsRxReadyFd();
    int serial_fd = serial_framer.getFrameReadyFd(); 
    std::vector<int> fds_to_monitor = { serial_fd, linux_fd, bcast_fd };
    for (int fd : fds_to_monitor) {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
            perror("epoll_ctl: adding fd");
            close(epfd);
            return false;
        }
    }

    int tx_bcast_to_serial = 0; int rx_bcast_to_serial = 0;
    int tx_serial_to_bcast = 0; int rx_serial_to_bcast = 0;
    int tx_linux_to_serial = 0; int rx_linux_to_serial = 0;
    int tx_serial_to_linux = 0; int rx_serial_to_linux = 0;

    // Send Serial to Linux
    std::vector<uint8_t> pkt = createRandomPayload({ 0x01, 9, 8, 0xC8, 0x01}, 16);
    serial_framer.send(pkt); tx_serial_to_linux++;

    // Send Serial to Broadcast
    pkt = createRandomPayload({ 0x01, 0xff, 0x08, 0xc8, 0x01}, 16);
    serial_framer.send(pkt); tx_serial_to_bcast++;

    // Send Broadcast to Serial
    pkt = createRandomPayload({ 0x01, 0xff, 0x00, 0xc8, 0x01}, 16);
    send(bcast_fd, pkt.data(), pkt.size(), 0); tx_bcast_to_serial++;

    // Linux to Serial
    pkt = createRandomPayload({ 0x01, 0x08, 0x9, 0xC8, 0x01}, 16);   
    linux_mctp.send(pkt); tx_linux_to_serial++;

    // loop for up to 2 seconds trying to receive data
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const int max_duration = 2;
    struct epoll_event events[1];
    while (1)  {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - start.tv_sec) >= max_duration) {
            break;
        }

        // wait here for data to arrive or timeout
        int nfds = epoll_wait(epfd, events, 1, 100);
        if (nfds == -1) { perror("epoll_wait"); close(epfd); return false; }
        if (nfds == 0) continue; // timeout, loop again
        
        if (events[0].data.fd == linux_fd) {
            // Recevied serial to linux message - drain the messages
            std::vector<uint8_t> msg;
            linux_mctp.receive(msg); rx_serial_to_linux++;
            // now, send a new message
            std::vector<uint8_t> pkt = createRandomPayload({ 0x01, 0x09, 0x08, 0xC8, 0x01}, 16);
            serial_framer.send(pkt); tx_serial_to_linux++;
        } else if (events[0].data.fd == serial_fd) {    
            // Linux to Serial or Broadcast to Serial packet received - drain the signal
            std::vector<uint8_t> msg = serial_framer.receive();
            if ((msg[1] == 0) || (msg[1] == 0xff)) {
                rx_bcast_to_serial++;
                pkt = createRandomPayload({ 0x01, 0xff, 0x09, 0xc8, 0x01}, 16);
                send(bcast_fd, pkt.data(), pkt.size(), 0); tx_bcast_to_serial++;
            } else {
                rx_linux_to_serial++;
                pkt = createRandomPayload({ 0x01, 0x08, 0x09, 0xC8, 0x01}, 16);   
                linux_mctp.send(pkt); tx_linux_to_serial++;
            }                 
        } else if (events[0].data.fd == bcast_fd) {
            // broadcast packet received - drain the packet
            uint8_t buf[256];
            recv(bcast_fd, buf, sizeof(buf), 0); rx_serial_to_bcast++;

            // send a new broadcast message from serial
            pkt = createRandomPayload({ 0x01, 0xff, 0x08, 0xc8, 0x01}, 16);
            serial_framer.send(pkt); tx_serial_to_bcast++;
        }
    }
    close(epfd);
    
    // check for transmitted vs received counts (allowing for off-by-one)
    if ((tx_bcast_to_serial != rx_bcast_to_serial) && (tx_bcast_to_serial != rx_bcast_to_serial+1)) {
        std::cerr << "[FAIL] Mismatch in broadcast to serial counts: tx = " << tx_bcast_to_serial << ", rx = " << rx_bcast_to_serial << "\n";
        return false;
    }
    if ((tx_serial_to_bcast != rx_serial_to_bcast) && (tx_serial_to_bcast != rx_serial_to_bcast+1)) {
        std::cerr << "[FAIL] Mismatch in serial to broadcast counts: tx = " << tx_serial_to_bcast << ", rx = " << rx_serial_to_bcast << "\n";
        return false;
    }
    if ((tx_linux_to_serial != rx_linux_to_serial) && (tx_linux_to_serial != rx_linux_to_serial+1)) {
        std::cerr << "[FAIL] Mismatch in linux to serial counts: tx = " << tx_linux_to_serial << ", rx = " << rx_linux_to_serial << "\n";
        return false;
    }
    if ((tx_serial_to_linux != rx_serial_to_linux) && (tx_serial_to_linux != rx_serial_to_linux+1)) {
        std::cerr << "[FAIL] Mismatch in serial to linux counts: tx = " << tx_serial_to_linux << ", rx = " << rx_serial_to_linux << "\n";
        return false;
    }

    // traffic should also be balanced in both directions
    if ((tx_bcast_to_serial>tx_serial_to_bcast*1.2)||(tx_serial_to_bcast>tx_bcast_to_serial*1.2)) {
        std::cerr << "[FAIL] Imbalance in broadcast to serial vs serial to broadcast traffic\n";
        return false;
    }
    if ((tx_linux_to_serial>tx_serial_to_linux*1.2)||(tx_serial_to_linux>tx_linux_to_serial*1.2)) {
        std::cerr << "[FAIL] Imbalance in linux to serial vs serial to linux traffic\n";
        return false;
    }
    std::cout << "[PASS] All transmitted messages were received successfully.\n";
    return true;
}

/**
 * @brief Configures the broadcast interface by creating and connecting
 *        a UNIX domain socket to the specified broadcast socket name.
 * 
 * @param name The UNIX domain socket path for the broadcast interface.
 * @return true on successful connection, false on failure.
 */
bool configureBroadcastInterface(std::string name) {
    // Create client socket and connect
    bcast_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (bcast_fd < 0) { 
        perror("open_broadcast_socket"); 
        restore_and_close();
        return 1; 
    }
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, name.c_str(), sizeof(addr.sun_path)-1);

    // Attempt connect; may need to retry a few times while server is ready
    bool connected = false;
    for (int i = 0; i < 20; ++i) {
        if (connect(bcast_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
             connected = true; 
             return true; 
        }
        if (errno == ENOENT) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        // other errors may still be transient
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return false; 
}

/**
 * @brief Main entry point for the bridge stress test.
 * 
 * Sets up a virtual serial link, starts the MctpBridge,
 * and runs stress tests sending traffic in both directions.
 * 
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return int Exit code: 0 on success, 1 on failure, 2 if not run as root.
 */
// Expose a test function that can be called by the aggregated `test_runner`.
// The original `main()` logic is preserved but moved into this function so
// the harness can call it directly. The function returns `true` on success
// and `false` on failure. If the test cannot run because it requires root
// privileges, it will print a notice and return `true` so CI/unit runs that
// are non-privileged do not fail (integration runs should execute it as root).
bool bridge_stress_test() {
    if (geteuid() != 0) {
        std::cerr << "[ FAIL ] bridge_stress: requires root; run the test runner with sudo.\n";
        return false;
    }

    // capture signals to allow clean shutdown
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    // Create PTY master
    int nullmodem_master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (nullmodem_master_fd < 0) {
        throw std::runtime_error("Failed to posix_openpt");
    }
    if (grantpt(nullmodem_master_fd) < 0 || unlockpt(nullmodem_master_fd) < 0) {
        close(nullmodem_master_fd);
        throw std::runtime_error("Failed to grant/unlock pty");
    }
    char *nullmodem_slave_name = ptsname(nullmodem_master_fd);
    if (!nullmodem_slave_name) {
        restore_and_close();
        throw std::runtime_error("ptsname() failed");
    }

    // Start the real bridge. Leave the requested interface name empty so the
    // kernel assigns the default mctpserial name; some systems may behave
    // differently when an explicit name is requested.
    if (!bridge.open(nullmodem_slave_name, BaudRate::BR_115200, false, 0, {8})) {
        std::cout << "bridge.open() failed\n";
        restore_and_close();
        return false;
    }

    // Attach a framer to the serial-side connection (this duplicates fd)
    int serial_pipe = serial_framer.openFd(nullmodem_master_fd);
    if (serial_pipe < 0) {
        std::cerr << "serial_framer.openFd() failed\n";
        restore_and_close();
        return false;
    }

    // expose the framer's internal interrupt write-end to the signal handler so
    // the handler can poke the framer's select() safely (write is signal-safe).
    int framer_wfd = serial_framer.getFrameReadyFd();

    // configure the linux-side communication
    std::string linux_if_name;
    // Pass an explicit empty name to avoid using an uninitialized variable
    linux_if_name = linux_mctp.initialize("", 0, {9});

    // configure the broadcast interface
    if (!configureBroadcastInterface(bridge.getBroadcastName())) {
        std::cerr << "Failed to configure broadcast interface\n";
        restore_and_close();
        return false;
    }

    bool passed = runTests();

    restore_and_close();
    return passed;
}
