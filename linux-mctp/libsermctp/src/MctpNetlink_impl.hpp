/**
 * @file MctpNetlink_impl.hpp
 * @brief Internal declarations for small MCTP netlink helpers.
 */
#pragma once
#include <string>
#include <cstdint>

namespace mctpnet {

bool setMctpInterfaceName(const std::string &oldname, const std::string &newname);
bool setMctpLocalEid(const std::string &ifname, uint8_t eid);
bool deleteMctpLocalEid(const std::string &ifname, uint8_t eid);
bool addMctpRoute(const std::string &ifname, uint8_t dest_eid);
bool removeMctpRoute(const std::string &ifname, uint8_t dest_eid);
bool setMctpInterfaceStatus(const std::string &ifname, bool up);
bool getMctpInterfaceNetId(const std::string &ifname, uint32_t &out_net);
bool setMctpInterfaceNetId(const std::string &ifname, uint32_t net);

} // namespace mctpnet
