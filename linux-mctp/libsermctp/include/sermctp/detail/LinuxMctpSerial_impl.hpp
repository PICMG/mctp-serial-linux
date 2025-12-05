/**
 * @file LinuxMctpSerial.hpp (implementation copy, moved to ser mctp/detail)
 */
#pragma once
#include <string>
#include <cstring>
#include <vector>
#include <cstdint>
#include "sermctp/detail/MctpFramer_impl.hpp"
#include <thread>
#include <atomic>
extern "C" {
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/ioctl.h>
    #include <linux/tty.h>
    #include <dirent.h>
}

class LinuxMctpSerial {
public:
    LinuxMctpSerial();
    ~LinuxMctpSerial();
    std::string initialize(const std::string& mctp_if_name, uint8_t local_eid, std::vector<uint8_t> peer_eids);
    void close();
    std::string getMctpIfName() const;
    int getLocalEid() const;
    int getIsRxReadyFd() const;
    void send(const std::vector<uint8_t>& msg);
    ssize_t receive(std::vector<uint8_t>& result);
    bool isPacketAvailable() const;
    void show_diagnostics(bool enable);
private:
    std::string mctp_if_name;
    uint8_t local_eid = 0;
    MctpFramer framer;
    int rx_fd = -1;
    int pty_master_fd = -1;
    int pty_slave_fd = -1;
    std::string pty_slave_name;
    std::vector<std::string> list_mctp_devices();
};
