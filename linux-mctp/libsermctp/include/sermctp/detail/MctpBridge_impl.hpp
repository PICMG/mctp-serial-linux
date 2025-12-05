/**
 * @file MctpBridge.hpp (implementation copy, moved to ser mctp/detail)
 */
#pragma once
#include <string>
#include <cstring>
#include <vector>
#include "sermctp/detail/LinuxMctpSerial_impl.hpp"
#include "sermctp/detail/MctpFramer_impl.hpp"
#include <memory>
#include "sermctp/export.h"
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
              uint8_t local_eid, std::vector<uint8_t> peer_eids);
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
    bool setup_bcast(std::string name);
    void run();
};
