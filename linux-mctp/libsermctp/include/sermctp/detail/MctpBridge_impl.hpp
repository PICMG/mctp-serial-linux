/**
 * @file MctpBridge.hpp (implementation copy, moved to ser mctp/detail)
 *
 * This implementation provides the internal `MctpBridge` used by the
 * namespaced wrapper. It manages opening a serial endpoint, configuring the
 * TTY, attaching it to the framer, and running the dispatch loop that
 * forwards between the MCTP endpoint and the serial link.
 *
 * `open()` supports an optional `use_id_path_tag` boolean
 * parameter (default `false`). If set to `true`, the first string parameter
 * is interpreted as a `ID_PATH_TAG` udev identifier and the bridge will
 * instantiate a `ManagedUsbTty` internally. The bridge will then use the
 * duplicated PTY slave fd returned by the helper as the serial fd instead of
 * opening a physical device path directly.
 *
 * Note on ID_PATH_TAG
 * -------------------
 * The udev property `ID_PATH_TAG` identifies the physical path of a USB
 * device (the bus topology) and is used by this implementation to match a
 * stable identifier for the target serial device. To query `ID_PATH_TAG` on
 * the command line for a device node, run:
 *    udevadm info -q property -n /dev/ttyUSB0 | grep -E '^(ID_PATH_TAG)='
 */
#pragma once
#include <string>
#include <cstring>
#include <vector>
#include "sermctp/detail/LinuxMctpSerial_impl.hpp"
#include "sermctp/detail/MctpFramer_impl.hpp"
#include <memory>
#include "sermctp/export.h"
#include "sermctp/detail/ManagedUsbTty.hpp"
// forward-declare BcastMessenger so it doesn't leak into public headers
class BcastMessenger;
extern "C" {
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/ioctl.h>
    #include <linux/tty.h>
    #include <dirent.h>
    #include <termios.h>
}

enum class BaudRate : speed_t {
    BR_0        = B0,
    BR_50       = B50,
    BR_75       = B75,
    BR_110      = B110,
    BR_134      = B134,
    BR_150      = B150,
    BR_200      = B200,
    BR_300      = B300,
    BR_600      = B600,
    BR_1200     = B1200,
    BR_1800     = B1800,
    BR_2400     = B2400,
    BR_4800     = B4800,
    BR_9600     = B9600,
    BR_19200    = B19200,
    BR_38400    = B38400,
    BR_57600    = B57600,
    BR_115200   = B115200,
    BR_230400   = B230400
};

class SERMCTP_API MctpBridge {
public:
    MctpBridge();
    ~MctpBridge();
    bool open(const std::string& tty_path, BaudRate baud, bool hw_flow_control,
              uint8_t local_eid, std::vector<uint8_t> peer_eids,
              bool use_id_path_tag = false);
    std::string getMctpIfName() const;
    std::string getBroadcastName() const;
    void close();
private:
    LinuxMctpSerial linuxEndpoint;
    MctpFramer mctpSerial;
    std::unique_ptr<BcastMessenger> bcast;
    std::string ifname;
    std::string broadcastName;
    int tty_fd;
    int tty_raw_fd;
    int pty_master_fd;
    std::string tty_name;
    std::atomic<bool> running;
    std::thread dispatch_thread;
    std::unique_ptr<sermctp::detail::ManagedUsbTty> managedUsb;
    bool setup_bcast(std::string name);
    void run();
};
