/**
 * @file linux_to_tty_example.cpp
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
 * - Sends an AF_MCTP datagram from a local socket throgh the bridge, to
 *   to a "remote" `iotorch::sermctp::MctpFramer` instance where the message
 *   is deframed and printed.
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
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
extern "C" {
    #include <unistd.h>
    #include <pty.h>
    #include <fcntl.h>
    #include <sys/socket.h>
    #include <linux/mctp.h>
}
#include "sermctp/MctpFramer.hpp"
#include "sermctp/MctpBridge.hpp"

/**
 * @brief Open an AF_MCTP socket and bind it to the specified local EID.
 * 
 * @param local_eid Local Endpoint ID to bind the socket to.
 * @return Socket file descriptor on success, -1 on failure.
 */
static int open_bind_mctp_socket(int local_eid) {
    int sock = socket(AF_MCTP, SOCK_DGRAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_mctp local{};
    memset(&local, 0, sizeof(local));
    local.smctp_family = AF_MCTP;
    local.smctp_network = 0;
    local.smctp_addr.s_addr = (uint8_t)local_eid;
    if (bind(sock, (struct sockaddr*)&local, sizeof(local)) < 0) { close(sock); return -1; }
    return sock;
}


/**
 * Main entry point
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
    // to local EID 8 (it could be bound to any EID on the system)
    // This socket represents a linux-side MCTP endpoint that will send messages to
    // the MCTP bridge's remotely connected endpoint (which has local EID 9 on the tty link)
    int tx = open_bind_mctp_socket(8);
    if (tx < 0) { perror("open_bind_mctp_socket"); bridge.close(); close(master_fd); return 1; }
    std::cerr << "Bound AF_MCTP socket to EID 8 fd=" << tx << "\n";

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

    // prepare a message to send from linux MCTP endpoint to the bridge's remote endpoint
    uint8_t payload[] = { 0x55, 0xAA };
    std::vector<uint8_t> msg(std::begin(payload), std::end(payload));
    struct sockaddr_mctp destaddr{};
    memset(&destaddr, 0, sizeof(destaddr));
    destaddr.smctp_family = AF_MCTP;            // mctp protocol
    destaddr.smctp_network = 0;                 // default network
    destaddr.smctp_addr.s_addr = (uint8_t)9;    // remote endpoint's EID
    destaddr.smctp_type = 0x01;                 // message type

    // Send the message from Linux MCTP endpoint to our "remote" endpoint connected via the tty link
    ssize_t s = sendto(tx, msg.data(), msg.size(), 0, (struct sockaddr*)&destaddr, sizeof(destaddr));
    if (s < 0) perror("sendto"); else std::cerr << "sendto sent " << s << " bytes\n";

    // Wait for framer to indicate a frame and read it
    // This code would be running on the remote MCTP endpoint side of the serial link,
    // not on the linux system.
    int fr_fd = framer.getFrameReadyFd();
    if (fr_fd >= 0) {
        fd_set rfds;
        struct timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
        FD_ZERO(&rfds); FD_SET(fr_fd, &rfds);
        int rv = select(fr_fd+1, &rfds, nullptr, nullptr, &tv);
        if (rv > 0 && FD_ISSET(fr_fd, &rfds)) {
            std::vector<uint8_t> recvd = framer.receive();
            if (!recvd.empty()) {
                std::cerr << "Framer received payload (" << recvd.size() << " bytes): ";
                for (auto b : recvd) fprintf(stderr, "%02x ", b);
                fprintf(stderr, "\n");
            } else {
                std::cerr << "Framer indicated ready but received no payload\n";
            }
        } else {
            std::cerr << "No frame received within timeout\n";
        }
    } else {
        std::cerr << "Framer has no ready fd\n";
    }

    close(tx);
    framer.close();
    bridge.close();
    close(master_fd);
    std::cerr << "Cleaning up...\n";
    return 0;
}
