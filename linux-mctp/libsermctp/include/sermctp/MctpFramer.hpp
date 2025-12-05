// Thin namespaced wrapper for the existing global `MctpFramer` API.
// This wrapper forwards calls to the implementation in the global namespace
// so users can migrate to `iotorch::sermctp::MctpFramer` without breaking
// the existing implementation immediately.
#pragma once
#include <memory>
#include <vector>
#include <cstdint>
#include <string>
#include "sermctp/detail/MctpFramer_impl.hpp"
#include "export.h"

namespace iotorch::sermctp {

class SERMCTP_API MctpFramer {
public:
    MctpFramer() : impl_(new ::MctpFramer()) {}
    ~MctpFramer() = default;

    int open(std::string dev_path, int baud_rate = B115200, bool hw_flow_control = false) {
        return impl_->open(std::move(dev_path), baud_rate, hw_flow_control);
    }
    int openFd(int fd) { return impl_->openFd(fd); }
    bool close() { return impl_->close(); }
    void send(std::vector<uint8_t> msg) { impl_->send(std::move(msg)); }
    std::vector<uint8_t> buildWire(const std::vector<uint8_t>& msg, uint16_t *out_fcs = nullptr) { return impl_->buildWire(msg, out_fcs); }
    std::vector<uint8_t> receive() { return impl_->receive(); }
    bool rxEmpty() { return impl_->rxEmpty(); }
    int getFrameReadyFd() const { return impl_->getFrameReadyFd(); }
    bool isFrameAvailable() const { return impl_->isFrameAvailable(); }
    int getSerialFd() const { return impl_->getSerialFd(); }
    void show_diagnostics(bool enable) { impl_->show_diagnostics(enable); }

private:
    std::unique_ptr<::MctpFramer> impl_;
};

} // namespace iotorch::sermctp
