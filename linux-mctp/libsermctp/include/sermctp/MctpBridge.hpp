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
              uint8_t local_eid, std::vector<uint8_t> peer_eids, bool use_id_path_tag = false) {
        return impl_->open(tty_path, baud, hw_flow_control, local_eid, std::move(peer_eids), use_id_path_tag);
    }

    std::string getMctpIfName() const { return impl_->getMctpIfName(); }
    std::string getBroadcastName() const { return impl_->getBroadcastName(); }

    void close() { impl_->close(); }

private:
    std::unique_ptr<::MctpBridge> impl_;
};

} // namespace iotorch::sermctp
