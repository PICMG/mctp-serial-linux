/**
 * @file broadcast_to_tty_example.cpp
 * @brief Demonstrate sending a broadcast MCTP datagram from Linux to a remote
 *        serial endpoint via the MctpBridge broadcast interface.
 *
 * Simulated use case:
 * - A local Linux process wants to send a broadcast MCTP message onto the
 *   serial bus. Because Linux kernels do not support broadcast endpoint
 *   assignment, the `MctpBridge` exposes a user-space broadcast interface
 *   (a UNIX domain socket) that applications can send datagrams to. The
 *   bridge will transmit those datagrams as MCTP broadcasts on the serial
 *   link.
 *
 * What this example does (high level):
 * - Create a PTY pair to emulate the serial connection.
 * - Open `iotorch::sermctp::MctpBridge` on the PTY slave (linux side).
 * - Attach `iotorch::sermctp::MctpFramer` to the PTY master (remote side)
 *   to observe framed MCTP traffic that the bridge transmits.
 * - Create an AF_UNIX/SEQPACKET client socket and connect to the bridge's
 *   broadcast socket name (obtained from `MctpBridge::getBroadcastName`).
 * - Send a broadcast MCTP datagram via the broadcast socket and print the
 *   message received at the remote framer side.
 *
 * Notes:
 * - Run as root (sudo) because setting the MCTP line discipline and creating
 *   the kernel-facing MCTP interface requires privileges.
 * - The example is intentionally minimal and diagnostic-friendly.
 *
 *
 * Author: Doug Sandy <doug@picmg.org>
 * License: MIT No Attribution (MIT-0)
 *
 * Disclaimer: THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
 * KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 * NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

#include "sermctp/MctpFramer.hpp"
#include "sermctp/MctpBridge.hpp"

extern "C" {
    #include <unistd.h>
    #include <sys/socket.h>
    #include <sys/un.h>
    #include <pty.h>
    #include <termios.h>
    #include <fcntl.h>
}

static volatile sig_atomic_t g_stop = 0;

static void handle_sig(int) { g_stop = 1; }

// Connect a client AF_UNIX/SEQPACKET socket to the bridge broadcast socket
// name. Returns a connected fd on success or -1 on failure.
static int connect_broadcast_client(const std::string &sockpath) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sockpath.c_str(), sizeof(addr.sun_path)-1);

    // Retry loop while the bridge creates the socket file
    for (int i = 0; i < 40; ++i) {
        if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        if (errno == ENOENT) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        // other transient errors: short sleep and retry
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    close(fd);
    return -1;
}

int main() {
    if (geteuid() != 0) { std::cerr << "This program must be run as root. Exiting.\n"; return 2; }

    // Create a PTY pair
    // * The slave side of this virtual serial link represents the TTY device on
    //   our linux system.
    // * The master side of this virtual serial link represents a serial
    //   connection on an MCTP-enabled serial endpoint that is external to
    //   our linux system (the "remote").
    // * The serial link in an actual system would be a physical serial cable
    //   or an equivalent transport.
    int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd < 0) { perror("posix_openpt"); return 1; }
    if (grantpt(master_fd) < 0 || unlockpt(master_fd) < 0) { perror("grantpt/unlockpt"); close(master_fd); return 1; }
    char *slave_name_c = ptsname(master_fd);
    if (!slave_name_c) { perror("ptsname"); close(master_fd); return 1; }
    std::string slave_name(slave_name_c);
    int flags = fcntl(master_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

    // Instantiate and configure the bridge. This provides the linux system
    // interface to the tty connection. When opening the bridge it will
    // configure the serial port, set its line discipline to MCTP, and create
    // the kernel-facing `mctpserial` interface. Once this is done other
    // endpoints can send/receive MCTP messages via the bridge.
    iotorch::sermctp::MctpBridge bridge;
    if (!bridge.open(slave_name, BaudRate::BR_115200, false, 8, std::vector<uint8_t>{9})) {
        std::cerr << "MctpBridge open failed for " << slave_name << "\n";
        close(master_fd);
        return 1;
    }
    std::cerr << "MctpBridge opened on " << slave_name << "\n";

    // Attach framer to master (remote device) so we can observe broadcast frames
    iotorch::sermctp::MctpFramer framer;
    if (framer.openFd(master_fd) < 0) {
        std::cerr << "Failed to attach framer to PTY master\n";
        bridge.close(); close(master_fd); return 1;
    }

    // Connect to the bridge's broadcast interface (AF_UNIX/SEQPACKET).
    // The bridge exposes a unix socket whose name is returned by
    // `MctpBridge::getBroadcastName()`. Applications send broadcast
    // datagrams to that socket and the bridge will transmit them on the
    // serial link as MCTP broadcast messages.
    std::string bcast = bridge.getBroadcastName();
    std::cerr << "Connecting to broadcast socket: " << bcast << "\n";
    int bfd = connect_broadcast_client(bcast);
    if (bfd < 0) {
        std::cerr << "Failed to connect to bridge broadcast socket\n";
        framer.close(); bridge.close(); close(master_fd); return 1;
    }
    std::cerr << "Connected to broadcast socket fd=" << bfd << "\n";

    // Build a broadcast MCTP datagram (unframed).  Byte layout mirrors tests:
    // { msg_type, dest_eid, src_eid, <rest...> }
    // Use dest EID 0xff for broadcast and source EID 0x08 (the local bridge endpoint)
    std::vector<uint8_t> bmsg = { 0x01, 0xff, 0x08, 0xC8, 0x01, 0x55, 0xAA };

    // Send the broadcast datagram to the bridge's broadcast socket. The
    // bridge will transmit this as an MCTP broadcast on the serial link.
    ssize_t w = send(bfd, bmsg.data(), bmsg.size(), 0);
    if (w < 0) { perror("send(bcast)"); }
    else std::cerr << "Sent broadcast datagram (" << w << " bytes) to bridge\n";

    // Wait for the framer on the remote side to indicate a received frame
    // (the remote endpoint would receive the broadcast over the serial link)
    int fr_fd = framer.getFrameReadyFd();
    if (fr_fd >= 0) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(fr_fd, &rfds);
        struct timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
        int rv = select(fr_fd+1, &rfds, nullptr, nullptr, &tv);
        if (rv > 0 && FD_ISSET(fr_fd, &rfds)) {
            std::vector<uint8_t> recvd = framer.receive();
            if (!recvd.empty()) {
                std::cerr << "Framer received broadcast payload (" << recvd.size() << " bytes): ";
                for (auto b : recvd) fprintf(stderr, "%02x ", b);
                fprintf(stderr, "\n");
            } else {
                std::cerr << "Framer indicated ready but received no payload\n";
            }
        } else {
            std::cerr << "No framed broadcast received within timeout\n";
        }
    } else {
        std::cerr << "Framer has no ready fd\n";
    }

    // Cleanup and restore
    close(bfd);
    framer.close();
    bridge.close();
    close(master_fd);
    std::cerr << "Cleaning up...\n";
    return 0;
}
