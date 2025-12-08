/**
 * @file ManagedUsbTty.cpp
 * @brief Managed PTY that forwards between a persistent PTY and a USB serial tty.
 *
 * This internal implementation creates a PTY whose slave is returned to callers
 * while a background thread monitors udev for a physical USB tty matching a
 * requested persistent identifier (ID_PATH_TAG). When a matching device is
 * added the implementation opens and configures the physical tty and forwards
 * data bidirectionally between the PTY master and the physical device using
 * epoll for efficient multiplexing.
 *
 * The API is non-throwing and returns negative error codes defined in
 * `ManagedUsbTty.hpp`.
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

#include "sermctp/detail/ManagedUsbTty.hpp"
#include <unistd.h>
#include <pty.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <libudev.h>

using namespace sermctp::detail;

ManagedUsbTty::ManagedUsbTty()
    : running(false), pty_master_fd(-1), phys_fd(-1), baud(0), hwfc(false), udev_ctx(nullptr), udev_mon(nullptr), udev_fd(-1) {}

/**
 * @brief Destructor — stop forwarding and release resources.
 *
 * Calls `close()` to stop the background thread and close internal file
 * descriptors. The caller-owned slave fd returned by `open()` is not closed
 * by this destructor; the caller remains responsible for it.
 */
ManagedUsbTty::~ManagedUsbTty() {
    close();
}

/**
 * @brief Set a file descriptor to non-blocking mode.
 *
 * Returns 0 on success or -1 on failure and does not modify errno on
 * success. This helper is intentionally minimal and internal-only.
 *
 * @param fd File descriptor to set non-blocking.
 * @return 0 on success, -1 on failure.
 */
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
}

/**
 * @brief Add a file descriptor to an epoll instance for read/error events.
 *
 * Logs epoll_ctl failures to stderr for debugging; this helper keeps the
 * epoll interaction out of the main thread loop to keep that code clearer.
 *
 * @param epfd Epoll instance fd returned from epoll_create1().
 * @param fd   File descriptor to register with epoll.
 * @return void
 */
static void add_epoll_fd(int epfd, int fd) {
    if (epfd < 0 || fd < 0) return;
    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        std::perror("add_epoll_fd: epoll_ctl ADD");
    }
}

/**
 * @brief Apply termios settings to a serial device fd.
 *
 * Sets raw mode, disables software flow control (XON/XOFF), adjusts RTS/CTS
 * hardware flow control according to `rts_cts`, and applies the requested
 * baud rate. Returns MUSB_OK on success or a negative error code.
 *
 * @param fd The open file descriptor for the serial device.
 * @param baudrate Baud rate as an integer (mapped via cfset* calls).
 * @param rts_cts Enable hardware RTS/CTS flow control if true.
 * @return MUSB_OK on success or a negative ManagedUsbTtyErr code on failure.
 */
int ManagedUsbTty::configureSerial(int fd, int baudrate, bool rts_cts) noexcept {
    if (fd < 0) return MUSB_ERR_OPEN_TTY;
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) return MUSB_ERR_OPEN_TTY;

    // Set baud
    cfsetospeed(&tty, static_cast<speed_t>(baudrate));
    cfsetispeed(&tty, static_cast<speed_t>(baudrate));

    // Input flags - clear processing and disable software flow control
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    // Output flags - disable post-processing
    tty.c_oflag &= ~OPOST;
    // Control flags - set 8N1
    tty.c_cflag &= ~(CSIZE | PARENB);
    tty.c_cflag |= CS8;
    // Local flags - disable echo and canonical
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

    if (!rts_cts) {
        tty.c_cflag &= ~CRTSCTS;
    } else {
        tty.c_cflag |= CRTSCTS;
    }

    if (tcsetattr(fd, TCSANOW, &tty) != 0) return MUSB_ERR_OPEN_TTY;

    // set non-blocking for async IO
    set_nonblocking(fd);
    return MUSB_OK;
}

/**
 * @brief Check whether a udev device matches the configured serial id.
 *
 * Compares the device's `ID_PATH_TAG` property against the stored
 * `serialId` string.
 *
 * @param probe udev device to probe (may be a proxied probe device).
 * @return true if the device matches the requested `serialId`, false otherwise.
 */
bool ManagedUsbTty::matchesSerialId(struct ::udev_device *probe) const noexcept {
    if (!probe) return false;
    const char *id_path_tag = udev_device_get_property_value(probe, "ID_PATH_TAG");
    if (!id_path_tag) return false;
    return serialId == std::string(id_path_tag);
}

/**
 * @brief Create a PTY, start the monitor/forwarding thread, and return a slave fd.
 *
 * The returned value is a duplicated slave fd owned by the caller; the
 * ManagedUsbTty instance will keep the master fd for forwarding. On error a
 * negative ManagedUsbTtyErr code is returned.
 *
 * @param serial_id Persistent serial identifier to match (ID_PATH_TAG).
 * @param baudrate Baud rate to configure the physical tty when attached.
 * @param rts_cts Whether to enable RTS/CTS hardware flow control.
 * @return Duplicated slave fd (>=0) on success, or negative ManagedUsbTtyErr on failure.
 */
int ManagedUsbTty::open(const std::string &serial_id, int baudrate, bool rts_cts) noexcept {
    if (running.load()) return MUSB_ERR_THREAD;
    if (serial_id.empty()) return MUSB_ERR_INVALID_ARG;

    serialId = serial_id;
    baud = baudrate;
    hwfc = rts_cts;

    // Create pty
    int master_fd = -1;
    int slave_fd = -1;
    char slave_name[64]{};
    if (openpty(&master_fd, &slave_fd, slave_name, nullptr, nullptr) < 0) {
        return MUSB_ERR_PTY;
    }

    // Configure slave as raw, no echo
    struct termios tios{};
    if (tcgetattr(slave_fd, &tios) == 0) {
        cfmakeraw(&tios);
        tios.c_lflag &= ~(ECHO | ECHONL);
        tcsetattr(slave_fd, TCSANOW, &tios);
    }

    // Duplicate the slave fd to return to caller. Caller owns this dup and must close it.
    int slave_dup = dup(slave_fd);
    if (slave_dup < 0) {
        ::close(master_fd);
        ::close(slave_fd);
        return MUSB_ERR_PTY_DUP;
    }

    // Close the copy of the slave we created via openpty; caller has dup
    ::close(slave_fd);

    // Keep master fd for internal forwarding
    pty_master_fd = master_fd;
    set_nonblocking(pty_master_fd);

    // Start worker thread
    running.store(true);
    try {
        worker = std::thread(&ManagedUsbTty::threadMain, this);
    } catch (...) {
        running.store(false);
        ::close(pty_master_fd);
        pty_master_fd = -1;
        ::close(slave_dup);
        return MUSB_ERR_THREAD;
    }

    // Return slave dup to caller (ownership to caller)
    return slave_dup;
}

/**
 * @brief Stop forwarding and close all internal resources.
 *
 * This will stop the background thread and close the PTY master and any
 * opened physical tty. The duplicated slave fd returned to the caller by
 * `open()` remains the caller's responsibility to close.
 *
 * @return MUSB_OK on success or a negative ManagedUsbTtyErr on failure.
 */
int ManagedUsbTty::close() noexcept {
    if (!running.load() && pty_master_fd < 0 && phys_fd < 0) return MUSB_OK;

    running.store(false);
    if (worker.joinable()) worker.join();

    if (phys_fd >= 0) {
        ::close(phys_fd);
        phys_fd = -1;
    }
    if (pty_master_fd >= 0) {
        ::close(pty_master_fd);
        pty_master_fd = -1;
    }

    // udev cleanup (if any left)
    if (udev_mon) {
        udev_monitor_unref(udev_mon);
        udev_mon = nullptr;
    }
    if (udev_ctx) {
        udev_unref(udev_ctx);
        udev_ctx = nullptr;
    }
    udev_fd = -1;

    return MUSB_OK;
}

/**
 * @brief Query whether the forwarder thread is running and PTY master is present.
 *
 * @return true if the forwarder thread is active and PTY master is open.
 */
bool ManagedUsbTty::isOpen() const noexcept {
    return running.load();
}

// Forwarding & udev watcher main loop
/**
 * @brief Main worker: monitor udev, manage phys fd, and forward data.
 *
 * This background thread creates a udev monitor, registers the PTY master
 * and the udev monitor fd with epoll, and then loops forwarding data
 * bidirectionally between the PTY master and the physical tty when present.
 *
 * @note This function runs on a dedicated thread and does not return until
 *       `close()` signals shutdown via `running`.
 */
void ManagedUsbTty::threadMain() {
    // Create udev and monitor
    udev_ctx = udev_new();
    if (!udev_ctx) {
        std::cerr << "ManagedUsbTty: failed to create udev context\n";
        // continue without udev - reconnection won't happen
    } else {
        udev_mon = udev_monitor_new_from_netlink(udev_ctx, "udev");
        if (!udev_mon) {
            std::cerr << "ManagedUsbTty: failed to create udev monitor\n";
            udev_unref(udev_ctx);
            udev_ctx = nullptr;
        } else {
            udev_monitor_enable_receiving(udev_mon);
            udev_fd = udev_monitor_get_fd(udev_mon);
            set_nonblocking(udev_fd);
        }
    }

    const int MAX_EVENTS = 8;
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        std::perror("epoll_create1");
        return;
    }

    // Register pty master and udev fd (if present)
    add_epoll_fd(epfd, pty_master_fd);
    if (udev_fd >= 0) add_epoll_fd(epfd, udev_fd);

    while (running.load()) {
        struct epoll_event events[MAX_EVENTS];
        int timeout_ms = 500; // half-second to allow loop wakeups
        int n = epoll_wait(epfd, events, MAX_EVENTS, timeout_ms);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (udev_fd >= 0 && fd == udev_fd) {
                // handle udev events
                struct udev_device *dev = udev_monitor_receive_device(udev_mon);
                if (!dev) continue;
                const char *action = udev_device_get_action(dev);
                const char *subsystem = udev_device_get_subsystem(dev);
                if (!action || !subsystem) { udev_device_unref(dev); continue; }
                if (std::strcmp(subsystem, "tty") != 0) { udev_device_unref(dev); continue; }

                const char *devnode = udev_device_get_devnode(dev);
                const char *syspath = udev_device_get_syspath(dev);

                struct udev_device *probe = nullptr;
                if (syspath) probe = udev_device_new_from_syspath(udev_ctx, syspath);
                struct udev_device *use = probe ? probe : dev;

                bool matched = matchesSerialId(use);

                if (std::strcmp(action, "remove") == 0) {
                    if (matched && phys_fd >= 0) {
                        // Remove from epoll before closing fd so the epoll set stays consistent
                        if (epfd >= 0) epoll_ctl(epfd, EPOLL_CTL_DEL, phys_fd, nullptr);
                        ::close(phys_fd);
                        phys_fd = -1;
                    }
                } else if (std::strcmp(action, "add") == 0) {
                    if (matched && devnode) {
                        // open device
                        int ofd = ::open(devnode, O_RDWR | O_NOCTTY);
                        if (ofd >= 0) {
                            if (configureSerial(ofd, baud, hwfc) == MUSB_OK) {
                                phys_fd = ofd;
                                set_nonblocking(phys_fd);
                                add_epoll_fd(epfd, phys_fd);
                            } else {
                                ::close(ofd);
                            }
                        }
                    }
                }

                if (probe) udev_device_unref(probe);
                udev_device_unref(dev);
                continue;
            }

            if (fd == pty_master_fd) {
                // data from PTY -> physical tty (if present)
                char buf[4096];
                ssize_t r = ::read(pty_master_fd, buf, sizeof(buf));
                if (r > 0 && phys_fd >= 0) {
                    ssize_t written = 0;
                    while (written < r) {
                        ssize_t w = ::write(phys_fd, buf + written, r - written);
                        if (w < 0) {
                            if (errno == EAGAIN || errno == EINTR) continue;
                            // error writing - drop and close phys
                            if (epfd >= 0) epoll_ctl(epfd, EPOLL_CTL_DEL, phys_fd, nullptr);
                            ::close(phys_fd);
                            phys_fd = -1;
                            break;
                        }
                        written += w;
                    }
                }
                continue;
            }

            if (phys_fd >= 0 && fd == phys_fd) {
                // data from physical tty -> PTY
                char buf[4096];
                ssize_t r = ::read(phys_fd, buf, sizeof(buf));
                if (r > 0) {
                    ssize_t written = 0;
                    while (written < r) {
                        ssize_t w = ::write(pty_master_fd, buf + written, r - written);
                        if (w < 0) {
                            if (errno == EAGAIN || errno == EINTR) continue;
                            // error writing -> close phys
                            ::close(phys_fd);
                            phys_fd = -1;
                            break;
                        }
                        written += w;
                    }
                    } else if (r == 0) {
                    // EOF: close phys_fd
                    if (epfd >= 0) epoll_ctl(epfd, EPOLL_CTL_DEL, phys_fd, nullptr);
                    ::close(phys_fd);
                    phys_fd = -1;
                } else {
                    if (errno != EAGAIN && errno != EINTR) {
                        if (epfd >= 0) epoll_ctl(epfd, EPOLL_CTL_DEL, phys_fd, nullptr);
                        ::close(phys_fd);
                        phys_fd = -1;
                    }
                }
                continue;
            }
        }
    }

    ::close(epfd);
}
