/**
 * @file MctpBridge.cpp
 * @brief Class for managing MCTP serial links over Linux TTY interfaces.
 *
 * This class provides functionality to initialize and tear down MCTP serial links
 * by configuring a TTY device with the MCTP line discipline, monitoring the creation
 * and removal of associated mctpserial network interfaces, and managing terminal settings.
 *
 * `open()` now accepts an optional `use_id_path_tag` flag. When
 * `use_id_path_tag` is true the `tty_path` argument is treated as an
 * `ID_PATH_TAG` udev identifier and the bridge will instantiate a
 * `ManagedUsbTty` helper which monitors udev and binds to the physical USB
 * tty matching the provided ID. In that mode the bridge uses the duplicated
 * PTY slave fd returned by `ManagedUsbTty::open()` as the serial fd.
 *
 * Note on ID_PATH_TAG
 * -------------------
 * The udev property `ID_PATH_TAG` identifies the physical path of a USB
 * device (the bus topology) and is used by this implementation to match a
 * stable identifier for the target serial device. To query `ID_PATH_TAG` on
 * the command line for a device node, run:
 *    udevadm info -q property -n /dev/ttyUSB0 | grep -E '^(ID_PATH_TAG)='
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
#include "sermctp/detail/MctpBridge_impl.hpp"
#include "sermctp/detail/ManagedUsbTty.hpp"
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <regex>
#include <algorithm>
#include <memory>
#include "BcastMessenger.hpp"
extern "C" {
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/un.h>     // For struct sockaddr_un
    #include <netinet/in.h>
    #include <net/if.h>        // for if_nametoindex
    #include <linux/if_ether.h> // for ETH_P_ALL
    #include <unistd.h>
    #include <termios.h>
    #include <linux/if_packet.h> // use kernel header (included by linux/mctp.h anyway)
    #include <linux/mctp.h>
    #include <pty.h>
    #include <sys/select.h>
    #include <sys/epoll.h>
}
#include <limits.h>


/**
 * @brief Constructs a new MctpBridge object.
 * 
 * Initializes internal components including LinuxMctpSerial, NullModem, MctpSerial,
 * and sets up file descriptors and thread control flags. Actual values are assigned later.
 */
MctpBridge::MctpBridge()
    : tty_fd(-1),
    tty_raw_fd(-1),
    running(false),
    bcast(std::make_unique<BcastMessenger>())
{
}

/**
 * @brief Destructor for MctpBridge.
 *
 * Automatically calls close() to clean up resources.
 */
MctpBridge::~MctpBridge() {
    close();
}

/**
 * @brief Initializes the MCTP serial link on the specified TTY.
 *
 * Opens the TTY device, configures it for raw mode with no flow control,
 * sets the MCTP line discipline, and waits for a new mctpserial device to appear.
 * If a previous link was active, it will be closed first.
 *
 * @param tty_path Path to the TTY device (e.g., "/dev/pts/8").
 * @param baud the baud rate expressed
 * @param hw_flow_control true to enable hardware flow control (RTS/CTS), false to disable. 
 * @param mctp_name the resulting name of the public mctp device to be bound to.  If not empty on entrance,
 *                 this reflects the desired name for the inerface.  It must be unique across the system.
 * @param local_eid Local Endpoint ID for the MCTP interface.
 * @param peer_eids Vector of peer Endpoint IDs for the MCTP interface. 
 * @param use_id_path_tag true to interpret tty_path as an ID_PATH_TAG for ManagedUsbTty usage.
 * @return true on success, otherwise failure.
 */
bool MctpBridge::open(const std::string& tty_path, BaudRate baud, bool hw_flow_control, uint8_t local_eid, std::vector<uint8_t> peer_eids, bool use_id_path_tag) {
    // close if already configured.  this also stops the routing thread
    close();

    this->broadcastName.clear();

    // We'll initialize the Linux MCTP endpoint below once we know whether
    // we're using a direct TTY or an ID_PATH_TAG-backed ManagedUsbTty.

    int fd = -1;
    int pipefd = -1;

    if (!use_id_path_tag) {
        // Initialize Linux endpoint using a fresh PTY master created by LinuxMctpSerial
        try {
            this->ifname = linuxEndpoint.initialize(ifname, local_eid, peer_eids);
        } catch (...) {
            std::cerr << "MctpBridge LinuxMctpEndpoint initialization failed\n";
            close();
            return false;
        }

        this->broadcastName = "/tmp/bcast_"+this->ifname+".sock";
        if (!bcast->open(broadcastName)) {
            std::cout << "MctpBridge broadcast setup failed\n";
            close();
            return false;
        }
        // connect to the physical interface
        // Open and configure the physical TTY here (MctpBridge owns TTY config).
        fd = ::open(tty_path.c_str(), O_RDWR | O_NOCTTY);
        if (fd < 0) {
            std::cerr << "MctpBridge failed to open TTY device\n";
            close();
            return false;
        }

        // Configure termios as requested by caller
        struct termios tty;
        if (tcgetattr(fd, &tty) != 0) {
            ::close(fd);
            close();
            return false;
        }
        cfsetospeed(&tty, static_cast<speed_t>(baud));
        cfsetispeed(&tty, static_cast<speed_t>(baud));
        // Input flags - clear processing and disable software flow control
        tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
        // Output flags - disable post-processing
        tty.c_oflag &= ~OPOST;
        // Control flags - set 8N1
        tty.c_cflag &= ~(CSIZE | PARENB);
        tty.c_cflag |= CS8;
        // Local flags - disable echo and canonical
        tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        // Hardware flow control
        if (!hw_flow_control) {
            tty.c_cflag &= ~CRTSCTS;
        } else {
            tty.c_cflag |= CRTSCTS;
        }

        if (tcsetattr(fd, TCSANOW, &tty) != 0) {
            ::close(fd);
            close();
            return false;
        }

        // Attach the configured fd to our framer so it can start processing.
        pipefd = mctpSerial.openFd(fd);
        if (pipefd < 0) {
            ::close(fd);
            close();
            return false;
        }
        // Keep the original TTY fd for cleanup, but select() on the framer wake pipe
        tty_raw_fd = fd;
        tty_fd = pipefd;
    } else {
        // Use ManagedUsbTty: the passed string is the ID_PATH_TAG
        try {
            managedUsb = std::make_unique<sermctp::detail::ManagedUsbTty>();
        } catch (...) {
            std::cerr << "MctpBridge: failed to allocate ManagedUsbTty\n";
            close();
            return false;
        }

        int slave_dup = managedUsb->open(tty_path, static_cast<int>(baud), hw_flow_control);
        if (slave_dup < 0) {
            std::cerr << "MctpBridge: ManagedUsbTty failed to open ID_PATH_TAG='" << tty_path << "'\n";
            managedUsb.reset();
            close();
            return false;
        }
        // Initialize Linux endpoint as a decoupled kernel-side interface
        // (LinuxMctpSerial creates its own PTY and manages the kernel mctpserial
        // device). Keep LinuxMctpSerial independent of ManagedUsbTty.
        try {
            this->ifname = linuxEndpoint.initialize(ifname, local_eid, peer_eids);
        } catch (...) {
            std::cerr << "MctpBridge LinuxMctpEndpoint initialization failed\n";
            managedUsb->close();
            ::close(slave_dup);
            managedUsb.reset();
            close();
            return false;
        }

        this->broadcastName = "/tmp/bcast_"+this->ifname+".sock";
        if (!bcast->open(broadcastName)) {
            std::cout << "MctpBridge broadcast setup failed\n";
            managedUsb->close();
            ::close(slave_dup);
            managedUsb.reset();
            close();
            return false;
        }

        // Attach duplicated slave fd to framer
        pipefd = mctpSerial.openFd(slave_dup);
        if (pipefd < 0) {
            std::cerr << "MctpBridge: mctpSerial.openFd failed for ManagedUsbTty slave fd\n";
            // stop managedUsb and close the duplicate
            managedUsb->close();
            ::close(slave_dup);
            managedUsb.reset();
            close();
            return false;
        }

        // The bridge owns the dupfd for cleanup
        tty_raw_fd = slave_dup;
        tty_fd = pipefd;
    }

    int linux_fd_diag = linuxEndpoint.getIsRxReadyFd();
    int framer_serial_fd = mctpSerial.getSerialFd();

    running = true;

    // Start the dispatch loop in a thread
    dispatch_thread = std::thread(&MctpBridge::run, this);
    return true;
}

/**
 * @brief Closes the active MCTPbridge and releases its resoures
 */
void MctpBridge::close() {
    // stop the dispatch thread
    running = false;
    if (dispatch_thread.joinable()) {
        dispatch_thread.join();
    }

    // close related devices
    linuxEndpoint.close();
    mctpSerial.close();

     
    // close the original TTY fd (framer close() will close the duplicated fd and pipes)
    // If we used ManagedUsbTty, stop it first so it shuts down worker and master fd
    if (managedUsb) {
        managedUsb->close();
        managedUsb.reset();
    }

    if (tty_raw_fd>=0) {
        ::close(tty_raw_fd);
        tty_raw_fd = -1;
    }
    tty_fd = -1;

    // clear names associated with this bridge
    if (bcast) bcast->close();
    broadcastName.clear(); 
    ifname.clear();
}

/**
 * @brief Run the main bridging function until interrupted
 * 
 * This method runs within a separate thread and is started when
 * the bridge is successfully opened.  
 */
void MctpBridge::run() {
   while (running) {
        // Try to accept a new connection (non-blocking)
        if (!bcast->isConnected()) {
            bcast->acceptConnection();  
        }

        // Prepare descriptors
        int linux_fd = linuxEndpoint.getIsRxReadyFd();

        // Create epoll instance for this iteration (keeps bookkeeping simple)
        int epfd = epoll_create1(0);
        if (epfd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        auto add_ep = [&](int fd) {
            if (fd < 0) return;
            struct epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = fd;
            epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
        };

        add_ep(linux_fd);
        add_ep(tty_fd);
        if (bcast->isConnected()) add_ep(bcast->getFd());

        const int max_events = 8;
        struct epoll_event events[max_events];
        int timeout_ms = 500; // 0.5s
        int n = epoll_wait(epfd, events, max_events, timeout_ms);
        if (n <= 0) {
            ::close(epfd);
            continue;
        }

        // Prefer handling broadcast messages found in the ready set first
        bool linux_ready = false;
        bool tty_ready = false;
        int bcast_fd = -1;
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (bcast->isConnected() && fd == bcast->getFd()) {
                bcast_fd = fd;
            } else if (fd == linux_fd) {
                linux_ready = true;
            } else if (fd == tty_fd) {
                tty_ready = true;
            }
        }

        if (bcast_fd >= 0) {
            std::vector<uint8_t> msg = bcast->recvMessage();
            if (msg.size() > 0) {
                uint8_t dest_eid = msg[1];
                if ((dest_eid == 0xff) || (dest_eid == 0x00)) {
                    mctpSerial.send(msg);
                }
            }
        }

        if (linux_ready) {
            // All inbound traffic should be sent directly to the tty.
            std::vector<uint8_t> message;
            ssize_t len = linuxEndpoint.receive(message);
            mctpSerial.send(message);
        }

        if (tty_ready) {
            std::vector<uint8_t> msg = mctpSerial.receive();
            if (msg.size() > 0) {
                uint8_t dest_eid = msg[1];
                if ((dest_eid == 0xff) || (dest_eid == 0x00)) {
                    if (bcast->isConnected()) {
                        bcast->sendMessage(msg);
                    }
                } else {
                    linuxEndpoint.send(msg);
                }
            }
        }
        ::close(epfd);
    }
}

/**
 * @brief Returns the name of the bound mctpserial interface.
 * 
 * @return The mctpserial interface name as a string.
 */
std::string MctpBridge::getMctpIfName() const {
    return ifname;
}  

/**
 * @brief Returns the name of the broadcast interface.
 * 
 * @return The broadcast interface name as a string.
 */
std::string MctpBridge::getBroadcastName() const {
    return broadcastName;
}


