/**
 * @file broadcast_from_tty_example.cpp
 * @brief Demonstrate sending a broadcast MCTP datagram from the remote PTY
 *        endpoint to the linux bridge; the bridge exposes the message on its
 *        broadcast interface (AF_UNIX socket).
 *
 * Simulated use case:
 * - An MCTP-capable device on the serial bus transmits a broadcast message.
 * - The Linux `MctpBridge` receives the framed bytes on the serial link and
 *   forwards the broadcast datagram to a user-space broadcast socket.
 *
 * What this example does:
 * - Create a PTY pair (master=remote, slave=linux) and open `MctpBridge`
 *   on the slave.
 * - Attach `MctpFramer` to the PTY master (remote) to transmit a framed
 *   broadcast message over the serial link.
 * - Connect a client to the bridge's broadcast socket and receive the
 *   datagram sent by the bridge when it observes the broadcast on serial.
 *
 * Notes: run as root (sudo). The example is verbose to aid debugging.
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
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    close(fd);
    return -1;
}

int main() {
    if (geteuid() != 0) { std::cerr << "This program must be run as root. Exiting.\n"; return 2; }

    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    // Create a PTY pair
    // * The slave side of this virtual serial link represents the TTY device on
    //   the Linux system.
    // * The master side represents a remote MCTP-over-serial endpoint which
    //   transmits frames onto the wire.
    // * We use a PTY pair so this example is hermetic and doesn't require
    //   physical serial hardware.
    int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd < 0) { perror("posix_openpt"); return 1; }
    if (grantpt(master_fd) < 0 || unlockpt(master_fd) < 0) { perror("grantpt/unlockpt"); close(master_fd); return 1; }
    char *slave_name_c = ptsname(master_fd);
    if (!slave_name_c) { perror("ptsname"); close(master_fd); return 1; }
    std::string slave_name(slave_name_c);
    int flags = fcntl(master_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

    // Instantiate and configure the bridge. This provides the Linux-side
    // interface to the TTY connection. When opened the bridge will configure
    // the serial port, set its line discipline to MCTP, and create the
    // kernel-facing `mctpserial` interface so the kernel can route datagrams.
    iotorch::sermctp::MctpBridge bridge;
    if (!bridge.open(slave_name, BaudRate::BR_115200, false, 8, std::vector<uint8_t>{9})) {
        std::cerr << "MctpBridge open failed for " << slave_name << "\n";
        close(master_fd);
        return 1;
    }
    std::cerr << "MctpBridge opened on " << slave_name << "\n";

    // Connect to the bridge's broadcast interface (AF_UNIX/SEQPACKET).
    // The bridge exposes a unix socket whose name is returned by
    // `MctpBridge::getBroadcastName()`. Applications connect to that socket
    // and send/receive broadcast datagrams via the bridge.
    std::string bcast = bridge.getBroadcastName();
    int bfd = connect_broadcast_client(bcast);
    if (bfd < 0) {
        std::cerr << "Failed to connect to bridge broadcast socket\n";
        bridge.close(); close(master_fd); return 1;
    }
    std::cerr << "Connected to broadcast socket fd=" << bfd << "\n";

    // Attach the project's serial framer to the PTY master. In a real
    // remote endpoint the framer would run on the device; here the framer
    // produces correctly framed MCTP serial bytes (start/stop, escaping,
    // FCS) so the bridge can parse and forward the datagram.
    iotorch::sermctp::MctpFramer framer;
    if (framer.openFd(master_fd) < 0) {
        std::cerr << "Failed to attach framer to PTY master\n";
        close(bfd); bridge.close(); close(master_fd); return 1;
    }

    // Build a broadcast MCTP message (payload): { msg_type, dest_eid, src_eid, ... }
    std::vector<uint8_t> payload = { 0x01, 0xff, 0x08, 0xC8, 0x01, 0x55, 0xAA };

    // Send the framed broadcast from the remote endpoint into the serial link.
    // The bridge should observe the framed bytes, decode the MCTP datagram,
    // and forward the datagram to its broadcast socket where our connected
    // client will receive it.
    framer.send(payload);
    std::cerr << "Remote framer sent broadcast payload (" << payload.size() << " bytes)\n";

    // Wait for a message on the broadcast socket (bridge should forward it)
    fd_set rfds; FD_ZERO(&rfds); FD_SET(bfd, &rfds);
    struct timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
    int rv = select(bfd+1, &rfds, nullptr, nullptr, &tv);
    if (rv > 0 && FD_ISSET(bfd, &rfds)) {
        uint8_t buf[512];
        ssize_t r = recv(bfd, buf, sizeof(buf), 0);
        if (r < 0) {
            perror("recv(bcast)");
        } else {
            std::cerr << "Broadcast listener received " << r << " bytes: ";
            for (ssize_t i=0;i<r;++i) fprintf(stderr, "%02x ", buf[i]);
            fprintf(stderr, "\n");
        }
    } else {
        std::cerr << "No broadcast received within timeout\n";
    }

    // Cleanup
    close(bfd);
    framer.close();
    bridge.close();
    close(master_fd);
    std::cerr << "Cleaning up...\n";
    return 0;
}
