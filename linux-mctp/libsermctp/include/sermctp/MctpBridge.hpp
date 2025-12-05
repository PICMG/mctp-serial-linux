// Thin namespaced wrapper for the existing global `MctpBridge` class.
// This wrapper forwards calls to the implementation in the global namespace
// so users can migrate to `iotorch::sermctp::MctpBridge` gradually.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "export.h"
#include "sermctp/detail/MctpBridge_impl.hpp"

namespace iotorch::sermctp {

class SERMCTP_API MctpBridge {
public:
    MctpBridge() : impl_(new ::MctpBridge()) {}
    ~MctpBridge() = default;

    bool open(const std::string& tty_path, BaudRate baud, bool hw_flow_control,
              uint8_t local_eid, std::vector<uint8_t> peer_eids) {
        return impl_->open(tty_path, baud, hw_flow_control, local_eid, std::move(peer_eids));
    }

    std::string getMctpIfName() const { return impl_->getMctpIfName(); }
    std::string getBroadcastName() const { return impl_->getBroadcastName(); }

    void close() { impl_->close(); }

private:
    std::unique_ptr<::MctpBridge> impl_;
};

} // namespace iotorch::sermctp
