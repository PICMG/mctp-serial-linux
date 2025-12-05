/**
 * @file MctpFramer.hpp
 * @brief Definitions for the MCTP serial framer and receive FSM.
 * 
 * This file implements the MctpFramer class which provides methods to
 * open and configure a serial port, send and receive MCTP frames, and
 * manage the internal receive state machine. The framer handles byte
 * stuffing, frame delimiting, and FCS checking for MCTP messages over
 * a serial (byte-stream) link.
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
#include <string>
#include <iostream>
#include <cstring>
#include <iomanip>
#include <limits.h>
extern "C" {
    #include <sys/epoll.h>
    #include <unistd.h>         // for read(), write(), close()
    #include <fcntl.h>          // for open(), O_RDWR, O_NONBLOCK
    #include <termios.h>        // for termios configuration
    #include <errno.h>          // for errno values like EAGAIN
    #include <unistd.h>
    #include <sys/socket.h>
    #include <poll.h>
    #include <sys/ioctl.h>
    #include <sys/stat.h>
}

/**
 * @brief Constructor for MctpFramer.
 */
MctpFramer::MctpFramer(): 
    rxState(MCTPSER_WAITING_FOR_SYNC), byte_count(0), running(false), 
    pipe_fds{-1, -1}, intr_pipe_fds{-1, -1}, 
    serial_fd(-1), diagnostics_enabled(false)
{
}

/**
 * @brief Destructor for MctpFramer.
 */
MctpFramer::~MctpFramer()
{
    close();
}

/**
 * @brief Opens and configures the serial port for MCTP framing.
 *
 * @param dev_path Path to the serial device (e.g., "/dev/ttyS0").
 * @param baud_rate Baud rate for the serial connection (default: B115200).
 * @param hw_flow_control Enable RTS/CTS hardware flow control if true (default: false).
 * @return File descriptor for waking up select on success, or -1 on error. 
 */
int MctpFramer::open(std::string dev_path, int baud_rate, bool hw_flow_control) {
    struct termios tty;
    if (serial_fd>0) return -1;

    if (pipe_fds[0]<0) {
        if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pipe_fds) == -1) {
            return -1;
        }
        int f0 = fcntl(pipe_fds[0], F_GETFL, 0);
        int f1 = fcntl(pipe_fds[1], F_GETFL, 0);
        if (f0 != -1) fcntl(pipe_fds[0], F_SETFL, f0 | O_NONBLOCK);
        if (f1 != -1) fcntl(pipe_fds[1], F_SETFL, f1 | O_NONBLOCK);
        fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);
    }
    if (intr_pipe_fds[0]<0) {
        if (pipe(intr_pipe_fds)==-1) {
            // non-fatal: close previously created pipe and fail
            ::close(pipe_fds[0]); ::close(pipe_fds[1]);
            pipe_fds[0] = pipe_fds[1] = -1;
            return -1;
        }
    }

    serial_fd = ::open(dev_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (serial_fd < 0) {
        return false;
    }

    if (tcgetattr(serial_fd, &tty) != 0) {
        ::close(serial_fd);
        serial_fd = -1;
        return -1;
    }

    cfsetospeed(&tty, baud_rate);
    cfsetispeed(&tty, baud_rate);

    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    tty.c_oflag &= ~OPOST;
    tty.c_cflag &= ~(CSIZE | PARENB);
    tty.c_cflag |= CS8;
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

    if (!hw_flow_control) {
        tty.c_cflag &= ~CRTSCTS;
    } else {
        tty.c_cflag |= CRTSCTS;
    }

    if (tcsetattr(serial_fd, TCSANOW, &tty) != 0) {
        ::close(serial_fd);
        serial_fd = -1;
        return -1;
    }

    // create a single dup of the external ready fd for callers
    if (ready_fd_dup_ < 0 && pipe_fds[0] >= 0) {
        ready_fd_dup_ = ::dup(pipe_fds[0]);
        if (ready_fd_dup_ >= 0) {
            int flags = fcntl(ready_fd_dup_, F_GETFL, 0);
            if (flags != -1) fcntl(ready_fd_dup_, F_SETFL, flags | O_NONBLOCK);
            fcntl(ready_fd_dup_, F_SETFD, FD_CLOEXEC);
        }
    }

    // create epoll instance and register intr pipe; serial fd will be added
    if (epoll_fd_ < 0) epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ >= 0 && intr_pipe_fds[0] >= 0) {
        struct epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = intr_pipe_fds[0];
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, intr_pipe_fds[0], &ev);
        if (serial_fd >= 0) {
            struct epoll_event ev2{}; ev2.events = EPOLLIN; ev2.data.fd = serial_fd;
            epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, serial_fd, &ev2);
        }
    }

    running = true;
    recv_thread = std::thread(&MctpFramer::run, this);
    return ready_fd_dup_ >= 0 ? ready_fd_dup_ : pipe_fds[0];
}

/**
 * @brief Opens the MCTP framer using an existing file descriptor.  This 
 * allows the caller to provide a pre-configured serial port file descriptor.
 *
 * @param fd File descriptor to use for the serial connection.
 * @return File descriptor for waking up select on success, or -1 on error.
 */
int MctpFramer::openFd(int fd) {
    if (serial_fd>0) return -1;
    if (fd<0) return -1;

    if (pipe_fds[0]<0) {
        if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pipe_fds) == -1) {
            return -1;
        }
        int f0 = fcntl(pipe_fds[0], F_GETFL, 0);
        int f1 = fcntl(pipe_fds[1], F_GETFL, 0);
        if (f0 != -1) fcntl(pipe_fds[0], F_SETFL, f0 | O_NONBLOCK);
        if (f1 != -1) fcntl(pipe_fds[1], F_SETFL, f1 | O_NONBLOCK);
        fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);
    }
    if (intr_pipe_fds[0]<0) {
        if (pipe(intr_pipe_fds)==-1) {
            ::close(pipe_fds[0]); ::close(pipe_fds[1]);
            pipe_fds[0] = pipe_fds[1] = -1;
            return -1;
        }
    }

    int dupfd = ::dup(fd);
    if (dupfd < 0) return -1;

    int flags = fcntl(dupfd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(dupfd, F_SETFL, flags | O_NONBLOCK);
    }

    serial_fd = dupfd;

    // create a single dup for external notifications if not already present
    if (ready_fd_dup_ < 0 && pipe_fds[0] >= 0) {
        ready_fd_dup_ = ::dup(pipe_fds[0]);
        if (ready_fd_dup_ >= 0) {
            int f = fcntl(ready_fd_dup_, F_GETFL, 0);
            if (f != -1) fcntl(ready_fd_dup_, F_SETFL, f | O_NONBLOCK);
            fcntl(ready_fd_dup_, F_SETFD, FD_CLOEXEC);
        }
    }

    // create epoll instance and register intr pipe and serial fd
    if (epoll_fd_ < 0) epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ >= 0) {
        if (intr_pipe_fds[0] >= 0) {
            struct epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = intr_pipe_fds[0];
            epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, intr_pipe_fds[0], &ev);
        }
        if (serial_fd >= 0) {
            struct epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = serial_fd;
            epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, serial_fd, &ev);
        }
    }

    running = true;
    recv_thread = std::thread(&MctpFramer::run, this);
    return ready_fd_dup_ >= 0 ? ready_fd_dup_ : pipe_fds[0];
}

/**
 * @brief Closes the MCTP framer and associated serial connection.
 *
 * Stops the receive thread, closes the serial file descriptor,
 * and cleans up internal resources.
 *
 * @return true on success, false on error. 
 */
bool MctpFramer::close() {
    if (diagnostics_enabled) std::cerr << "MctpFramer: close() called\n";
    bool wait_for_join = running;
    running = false;
    // Wake the recv thread immediately by writing to the internal pipe so
    // select() returns without waiting for its timeout. Ignore errors here.
    if (intr_pipe_fds[1] >= 0) {
        ssize_t _w = ::write(intr_pipe_fds[1], "x", 1);
        (void)_w;
    }
    if ((wait_for_join)&&(recv_thread.joinable())) {
        if (diagnostics_enabled) std::cerr << "MctpFramer: joining recv_thread\n";
        recv_thread.join();
        if (diagnostics_enabled) std::cerr << "MctpFramer: recv_thread joined\n";
    }

    if (serial_fd>=0) 
        ::close(serial_fd);
    serial_fd = -1;
    // remove and close epoll instance
    if (epoll_fd_ >= 0) {
        if (intr_pipe_fds[0] >= 0) epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, intr_pipe_fds[0], nullptr);
        if (serial_fd >= 0) epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, serial_fd, nullptr);
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
    // close the single dup returned to callers
    if (ready_fd_dup_ >= 0) { ::close(ready_fd_dup_); ready_fd_dup_ = -1; }
    // Close and clear pipes
    if (pipe_fds[0] >= 0) { ::close(pipe_fds[0]); pipe_fds[0] = -1; }
    if (pipe_fds[1] >= 0) { ::close(pipe_fds[1]); pipe_fds[1] = -1; }
    if (intr_pipe_fds[0] >= 0) { ::close(intr_pipe_fds[0]); intr_pipe_fds[0] = -1; }
    if (intr_pipe_fds[1] >= 0) { ::close(intr_pipe_fds[1]); intr_pipe_fds[1] = -1; }

    return true;
 }

 /**
  * @brief the thread associated with receiving data from the serial port
  * and updating the receive FSM.
  */
void MctpFramer::run() {
    const int max_events = 4;
    struct epoll_event events[max_events];
    while (running) {
        int n = epoll_wait(epoll_fd_ >= 0 ? epoll_fd_ : -1, events, max_events, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) continue; // timeout

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == intr_pipe_fds[0]) {
                uint8_t buf[64];
                ssize_t rr = ::read(intr_pipe_fds[0], buf, sizeof(buf));
                (void)rr;
                if (!running) break;
            } else if (fd == serial_fd) {
                updateRxFSM();
            }
        }
    }
}

/**
 * @brief Returns the internal frame-ready file descriptor.
 *
 * This file descriptor can be used by external code (or a signal handler)
 * to wait for frame completion notifications.
 *
 * @return int File descriptor for frame-ready notifications.
 */
int MctpFramer::getFrameReadyFd() const {
    return ready_fd_dup_ >= 0 ? ready_fd_dup_ : pipe_fds[0];
}

/**
 * @brief Calculate the Frame Check Sequence (FCS) for a data buffer.
 *
 * @param fcs Initial FCS value.
 * @param cp Pointer to the data buffer.
 * @param len Length of the data buffer in bytes.
 * @return Calculated FCS value.
 */
uint16_t MctpFramer::calcFcs(uint16_t fcs, uint8_t *cp, int len)
{
    for(int i = 0; i<len; i++)
        fcs = 0x0ffff&((fcs >> 8) ^ fcstab[(fcs ^ (((int)cp[i])&0x0ff)) & 0xff]);
    return (fcs);
}

/** 
 * @brief Write all data to a file descriptor with timeout handling.
 * 
 * This function attempts to write the entire buffer to the specified file descriptor.
 * If the write would block, it waits for the file descriptor to become writable
 * using poll() with the specified timeout.
 * 
 * @param fd File descriptor to write to.
 * @param buf Buffer containing data to write.
 * @param len Length of the buffer in bytes.
 * @param timeout_ms Timeout in milliseconds to wait for writability.
 * @return ssize_t Number of bytes written on success, or -1 on error.
*/
static ssize_t framer_write_all(int fd, const uint8_t* buf, size_t len, int timeout_ms=1000) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::write(fd, buf + sent, len - sent);
        if (n > 0) { sent += (size_t)n; continue; }
        if (n == 0) { errno = EIO; return -1; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd pfd{fd, POLLOUT, 0};
            int rv = poll(&pfd, 1, timeout_ms);
            if (rv > 0 && (pfd.revents & POLLOUT)) continue;
            return -1;
        }
        return -1;
    }
    return (ssize_t)sent;
}

/**
 * @brief Transmit a framed MCTP message over the serial port.
 *
 * This function applies byte-stuffing (escaping) to the provided message
 * and writes the resulting frame to the serial file descriptor.
 *
 * @param msg Vector of bytes representing the framed MCTP message.
 */
void MctpFramer::transmitFrame(std::vector<uint8_t> msg) {
    // Build escaped frame into an output buffer then write it atomically with retries
    std::vector<uint8_t> out;
    out.reserve(msg.size() * 2 + 4);
    for (size_t i=0;i<msg.size();i++) {
        uint8_t data = msg[i];
        if (diagnostics_enabled) {
            if (i==0) std::cerr<<"MctpFramer: transmitFrame data: ";
            std::cerr<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)data<<" "<<std::dec;
            if (i==msg.size()-1) std::cerr<<"\n"<<std::flush;
        }
        if ((i<3)||(i>msg.size()-4)) {
            // skip framing of anything but the payload bytes
            out.push_back(data);
        } else if ((data == FRAME_CHAR) || (data == ESCAPE_CHAR)) {
            out.push_back(ESCAPE_CHAR);
            out.push_back((uint8_t)(data - 0x20));
        } else {
            out.push_back(data);
        }
    }
    // Attempt to write the entire framed packet with timeout/retries
    if (diagnostics_enabled) {
        std::cerr << "MctpFramer: transmitFrame framed bytes len=" << out.size() << ": ";
        for (size_t i=0;i<out.size();++i) std::cerr << std::hex << (int)out[i] << " ";
        std::cerr << std::dec << "\n";
    }
    ssize_t wrote = framer_write_all(serial_fd, out.data(), out.size(), 2000);
    if (wrote < 0) {
        int err = errno;
        if (diagnostics_enabled) {
            std::cerr << "MctpFramer: transmitFrame write failed fd=" << serial_fd
                      << " attempted=" << out.size()
                      << " errno=" << err << " (" << strerror(err) << ")\n";
        }
    } else {
        if (diagnostics_enabled) {
            std::cerr << "MctpFramer: wrote " << wrote << " bytes to fd=" << serial_fd << "\n";
        }
        // Instrumentation: check TIOCOUTQ to see how many bytes remain in the
        // PTY output queue immediately after write and shortly after to
        // confirm the kernel/line-discipline has consumed them.
        if (diagnostics_enabled) {
            int outq = -1;
            if (ioctl(serial_fd, TIOCOUTQ, &outq) == 0) {
                std::cerr << "MctpFramer: TIOCOUTQ immediate=" << outq << "\n";
                // short sleep then re-check
                usleep(5000);
                int outq2 = -1;
                if (ioctl(serial_fd, TIOCOUTQ, &outq2) == 0) {
                    std::cerr << "MctpFramer: TIOCOUTQ after 5ms=" << outq2 << "\n";
                }
            } else {
                if (diagnostics_enabled) std::cerr << "MctpFramer: TIOCOUTQ ioctl failed: " << strerror(errno) << "\n";
            }
        }
    }
}

/**
 * @brief Build the on-wire framed/escaped bytes for the given payload message
 *        without transmitting.
 *
 * If out_fcs is non-null it will be set to the computed FCS value for the
 * message (before escaping).
 *
 * @param msg Vector of bytes representing the MCTP payload message.
 * @param out_fcs Pointer to store the computed FCS value (optional).
 * @return Vector of bytes representing the on-wire framed and escaped message.
 */
std::vector<uint8_t> MctpFramer::buildWire(const std::vector<uint8_t> &msg, uint16_t *out_fcs) {
    std::vector<uint8_t> full_msg;
    full_msg.reserve(msg.size() + 4);
    // Construct the same full_msg as send() does: FRAME_CHAR, 0x01, len, payload...
    full_msg.push_back(FRAME_CHAR);
    full_msg.push_back(0x01);
    full_msg.push_back((uint8_t)msg.size());
    full_msg.insert(full_msg.end(), msg.begin(), msg.end());
    uint16_t fcs = calcFcs(INITFCS, full_msg.data()+1, full_msg.size()-1);
    full_msg.push_back((uint8_t)(fcs>>8));
    full_msg.push_back((uint8_t)(fcs&0xff));
    full_msg.push_back(FRAME_CHAR);
    if (out_fcs) *out_fcs = fcs;

    // Now escape interior bytes the same way transmitFrame() does
    std::vector<uint8_t> out;
    out.reserve(full_msg.size() * 2 + 4);
    for (size_t i=0;i<full_msg.size();i++) {
        uint8_t data = full_msg[i];
        if ((i<3)||(i>full_msg.size()-4)) {
            out.push_back(data);
        } else if ((data == FRAME_CHAR) || (data == ESCAPE_CHAR)) {
            out.push_back(ESCAPE_CHAR);
            out.push_back((uint8_t)(data - 0x20));
        } else {
            out.push_back(data);
        }
    }
    return out;
}

/**
 * @brief Send the specified MCTP message to the serial port.
 *
 * This function constructs the full framed message, computes the FCS,
 * applies byte-stuffing (escaping), and transmits it over the serial port.
 *
 * @param msg Vector of bytes representing the MCTP payload message.
 */
void MctpFramer::send(std::vector<uint8_t> msg){
    if (diagnostics_enabled) std::cerr << "MctpFramer: send() called payload_len=" << msg.size() << "\n";
    std::vector<uint8_t> full_msg = {FRAME_CHAR, 0x01, (uint8_t)msg.size()};
    full_msg.insert(full_msg.end(), msg.begin(), msg.end());
    uint16_t fcs = calcFcs(INITFCS, full_msg.data()+1, full_msg.size()-1);
    full_msg.push_back(fcs>>8);
    full_msg.push_back(fcs&0xff);
    full_msg.push_back(FRAME_CHAR);
    transmitFrame(full_msg);
}

/**
 * @brief Check if there are any received frames available.
 */
bool MctpFramer::rxEmpty() {
    if (pipe_fds[0] < 0) return true;
    struct pollfd pfd;
    pfd.fd = pipe_fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    int rv = poll(&pfd, 1, 0);
    return !(rv > 0 && (pfd.revents & POLLIN));
}

/**
 * @brief Validate the currently received frame in rx_in_progress.
 *
 * This function checks the length and FCS of the received frame to ensure
 * it is valid according to MCTP framing rules.
 *
 * @return true if the received frame is valid, false otherwise.
 */
bool MctpFramer::validateRx() {
    if (rx_in_progress.size()<11) return false;
    byte_count = rx_in_progress[2];
    if ((uint16_t)byte_count != (uint16_t)rx_in_progress.size()-6) return false;
    uint16_t fcs = calcFcs(INITFCS, rx_in_progress.data()+1, rx_in_progress.size()-4);
    uint16_t msg_fcs = rx_in_progress[rx_in_progress.size()-3];
    msg_fcs = msg_fcs<<8;
    msg_fcs += rx_in_progress[rx_in_progress.size()-2];
    if (msg_fcs!=fcs) return false;
    return true;
}

/**
 * @brief Update the receive FSM with data read from the serial port.
 *
 * This function reads bytes from the serial file descriptor and updates
 * the internal receive state machine to process incoming MCTP frames.
 */
void MctpFramer::updateRxFSM() {
    uint8_t by;
    ssize_t result = read(serial_fd, &by, 1);
    if (result == 1) {
    if (diagnostics_enabled) {
        std::cerr << "MctpFramer: read byte 0x" << std::hex << (int)by << std::dec << " state=" << rxState << "\n"<<std::flush;
    }
        switch (rxState) {
            case MCTPSER_WAITING_FOR_SYNC:
                if (by == FRAME_CHAR) {
                    rxState = MCTPSER_HEADER1;
                    rx_in_progress.clear();
                    rx_in_progress.push_back(FRAME_CHAR);
                }
                break;
            case MCTPSER_HEADER1:
                // this should have the protocol version byte.  Just push it to the back
                rx_in_progress.push_back(by);
                rxState = MCTPSER_HEADER2;
                break;
            case MCTPSER_HEADER2:
                // this should have the length byte.  Push it to the back
                rx_in_progress.push_back(by);
                byte_count = by;
                rxState = MCTPSER_BODY;
                break;
            case MCTPSER_BODY:
                if (by == ESCAPE_CHAR) {
                    // the next byte is escaped and needs to be unescaped
                    rxState = MCTPSER_ESCAPE;
                    break;
                } else if (by == FRAME_CHAR) {
                    // unexpected FRAME_CHAR - restart frame
                    rxState = MCTPSER_HEADER1;
                    rx_in_progress.clear();
                    rx_in_progress.push_back(FRAME_CHAR);
                    break;
                } else {
                    rx_in_progress.push_back(by);
                    byte_count--; 
                    if (byte_count == 0) {
                        rxState = MCTPSER_FCS1;
                    }
                }
                break;
            case MCTPSER_FCS1:
                rx_in_progress.push_back(by);
                rxState = MCTPSER_FCS2;
                break;
            case MCTPSER_FCS2:
                rx_in_progress.push_back(by);
                rx_in_progress.push_back(FRAME_CHAR);
                rxState = MCTPSER_END;
                break;
            case MCTPSER_END:
                if (by != FRAME_CHAR) {
                    // invalid end of frame - drop it
                    rxState = MCTPSER_WAITING_FOR_SYNC;
                    break;
                }
                // complete frame received - validate it
                if (validateRx()) {
                    rx_in_progress.erase(rx_in_progress.begin(), rx_in_progress.begin() + 3);
                    rx_in_progress.erase(rx_in_progress.end()-3, rx_in_progress.end());
                    std::vector<uint8_t> packet = rx_in_progress;
                    if (diagnostics_enabled) {
                        std::cerr << "MctpFramer: validated frame len=" << packet.size() << "\n"<<std::flush;
                    }
                    // If the socket buffer is full (EAGAIN) we drop the packet and optionally log.
                    if (packet.size() <5) {
                        std::cerr << "MctpFramer: is zero, dropping packet\n";
                        rxState = MCTPSER_WAITING_FOR_SYNC;
                        break;
                    }
                    ssize_t _w = ::send(pipe_fds[1], packet.data(), packet.size(), MSG_NOSIGNAL);
                    if (_w != (ssize_t)packet.size()) {
                        if (_w < 0) {
                            if (diagnostics_enabled) std::cerr << "MctpFramer: send failed: " << strerror(errno) << "\n";
                        } else {
                            if (diagnostics_enabled) std::cerr << "MctpFramer: partial send: " << _w << " of " << packet.size() << "\n";
                        }
                    }
                    rxState = MCTPSER_WAITING_FOR_SYNC;
                } else {    
                    if (diagnostics_enabled) {
                        std::cerr << "MctpFramer: invalid frame received len=" << rx_in_progress.size() << "\n"<<std::flush;
                    }
                    rxState = MCTPSER_WAITING_FOR_SYNC;
                }
                break;
            case MCTPSER_ESCAPE:
                if ((by==(ESCAPE_CHAR-0x20))||(by==(FRAME_CHAR-0x20))) {
                    by = (uint8_t)(by+0x20);
                    rx_in_progress.push_back(by);
                    byte_count--;
                    if (byte_count == 0) {
                        rxState = MCTPSER_FCS1;
                    } else { 
                        rxState = MCTPSER_BODY;
                    }
                    break;
                } else if (by == FRAME_CHAR) {
                    // UNEXPECTED FRAME_CHAR - restart frame
                    rx_in_progress.clear();
                    rx_in_progress.push_back(FRAME_CHAR);
                    rxState = MCTPSER_HEADER1;
                } else {
                    // invalid escape sequence - drop frame
                    rxState = MCTPSER_WAITING_FOR_SYNC;
                }
                break;
        }
    }
}

/**
 * @brief Receive a framed MCTP message from the internal pipe.
 *
 * This function reads a complete MCTP message from the internal
 * socketpair used for frame notifications.
 *
 * @return Vector of bytes representing the received MCTP message.
 */
std::vector<uint8_t> MctpFramer::receive() {
    std::vector<uint8_t> empty;
    if (pipe_fds[0] < 0) return empty;
    const size_t MAX_PKT = 4096;
    std::vector<uint8_t> buf(MAX_PKT);
    ssize_t r = recv(pipe_fds[0], buf.data(), buf.size(), 0);
    if (r <= 0) return empty;
    buf.resize(r);
    if (diagnostics_enabled)
        std::cerr<<"MctpFramer: receive() got message len="<<buf.size()<<"\n"<<std::flush;
    return buf;
}

/**
 * @brief Checks if there is a frame available to read.
 *
 * @return true if a frame is available, false otherwise.
 */
bool MctpFramer::isFrameAvailable() const {
    if (pipe_fds[0] < 0) return false;
    struct pollfd pfd;
    pfd.fd = pipe_fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    int rv = poll(&pfd, 1, 0);
    return (rv > 0 && (pfd.revents & POLLIN));
}

/**
 * @brief Enable or disable diagnostic output.
 *
 * @param enable True to enable diagnostics, false to disable.
 */
void MctpFramer::show_diagnostics(bool enable) {
    diagnostics_enabled = enable;
}
