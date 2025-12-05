/**
 * @file LinuxMctpSerial.cpp
 * @brief Class for managing MCTP serial links over Linux TTY interfaces.
 *
 * This class provides functionality to initialize and tear down MCTP serial links
 * by configuring a TTY device with the MCTP line discipline, monitoring the creation
 * and removal of associated mctpserial network interfaces, and managing terminal settings.
 *
 * @author Doug Sandy (doug@picmg.org)
 * @license MIT No Attribution (MIT-0)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
 */
#include "sermctp/detail/LinuxMctpSerial_impl.hpp"
#include "MctpNetlink_impl.hpp"
#include "sermctp/detail/MctpFramer_impl.hpp"
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <regex>
#include <algorithm>
extern "C" {
    #include <sys/socket.h>
    #include <termios.h>
    #include <net/if.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <linux/if_packet.h>
    #include <linux/mctp.h>
    #include <linux/if_ether.h>
}
#include <cerrno>

/**
 * @brief Constructor for LinuxMctpSerial.
 *
 * Initializes internal state and sets file descriptor to invalid.
 */
LinuxMctpSerial::LinuxMctpSerial() : pty_master_fd(-1), pty_slave_fd(-1), rx_fd(-1) {
}

/**
 * @brief Destructor for LinuxMctpSerial.
 *
 * Automatically calls close() to clean up resources.
 */
LinuxMctpSerial::~LinuxMctpSerial() {
    close();
}

/**
 * @brief Initializes the MCTP serial link.
 *
 * Opens the TTY device, configures it for raw mode with no flow control,
 * sets the MCTP line discipline, and waits for a new mctpserial device to appear.
 * If a previous link was active, it will be closed first.
 *
 * @param mctp_if_name Desired name for the mctpserial interface (optional).
 * @param local_eid Local Endpoint ID for the MCTP interface.
 * @param peer_eids Vector of peer Endpoint IDs for the MCTP interface.
 * @return std::string Name of the newly created mctpserial device (e.g., "mctpserial0").
 * @throws std::runtime_error if initialization fails or times out.
 */
std::string LinuxMctpSerial::initialize(const std::string& mctp_if_name, uint8_t local_eid, std::vector<uint8_t> peer_eids) {
    // If already initialized, close first
    if (pty_master_fd != -1 || pty_slave_fd != -1 || rx_fd != -1) {
        close();
    }

    // get the list of existing mctpserial devices
    std::vector<std::string> before = list_mctp_devices();

    // Create PTY master
    int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd < 0) {
        throw std::runtime_error("Failed to posix_openpt");
    }
    this->pty_master_fd = master_fd;
    if (grantpt(master_fd) < 0 || unlockpt(master_fd) < 0) {
        close();
        throw std::runtime_error("Failed to grant/unlock pty");
    }
    char *slave_name = ptsname(master_fd);
    if (!slave_name) {
        close();
        throw std::runtime_error("ptsname() failed");
    }

    // Save the slave name for optional external observation
    pty_slave_name = std::string(slave_name);

    // Open slave side so we can configure it (baud, raw, and line discipline)
    int slave_fd = ::open(slave_name, O_RDWR | O_NOCTTY);
    if (slave_fd < 0) {
        close();
        throw std::runtime_error(std::string("Failed to open slave PTY: ") + slave_name);
    }
    this->pty_slave_fd = slave_fd;

    // Set non-blocking on slave
    int flags = fcntl(slave_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(slave_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close();
        throw std::runtime_error("Failed to set non-blocking on slave PTY");
    }

    // set non-blocking on the master
    flags = fcntl(master_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(master_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close();
        throw std::runtime_error("Failed to set non-blocking on master PTY");
    }

    // Put the slave PTY into raw/no-echo mode so framed binary data
    // written to the master is not echoed back by the terminal layer.
    // This avoids the framer seeing its own transmitted bytes as input.
    struct termios tty_before, tty_slave, tty_after;
    if (tcgetattr(slave_fd, &tty_before) == 0) {
        // apply raw settings
        cfmakeraw(&tty_slave);
        tty_slave.c_cc[VMIN] = 1;
        tty_slave.c_cc[VTIME] = 0;
        if (tcsetattr(slave_fd, TCSANOW, &tty_slave) != 0) {
            std::cerr << "Warning: failed to set raw mode on slave PTY: " << strerror(errno) << "\n";
        } 
    } else {
        std::cerr << "Warning: tcgetattr failed on slave PTY: " << strerror(errno) << "\n";
    }

    // Set MCTP line discipline on the slave
    int ldisc = N_MCTP;
    if (ioctl(slave_fd, TIOCSETD, &ldisc) < 0) {
        perror("ioctl TIOCSETD");
        close();
        throw std::runtime_error("Failed to set MCTP line discipline on slave PTY");
    }

    // Optionally rename mctp interface when it appears (this loop will wait up to 1s)
    for (int i = 0; i < 10; ++i) {
        // get a list of current mctp devices
        std::vector<std::string> after = list_mctp_devices();

        // loop for each device name in after
        for (const auto& dev : after) {
            // see if this device was not present before
            if (std::find(before.begin(), before.end(), dev) == before.end()) {
                // set the new interface name 
                this->mctp_if_name = dev;
                // if a new name was requested, attempt to set it now
                if (!mctp_if_name.empty()) {
                    if (!mctpnet::setMctpInterfaceName(dev, mctp_if_name)) {
                        // close the framer and its pipe
                        close();
                        throw std::runtime_error("Failed to set mctpserial interface name to " + mctp_if_name);
                    }
                    this->mctp_if_name = mctp_if_name;
                    break;
                }
            }
        }
        if (this->mctp_if_name.empty()) break; // new interface has been found and (optionally) named
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (this->mctp_if_name.empty()) {
        // close the slave and master fds
        close();
        throw std::runtime_error("Timed out waiting for new mctpserial device");
    }

    // program the netlink attributes for this MCTP interface
    if (local_eid != 0) {
        if (!mctpnet::setMctpLocalEid(this->mctp_if_name, local_eid)) {
            std::cerr << "Warning: setMctpLocalEid failed for " << this->mctp_if_name << "\n";
        }
    }
    this->local_eid = local_eid;
    if (!mctpnet::setMctpInterfaceStatus(this->mctp_if_name, true)) {
        std::cerr << "Warning: setMctpInterfaceStatus up failed for " << this->mctp_if_name << "\n";
    }
    for (uint8_t peer_eid : peer_eids) {
        if (!mctpnet::addMctpRoute(this->mctp_if_name, peer_eid)) {
            std::cerr << "Warning: addMctpRoute failed for dest " << static_cast<int>(peer_eid) << " via " << this->mctp_if_name << "\n";
        }
    }
    
    // Wait a short while for kernel route/address propagation so early sends
    // don't race with netlink updates. This avoids transient EHOSTUNREACH
    // observed in short instrumented runs.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Attach the embedded framer to the master side of our pty so that it sees 
    // kernel <-> slave traffic.
    if (framer.openFd(master_fd) < 0) {
        std::cerr << "LinuxMctpSerial::initialize: framer_.openFd failed\n";
        close();
        throw std::runtime_error("Failed to attach framer to PTY master");
    }

    // attach and bind our Rx communication socket to the MCTP interface
    int ifindex = if_nametoindex(this->mctp_if_name.c_str());
    if (!ifindex) { 
        perror("if_nametoindex"); 
        close();
        throw std::runtime_error("Failed to get interface index");
    }

    // Use the embedded framer's ready FD as our RX trigger so callers
    // can poll/epoll for inbound framed MCTP messages on the PTY master
    this->rx_fd = framer.getFrameReadyFd();
    if (this->rx_fd < 0) {
        close();
        throw std::runtime_error("Failed to acquire framer ready fd");
    }

    // make rx_fd non-blocking so receive() can behave like before
    int rx_flags = fcntl(rx_fd, F_GETFL, 0);
    if (rx_flags >= 0) {
        fcntl(rx_fd, F_SETFL, rx_flags | O_NONBLOCK);
    }

    // Return the active mctpserial interface name
    return this->mctp_if_name;
}

/**
 * @brief Closes the active MCTP serial link.
 *
 * Resets the line discipline to N_TTY, closes the file descriptor,
 * and waits for the associated mctpserial device to disappear.
 * If no link is active, this function does nothing.
 */
void LinuxMctpSerial::close() {
    // Stop and detach embedded framer first so its recv thread exits and
    // duplicates of the master fd are closed.
    framer.close();
    // framer.close() will close its internal pipe fds; clear our rx_fd
    // grab so we don't attempt to close it again below.
    rx_fd = -1;

    // Small pause to give framer threads a moment to exit and the
    // kernel to settle outstanding work. This avoids racing the
    // unregister path which otherwise may see active references.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Now set the line discipline back to the default (N_TTY) using the
    // slave fd that we opened earlier. This must happen before closing
    // the slave/master fds so the kernel can properly unregister the
    // mctpserial device.
    if (pty_slave_fd >= 0) {
        int ldisc = N_TTY;
        if (ioctl(pty_slave_fd, TIOCSETD, &ldisc) < 0) {
            perror("ioctl TIOCSETD failed");
        }
        ::close(pty_slave_fd);
        pty_slave_fd = -1;
    }

    // Close master fd
    if (pty_master_fd >= 0) {
        ::close(pty_master_fd);
        pty_master_fd = -1;
    }

    // Close rx socket
    if (rx_fd >= 0) {
        ::close(rx_fd);
        rx_fd = -1;
    }

    // Wait for the associated mctpserial device to disappear
    for (int i = 0; i < 50; ++i) {
        std::vector<std::string> current = list_mctp_devices();
        if (std::find(current.begin(), current.end(), mctp_if_name) == current.end()) {
            mctp_if_name.clear();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cerr << "Warning: mctpserial device did not disappear after close()" << std::endl;
}

/**
 * @brief Returns the name of the active mctpserial device.
 *
 * @return std::string Name of the mctpserial device (e.g., "mctpserial0"),
 *         or empty string if no device is active.
 */
std::string LinuxMctpSerial::getMctpIfName() const {
    return mctp_if_name;
}

/**
 * @brief Returns a duplicate of the Rx-ready file descriptor.
 *
 * This file descriptor can be used with select() or epoll() to monitor
 * for incoming MCTP messages on the serial interface.
 *
 * @return int Duplicate of the Rx-ready file descriptor, or -1 if not initialized.
 */
int LinuxMctpSerial::getIsRxReadyFd() const {
    return rx_fd;
}

/**
 * @brief Sends an MCTP message via the embedded framer.
 *
 * This function delegates the send operation to the internal MctpFramer
 * instance, which handles framing and transmission over the serial link.
 * The linux mctp core will intercept the framed data and route it appropriately.
 *
 * @param msg Vector of bytes representing the MCTP message payload.
 */
void LinuxMctpSerial::send(const std::vector<uint8_t>& msg) {
    // Delegate to framer for transmission over the TTY
    framer.send(msg);
}

/**
 * @brief Receives an MCTP message from the MCTP socket listener
 * 
 * Note: This function reads raw MCTP messages from the socket bound to 
 * the mctpserial interface.  Using an interface-bound socket allows
 * all packets to be received (no type filtering), and does not require
 * neighbor or route entries.
 * 
 * @param result Vector to store the received MCTP message payload.
 * @return ssize_t Number of bytes received, or 0 on error.
 */
ssize_t LinuxMctpSerial::receive(std::vector<uint8_t>& result) {
    // Read a parsed, de-framed message from the embedded framer.
    // The framer returns an empty vector when no complete frame is available.
    std::vector<uint8_t> pkt = framer.receive();
    if (pkt.empty()) {
        result = {};
        return 0;
    }

    // Return the raw bytes (these are the de-framed message bytes produced
    // by the framer: the MCTP transport bytes as provided by the sender).
    result = std::move(pkt);
    return static_cast<ssize_t>(result.size());
}

/**
 * @brief Returns the local Endpoint ID for the MCTP interface.
 */
int LinuxMctpSerial::getLocalEid() const {
    return static_cast<int>(local_eid);
}   

/**
 * @brief Lists all currently available mctpserial devices.
 *
 * Scans /sys/class/net/ for entries matching the pattern "mctpserial[0-9]+".
 *
 * @return std::vector<std::string> List of mctpserial device names.
 */
std::vector<std::string> LinuxMctpSerial::list_mctp_devices() {
    const std::string net_path = "/sys/class/net/";
    DIR* dir = opendir(net_path.c_str());
    std::vector<std::string> devices;

    if (!dir) return devices;

    struct dirent* entry;
    std::regex pattern("^mctpserial[0-9]+$");

    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_LNK && std::regex_match(entry->d_name, pattern)) {
            devices.push_back(entry->d_name);
        }
    }

    closedir(dir);
    return devices;
}

/**
 * @brief Checks if there is a packet available to read.
 * 
 * @return true if a packet is available, false otherwise. 
 */
bool LinuxMctpSerial::isPacketAvailable() const {
    return framer.isFrameAvailable();
}

/**
 * @brief Enables or disables diagnostic output in the embedded framer.
 * 
 */
void LinuxMctpSerial::show_diagnostics(bool enable) {
    framer.show_diagnostics(enable);
}
