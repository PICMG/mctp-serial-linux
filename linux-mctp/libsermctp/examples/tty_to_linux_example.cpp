/**
 * @file tty_to_linux_example.cpp
 * @brief Simulated MCTP-over-serial use case and example
 *
 * Simulated use case:
 * - Emulate an external serial MCTP endpoint connected to the Linux system.
 * - The Linux system bridges between the physical tty (pty) interface using
 *   the library's `MctpBridge` object.
 * - The remote device receives encoded frames over the wire and decodes
 *   them using the library's `MctpFramer` object.
 * - the Linux system and the remote endpoint serial link is simuiledd using
 *   a PTY pair.
 *
 * What this example does:
 * - Creates a PTY pair, opens `iotorch::sermctp::MctpBridge` on the slave,
 *   and attaches `iotorch::sermctp::MctpFramer` to the master.
 * - Sends an mctp framed and encoded message from a "remote" 
 *   `iotorch::sermctp::MctpFramer` instance through the bridge, to
 *   to a local AF_MCTP socket where the message received and printed.
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
#include <algorithm>
#include <chrono>
#include <thread>

#include "sermctp/MctpFramer.hpp"
#include "sermctp/MctpBridge.hpp"

extern "C" {
    #include <unistd.h>
    #include <stdlib.h>
    #include <sys/ioctl.h>
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <termios.h>
    #include <pty.h>
    #include <fcntl.h>
    #include <linux/tty.h>
    #include <linux/mctp.h>
}

/**
 * @brief Open an AF_MCTP socket and bind it to the specified local EID.
 * 
 * @param local_eid Local Endpoint ID to bind the socket to.
 * @param smctp_type SMCTP message type to bind to.
 * @param timeout_sec Receive timeout in seconds. If 0 or less, no timeout is set.
 * @return Socket file descriptor on success, -1 on failure.
 */
static int open_and_bind_listener(int local_eid, uint8_t smctp_type, int timeout_sec) {
    int sock = socket(AF_MCTP, SOCK_DGRAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_mctp local{};
    memset(&local, 0, sizeof(local));
    local.smctp_family = AF_MCTP;
    local.smctp_network = 0;
    local.smctp_addr.s_addr = (uint8_t)local_eid;
    local.smctp_type = smctp_type;
    if (bind(sock, (struct sockaddr*)&local, sizeof(local)) < 0) {
        close(sock);
        return -1;
    }
    if (timeout_sec > 0) {
        struct timeval tv{timeout_sec, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    return sock;
}

/**
 * @brief Main entry point
 */
int main() {
    if (geteuid() != 0) { std::cerr << "Must run as root\n"; return 2; }

    // Create a PTY pair
    // * The slave side of this virtual serial link represents the TTY device on 
    //   our linux system.
    // * The master side of this virtual serial link represents a serial connection
    //   on an MCTP-enabled serial endpoint that is external to our linux system.
    // * The serial link in an actual system would be a physical serial cable or
    //   other serial transport.
    int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd < 0) { perror("posix_openpt"); return 1; }
    if (grantpt(master_fd) < 0 || unlockpt(master_fd) < 0) { perror("grantpt/unlockpt"); close(master_fd); return 1; }
    char *slave_name_c = ptsname(master_fd);
    if (!slave_name_c) { perror("ptsname"); close(master_fd); return 1; }
    std::string slave_name(slave_name_c);
    int flags = fcntl(master_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

    // Instantiate and configure the bridge.  This provides the linux system
    // interface to the tty connection.  When opening the bridge, it will configure the
    // serial port, and set its line discipline to MCTP.  Once this is done, other endpoints
    // can send MCTP messages to the tty connected device through the linux MCTP interface.
    iotorch::sermctp::MctpBridge bridge;
    if (!bridge.open(slave_name, BaudRate::BR_115200, false, 8, std::vector<uint8_t>{9})) {
        std::cerr << "MctpBridge open failed for " << slave_name << "\n";
        close(master_fd);
        return 1;
    }
    std::cerr << "MctpBridge opened on " << slave_name << "\n";

    // Now that the linux mctp interface is up and active, open AF_MCTP socket and bind it
    // to local EID 8 as a listener (it could be bound to any EID on the system).
    // This socket represents a linux-side MCTP endpoint that will receive messages from
    // the MCTP bridge's remotely connected endpoint (which has local EID 9 on the tty link)
    int rx = open_and_bind_listener(8, 0x01, 20);
    if (rx < 0) { perror("open_and_bind_listener"); bridge.close(); close(master_fd); return 1; }
    std::cerr << "Bound AF_MCTP socket to EID 8 fd=" << rx << "\n";

    // Instantiate the framer and attach it to the master side of the pty link.  
    // In an actual system, the framer would reside on the external MCTP endpoint 
    // connected via the serial link.
    iotorch::sermctp::MctpFramer framer;
    if (framer.openFd(master_fd) < 0) {
        std::cerr << "Failed to attach framer to PTY master\n";
        bridge.close();
        close(master_fd);
        return 1;
    }

    // prepare a message to send from remote tty endpoint
    std::vector<uint8_t> message = { 0x01, 0x08, 0x09, 0xC8, 0x01, 0x55, 0xAA };

    // Send the message from "remote" endpoint to our Linux listener connected via the tty link
    framer.send(message);

    // Wait for Linux listener to receive a frame and read it
    // This code runs on the Linux system and receives the deframed MCTP datagram
    if (rx >= 0) {
        uint8_t buf[256];
        struct sockaddr_mctp peer{};
        socklen_t plen = sizeof(peer);
        ssize_t r = recvfrom(rx, buf, sizeof(buf), 0, (struct sockaddr*)&peer, &plen);
        if (r < 0) {
            perror("recvfrom");
            std::cerr << "No data received on AF_MCTP socket\n";
        } else {
            std::cerr << "Received " << r << " bytes on AF_MCTP:\n";
            for (ssize_t i=0;i<r;++i) fprintf(stderr, "%02x ", buf[i]);
            fprintf(stderr, "\n");
            if (plen >= sizeof(peer)) {
                std::cerr << "Remote sockaddr: family=" << peer.smctp_family
                          << " net=" << (int)peer.smctp_network
                          << " eid=" << (int)peer.smctp_addr.s_addr
                          << " type=" << (int)peer.smctp_type
                          << " tag=0x" << std::hex << (int)peer.smctp_tag << std::dec << "\n";
            }
        }
        close(rx);
    }
    framer.close();
    bridge.close();
    close(master_fd);
    std::cerr << "Cleaning up...\n";
    return 0;
}
