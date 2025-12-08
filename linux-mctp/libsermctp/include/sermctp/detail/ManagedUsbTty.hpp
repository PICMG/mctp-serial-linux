/**
 * @file ManagedUsbTty.hpp
 * @brief Internal header for ManagedUsbTty helper.
 *
 * Internal header (not installed): helper to manage a persistent PTY and
 * forward data to/from a physical USB serial device discovered via udev.
 *
 * The `ManagedUsbTty` class provides a non-throwing open/close API. Calling
 * `open()` creates a PTY and returns a duplicated slave fd which the caller
 * owns and must close. The implementation runs a background thread which
 * monitors udev for a physical tty with the requested `ID_PATH_TAG` and
 * forwards data bidirectionally between the PTY master and the physical
 * device when it is present. Calls return negative `ManagedUsbTtyErr` codes
 * on failure.
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
#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <libudev.h>

namespace sermctp::detail {

enum ManagedUsbTtyErr {
    MUSB_OK = 0,
    MUSB_ERR_PTY = -1,
    MUSB_ERR_PTY_DUP = -2,
    MUSB_ERR_THREAD = -3,
    MUSB_ERR_UDEV = -4,
    MUSB_ERR_INVALID_ARG = -5,
    MUSB_ERR_OPEN_TTY = -6,
};

/**
 * @brief Internal helper that exposes an open/close API for a managed PTY.
 *
 * The class creates a PTY slave returned to the caller and runs a background
 * thread that monitors udev and forwards data between the PTY master and a
 * matching physical USB tty when it appears.
 */
class ManagedUsbTty {
public:
    ManagedUsbTty();
    ~ManagedUsbTty();

    // Create the persistent PTY and start the monitor/forwarder thread.
    // Returns: duplicated slave fd >= 0 on success, negative ManagedUsbTtyErr on failure.
    int open(const std::string &serial_id, int baudrate, bool rts_cts) noexcept;

    // Close the forwarder and release resources. Returns 0 on success or negative error code.
    int close() noexcept;

    // True if forwarder thread is running and PTY master present
    bool isOpen() const noexcept;

    // Non-copyable
    ManagedUsbTty(const ManagedUsbTty &) = delete;
    ManagedUsbTty &operator=(const ManagedUsbTty &) = delete;

private:
    void threadMain();
    int configureSerial(int fd, int baudrate, bool rts_cts) noexcept;
    bool matchesSerialId(struct ::udev_device *probe) const noexcept;

    std::thread worker;
    std::atomic<bool> running;

    // PTY master fd used for forwarding
    int pty_master_fd;

    // current physical TTY fd (-1 if not present)
    int phys_fd;

    // store requested params
    std::string serialId;
    int baud;
    bool hwfc;

    // udev objects are created in thread; keep pointers here for cleanup
    struct ::udev *udev_ctx;
    struct ::udev_monitor *udev_mon;
    int udev_fd;
};

} // namespace sermctp::detail
