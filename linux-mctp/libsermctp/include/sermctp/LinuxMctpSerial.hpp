// Thin namespaced wrapper for the existing global `LinuxMctpSerial` API.
// For migration, this forwards calls to the existing implementation.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include "sermctp/detail/LinuxMctpSerial_impl.hpp"
#include "export.h"

namespace iotorch::sermctp {

class SERMCTP_API LinuxMctpSerial {
public:
    LinuxMctpSerial() : impl_(new ::LinuxMctpSerial()) {}
    ~LinuxMctpSerial() = default;

    std::string initialize(const std::string& mctp_if_name, uint8_t local_eid, std::vector<uint8_t> peer_eids) {
        return impl_->initialize(mctp_if_name, local_eid, std::move(peer_eids));
    }
    void close() { impl_->close(); }
    std::string getMctpIfName() const { return impl_->getMctpIfName(); }
    int getLocalEid() const { return impl_->getLocalEid(); }
    int getIsRxReadyFd() const { return impl_->getIsRxReadyFd(); }
    void send(const std::vector<uint8_t>& msg) { impl_->send(msg); }
    ssize_t receive(std::vector<uint8_t>& result) { return impl_->receive(result); }
    bool isPacketAvailable() const { return impl_->isPacketAvailable(); }
    void show_diagnostics(bool enable) { impl_->show_diagnostics(enable); }

private:
    std::unique_ptr<::LinuxMctpSerial> impl_;
};

} // namespace iotorch::sermctp
