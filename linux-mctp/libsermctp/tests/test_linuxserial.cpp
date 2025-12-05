/**
 * @file bridge_stress.cpp
 * @brief A transfer test for the MctpLinuxSerial object.
 * 
 * A simplifed traffic test that tests the MctpLinuxSerial transfer capabilities
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
#include "sermctp/detail/MctpFramer_impl.hpp"
#include "sermctp/detail/MctpBridge_impl.hpp"
#include "BcastMessenger.hpp"
#include "MctpNetlink_impl.hpp"
#include "sermctp/detail/LinuxMctpSerial_impl.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <signal.h>
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>

extern "C" {
    #include <sys/socket.h>
    #include <sys/un.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <stdlib.h>
    #include <arpa/inet.h>
    #include <linux/mctp.h>
#include <sys/epoll.h>
}

#define MAX_EVENTS 4
#define TIMEOUT_MS 2000  // 2 seconds

/**
 * @brief Run the MctpLinuxSerial transfer test as a callable for the
 * aggregated `test_runner` harness.
 *
 * Returns true on success, false on failure.
 */
bool linuxserial_test() {
    // Ensure we are running as root — test requires privileged interfaces
    if (geteuid() != 0) {
        std::cerr << "This test must be run as root (sudo). Exiting.\n";
        return false;
    }

    // Start two mctp serial devices.
        LinuxMctpSerial linuxSerial0, linuxSerial1;

        // having no local EID allows these bridges to be transparent. Pass an explicit empty name (not the same
        // variable being initialized) to avoid undefined behavior.
        std::string actual_if0 = linuxSerial0.initialize("", 0, {8});
        std::string actual_if1 = linuxSerial1.initialize("", 0, {9});
    linuxSerial0.show_diagnostics(false);
    linuxSerial1.show_diagnostics(false);
    if (actual_if0.empty() || actual_if1.empty()) {
        std::cerr << "Failed to initialize LinuxMctpSerial devices.\n";
        return false;
    }
    std::cerr << "Initialized LinuxMctpSerial devices: " << actual_if0 << " and " << actual_if1 << "\n";
    sleep(1); // wait for interfaces to stabilize

    // Construct the MCTP messages (send will happen after fd setup)
    std::vector<uint8_t> pkt[2];
    pkt[0] = { 0x01, 0x09, 0x08, 0xC8, 0x01, 0x55, 0xAA };
    pkt[1] = { 0x01, 0x08, 0x09, 0xC8, 0x02, 0x66, 0xBB };

    // set up epoll to monitor both fds
    LinuxMctpSerial *endpoints[2] = { &linuxSerial0, &linuxSerial1 };
    int fd[2] = { linuxSerial0.getIsRxReadyFd(), linuxSerial1.getIsRxReadyFd() };
    int efd = epoll_create1(0);
    if (efd < 0) { perror("epoll_create1"); return false; }
    struct epoll_event ev = {};
    ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    for (int idx = 0; idx < (int)(sizeof(fd)/sizeof(fd[0])); ++idx) {
        ev.data.fd = fd[idx];
        if (epoll_ctl(efd, EPOLL_CTL_ADD, fd[idx], &ev) < 0) {
            perror("epoll_ctl ADD"); close(efd); return false;
        }
    }
    struct epoll_event events[MAX_EVENTS];

    // send the packets
    linuxSerial0.send(pkt[0]);
    linuxSerial1.send(pkt[1]);
    std::cerr << "Sent packets on both interfaces, now waiting to receive...\n";

    // Use chrono steady_clock to enforce an overall deadline for the loop
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::milliseconds(TIMEOUT_MS);
    int count = 0;
    while ((std::chrono::steady_clock::now() < deadline) && (count < 2)) {
        auto now = std::chrono::steady_clock::now();
        int remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (remaining < 0) remaining = 0;

        int n = epoll_wait(efd, events, MAX_EVENTS, remaining);
        if (n < 0) { if (errno == EINTR) continue; perror("epoll_wait"); break; }
        if (n == 0) { continue; } // iteration timed out;

        std::cerr << "epoll_wait returned " << n << " events\n";
        // process each event
        for (int i = 0; i < n && count < 2; ++i) {
            for (int idx = 0; idx < (int)(sizeof(fd)/sizeof(fd[0])); ++idx) {
                if (events[i].data.fd == fd[idx]) {
                    while (endpoints[idx]->isPacketAvailable()) {
                        LinuxMctpSerial &linuxSerial = *endpoints[idx];
                        std::vector<uint8_t> message;
                        ssize_t r = linuxSerial.receive(message);
                        std::cout << "Received " << r << " bytes on interface " << idx << ": ";
                        for (uint8_t b : message) {
                            std::cout << std::hex << static_cast<int>(b) << " ";
                        }
                        std::cout << std::dec << std::endl;
                        if (message == pkt[(idx+1)&1]) {
                            count ++;
                        } else {
                            std::cerr << "Received unexpected message on interface " << idx << " (len=" << r << ")\n";
                        }
                        break;
                    }
                }
            }
        }
    }

    // cleanup
    epoll_ctl(efd, EPOLL_CTL_DEL, fd[0], nullptr);
    epoll_ctl(efd, EPOLL_CTL_DEL, fd[1], nullptr);
    close(fd[0]);
    close(fd[1]);
    close(efd);

    if (count == 2) {
        std::cerr << "Successfully received both messages (PASS)\n";
        return true;
    } else {
        std::cerr << "Did not receive both messages within timeout (FAIL)\n";
        return false;
    }
}
