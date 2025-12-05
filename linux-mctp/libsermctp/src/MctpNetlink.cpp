/**
 * @file MctpNetlink.cpp
 * @brief Raw NETLINK_ROUTE helper implementations for MCTP administrative tasks.
 *
 * This file defines a small, explicit set of helpers that construct and send
 * NETLINK_ROUTE messages for common MCTP administrative tasks used by the
 * test tooling in this repository. The helpers are intentionally low-level and
 * send raw RTM_* messages suitable for programmatic control or tests.
 *
 * Exposed helpers:
 *  - `setMctpInterfaceName(oldname, newname)` : rename an existing interface.
 *    Use when you need to rename a kernel-created `mctpserial` device.
 *  - `setMctpLocalEid(ifname, eid)` : add a local MCTP EID to an interface
 *    (sends `RTM_NEWADDR` with `IFA_LOCAL`).
 *  - `addMctpRoute(ifname, dest_eid)` : add a unicast MCTP route
 *    (`RTM_NEWROUTE` with `RTA_DST`/`RTA_OIF`).
 *  - `removeMctpRoute(ifname, dest_eid)` : remove a previously added route.
 *  - `setMctpInterfaceStatus(ifname, up)` : set interface up (true) or down (false)
 *    via `RTM_NEWLINK`.
 *  - `getMctpInterfaceNetId(ifname, out_net)` : query the MCTP network id
 *    from `IFLA_AF_SPEC`/`AF_MCTP` nested attributes.
 *  - `setMctpInterfaceNetId(ifname, net)` : set the MCTP network id by
 *    sending `RTM_NEWLINK` with an `IFLA_AF_SPEC`/`AF_MCTP` nested block.
 *
 * These helpers return true on success and false on failure and are intended
 * for diagnostic / control use in this userland toolset. They do not attempt
 * to implement a full rtnetlink client — use libnl for production code.
 *
 * @author Doug Sandy
 * @license MIT No Attribution (MIT-0)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED.
 */

#include "MctpNetlink_impl.hpp"
#include <cstring>
#include <iostream>
#include <vector>
extern "C" {
    unsigned int if_nametoindex(const char *);
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <unistd.h>
    #include <arpa/inet.h>
    #include <linux/netlink.h>
    #include <linux/rtnetlink.h>
    #include <errno.h>
    #include <cstdint>
    #include <netlink/netlink.h>
    #include <netlink/socket.h>
    #include <netlink/route/link.h>
    #include <netlink/msg.h>
    #include <netlink/attr.h>
}

/* Some platforms may not define AF_MCTP; provide a local fallback. */
#if !defined(AF_MCTP)
#define AF_MCTP 45
#endif

/* Some platforms may not define IFLA_MCTP_NET; provide a local fallback. */
#if !defined(IFLA_MCTP_NET)
#define IFLA_MCTP_NET 1
#endif
/* Some platforms may not define ARPHRD_NONE; provide a local fallback. */
#ifndef ARPHRD_NONE
#define ARPHRD_NONE 0
#endif

/* Minimal fallback for interface flags we use; real values come from <net/if.h>. */
#ifndef IFF_UP
#define IFF_UP 0x1
#endif

namespace mctpnet {

/**
 * @brief Helper: resolve interface name to index.
 *
 * @param ifname The interface name to resolve.
 * @return The interface index (non-zero) on success, or 0 if not found.
 */
static uint32_t ifindexByName(const std::string &ifname)
{
    return if_nametoindex(ifname.c_str());
}

/**
 * @brief Open a NETLINK_ROUTE socket and bind to the local address.
 *
 * @return A socket file descriptor on success, or -1 on failure.
 */
static int openNetlink()
{
    int sock = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (sock < 0) {
        std::cerr << "netlink socket open failed: " << strerror(errno) << "\n";
        return -1;
    }
    sockaddr_nl local;
    memset(&local, 0, sizeof(local));
    local.nl_family = AF_NETLINK;
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        std::cerr << "netlink bind failed: " << strerror(errno) << "\n";
        close(sock);
        return -1;
    }
    return sock;
}

/**
 * @brief Send a single netlink message and wait for an ACK/error reply.
 *
 * @param sock Socket file descriptor for a NETLINK_ROUTE socket.
 * @param nlh Pointer to a prepared `nlmsghdr` buffer; `nlmsg_len` must be set.
 * @return `true` if the kernel returns an ACK (no error), `false` on failure.
 */
static bool sendNetlinkMessage(int sock, struct nlmsghdr *nlh)
{
    sockaddr_nl nladdr;
    memset(&nladdr, 0, sizeof(nladdr));
    nladdr.nl_family = AF_NETLINK;

    iovec iov = { nlh, nlh->nlmsg_len };
    msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &nladdr;
    msg.msg_namelen = sizeof(nladdr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    ssize_t ret = sendmsg(sock, &msg, 0);
    if (ret < 0) {
        std::cerr << "sendmsg failed: " << strerror(errno) << "\n";
        return false;
    }

    // Wait for ACK (simple)
    std::vector<char> buf(8192);
    ssize_t len = recv(sock, buf.data(), buf.size(), 0);
    if (len < 0) {
        std::cerr << "recv ack failed: " << strerror(errno) << "\n";
        return false;
    }
    struct nlmsghdr *hdr = (struct nlmsghdr *)buf.data();
    if (hdr->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(hdr);
        if (err->error != 0) {
            std::cerr << "netlink error: " << strerror(-err->error) << "\n";
            return false;
        }
    }
    return true;
}

/**
 * @brief Append an RT attribute to the provided netlink message buffer.
 *
 * Caller is responsible for ensuring the buffer has sufficient space.
 * The function appends an `rtattr` at the current end of the `nlh` buffer
 * and updates `nlmsg_len` with proper alignment.
 *
 * @param nlh Pointer to the netlink message header buffer to modify.
 * @param attrtype Attribute type (rta_type) to append.
 * @param data Pointer to attribute payload, or NULL for zero-length attrs.
 * @param datalen Length of the payload in bytes.
 * @return void
 */
// Helper to append a single attribute (with correct alignment).
static void nlaPut(struct nlmsghdr *nlh, int attrtype, const void *data, int datalen)
{
    int cur = nlh->nlmsg_len;
    struct rtattr *rta = (struct rtattr *)(((char *)nlh) + NLMSG_ALIGN(cur));
    rta->rta_type = attrtype;
    rta->rta_len = RTA_LENGTH(datalen);
    if (datalen > 0 && data) memcpy(RTA_DATA(rta), data, datalen);
    nlh->nlmsg_len = NLMSG_ALIGN(cur) + RTA_ALIGN(rta->rta_len);
}

/**
 * @brief Start a nested rtattr.
 *
 * Returns the start offset in `*start` and a pointer to the nested `rtattr`.
 * The caller should append child attributes and then call `nlaNestEnd`
 * with the returned start value to finalize lengths and alignment.
 *
 * @param nlh Pointer to the netlink message header buffer being built.
 * @param attrtype The attribute type for the nested attribute (will be OR'ed with NLA_F_NESTED).
 * @param start Pointer to an int that will receive the start offset of this nested attribute.
 * @return Pointer to the newly-created parent `rtattr` for the nested block.
 */
static struct rtattr *nlaNestStart(struct nlmsghdr *nlh, int attrtype, int *start)
{
    int cur = nlh->nlmsg_len;
    struct rtattr *rta = (struct rtattr *)(((char *)nlh) + NLMSG_ALIGN(cur));
    rta->rta_type = attrtype | NLA_F_NESTED;
    rta->rta_len = RTA_LENGTH(0);
    nlh->nlmsg_len = NLMSG_ALIGN(cur) + RTA_LENGTH(0);
    if (start) *start = cur;
    return rta;
}

/**
 * @brief Finalize a nested rtattr started with `nlaNestStart`.
 *
 * Updates the parent attribute's length to include any child attributes and
 * advances the overall netlink message length with proper alignment.
 *
 * @param nlh Pointer to the netlink message header buffer being modified.
 * @param start The start offset previously returned in `*start` by `nlaNestStart`.
 * @return void
 */
static void nlaNestEnd(struct nlmsghdr *nlh, int start)
{
    struct rtattr *rta = (struct rtattr *)(((char *)nlh) + NLMSG_ALIGN(start));
    int nested_len = nlh->nlmsg_len - (NLMSG_ALIGN(start) + RTA_LENGTH(0));
    rta->rta_len = RTA_LENGTH(nested_len);
    nlh->nlmsg_len = NLMSG_ALIGN(start) + RTA_ALIGN(rta->rta_len);
}

/**
 * @brief Backwards-compatible wrapper for adding an attribute.
 *
 * Delegates to `nlaPut` to append an attribute with proper alignment and
 * length bookkeeping. Kept for compatibility with other helpers in this TU.
 *
 * @param nlh Pointer to the netlink message header buffer to modify.
 * @param attrtype Attribute type to add.
 * @param data Pointer to attribute payload.
 * @param datalen Payload length in bytes.
 * @return void
 */
static void addAttribute(struct nlmsghdr *nlh, int attrtype, const void *data, int datalen)
{
    nlaPut(nlh, attrtype, data, datalen);
}

/**
 * @brief Query the kernel for the interface flags for the named interface.
 *
 * @param ifname Name of the interface to query.
 * @param out_flags Reference to a uint32_t to receive the interface flags on success.
 * @return `true` and sets `out_flags` on success, `false` on error.
 */
static bool getInterfaceFlags(const std::string &ifname, uint32_t &out_flags)
{
    uint32_t ifindex = ifindexByName(ifname);
    if (ifindex == 0) return false;

    int sock = openNetlink();
    if (sock < 0) return false;

    std::vector<char> buffer(8192);
    struct nlmsghdr *nlh = (struct nlmsghdr *)buffer.data();
    memset(nlh, 0, buffer.size());

    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    nlh->nlmsg_flags = NLM_F_REQUEST;
    nlh->nlmsg_type = RTM_GETLINK;

    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = ifindex;

    ssize_t s = send(sock, nlh, nlh->nlmsg_len, 0);
    if (s < 0) {
        close(sock);
        return false;
    }

    ssize_t len = recv(sock, buffer.data(), buffer.size(), 0);
    if (len < 0) {
        close(sock);
        return false;
    }

    struct nlmsghdr *hdr = (struct nlmsghdr *)buffer.data();
    for (; NLMSG_OK(hdr, (size_t)len); hdr = NLMSG_NEXT(hdr, len)) {
        if (hdr->nlmsg_type == NLMSG_ERROR) continue;
        if (hdr->nlmsg_type != RTM_NEWLINK) continue;
        struct ifinfomsg *rifi = (struct ifinfomsg *)NLMSG_DATA(hdr);
        out_flags = rifi->ifi_flags;
        close(sock);
        return true;
    }

    close(sock);
    return false;
}

/**
 * @brief Get the MCTP network id assigned to the named interface.
 *
 * This parses RTM_GETLINK reply attributes looking for IFLA_AF_SPEC ->
 * AF_MCTP -> IFLA_MCTP_NET and returns the contained u32 network id.
 *
 * @param ifname Interface name to query.
 * @param out_net Reference to uint32_t to receive the MCTP network id on success.
 * @return `true` on success (and sets `out_net`), `false` on failure.
 */
bool getMctpInterfaceNetId(const std::string &ifname, uint32_t &out_net)
{
    uint32_t ifindex = ifindexByName(ifname);
    if (ifindex == 0) {
        std::cerr << "interface not found: " << ifname << "\n";
        return false;
    }

    int sock = openNetlink();
    if (sock < 0) return false;

    std::vector<char> buffer(8192);
    struct nlmsghdr *nlh = (struct nlmsghdr *)buffer.data();
    memset(nlh, 0, buffer.size());

    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    nlh->nlmsg_flags = NLM_F_REQUEST;
    nlh->nlmsg_type = RTM_GETLINK;

    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = ifindex;

    // send request
    ssize_t s = send(sock, nlh, nlh->nlmsg_len, 0);
    if (s < 0) {
        std::cerr << "send(GETLINK) failed: " << strerror(errno) << "\n";
        close(sock);
        return false;
    }

    ssize_t len = recv(sock, buffer.data(), buffer.size(), 0);
    if (len < 0) {
        std::cerr << "recv(GETLINK) failed: " << strerror(errno) << "\n";
        close(sock);
        return false;
    }

    struct nlmsghdr *hdr = (struct nlmsghdr *)buffer.data();
    for (; NLMSG_OK(hdr, (size_t)len); hdr = NLMSG_NEXT(hdr, len)) {
        if (hdr->nlmsg_type == NLMSG_ERROR) continue;
        if (hdr->nlmsg_type != RTM_NEWLINK) continue;

        struct ifinfomsg *rifi = (struct ifinfomsg *)NLMSG_DATA(hdr);
        int attrlen = hdr->nlmsg_len - NLMSG_LENGTH(sizeof(*rifi));
        struct rtattr *rta = (struct rtattr *)((char *)rifi + NLMSG_ALIGN(sizeof(*rifi)));
        for (; RTA_OK(rta, attrlen); rta = RTA_NEXT(rta, attrlen)) {
            if (rta->rta_type == IFLA_AF_SPEC) {
                // nested: each entry type is an AF_* value; payload is nested attrs
                int af_len = RTA_PAYLOAD(rta);
                struct rtattr *af = (struct rtattr *)RTA_DATA(rta);
                for (; RTA_OK(af, af_len); af = RTA_NEXT(af, af_len)) {
                    if ((int)af->rta_type != AF_MCTP) continue;
                    // parse MCTP-specific nested attrs
                    int mlen = RTA_PAYLOAD(af);
                    struct rtattr *m = (struct rtattr *)RTA_DATA(af);
                    for (; RTA_OK(m, mlen); m = RTA_NEXT(m, mlen)) {
                        if (m->rta_type == IFLA_MCTP_NET) {
                            int payload = RTA_PAYLOAD(m);
                            if (payload >= (int)sizeof(uint32_t)) {
                                uint32_t val;
                                memcpy(&val, RTA_DATA(m), sizeof(val));
                                out_net = val; // attributes use native endianness
                                close(sock);
                                return true;
                            } else if (payload == 1) {
                                uint8_t v = *(uint8_t *)RTA_DATA(m);
                                out_net = v;
                                close(sock);
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }

    close(sock);
    return false;
}

/**
 * @brief Set the MCTP network id for the named interface.
 *
 * This constructs an RTM_NEWLINK with IFLA_AF_SPEC -> AF_MCTP -> IFLA_MCTP_NET
 * nested attributes carrying a u32 network id.
 *
 * @param ifname Interface name to modify.
 * @param net The 32-bit MCTP network id to assign.
 * @return `true` on success, `false` on failure.
 */
bool setMctpInterfaceNetId(const std::string &ifname, uint32_t net)
{
    uint32_t ifindex = ifindexByName(ifname);
    if (ifindex == 0) {
        std::cerr << "interface not found: " << ifname << "\n";
        return false;
    }

    // Build a raw RTM_NEWLINK message with nested attributes:
    // IFLA_AF_SPEC -> AF_MCTP -> IFLA_MCTP_NET (u32)
    int sock = openNetlink();
    if (sock < 0) return false;

    std::vector<char> buffer(512);
    struct nlmsghdr *nlh = (struct nlmsghdr *)buffer.data();
    memset(nlh, 0, buffer.size());

    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_type = RTM_NEWLINK;

    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = ifindex;
    uint32_t cur_flags = 0;
    if (!getInterfaceFlags(ifname, cur_flags)) cur_flags = 0;
    // Set current interface flags and try a sequence of ifi_change masks
    // until the kernel accepts the AF_SPEC update. Masks are tried in order:
    ifi->ifi_flags = cur_flags;
    uint32_t masks_to_try[3];
    masks_to_try[0] = 0;                      // preferred: zero change mask
    masks_to_try[1] = cur_flags | IFF_UP;     // indicate UP bit change
    masks_to_try[2] = 0xFFFFFFFFu;            // fallback: all-ones mask

    // Build nested attributes correctly using helpers to ensure alignment.
    int start_afspec = 0;
    nlaNestStart(nlh, IFLA_AF_SPEC, &start_afspec);

    int start_af = 0;
    nlaNestStart(nlh, AF_MCTP, &start_af);

    // Add the MCTP-specific net id (u32)
    nlaPut(nlh, IFLA_MCTP_NET, &net, (int)sizeof(net));

    // Close nested AF_MCTP and then IFLA_AF_SPEC
    nlaNestEnd(nlh, start_af);
    nlaNestEnd(nlh, start_afspec);

    // Try each mask until one succeeds.
    bool ok = false;
    for (int mi = 0; mi < 3; ++mi) {
        ifi->ifi_change = masks_to_try[mi];

        ok = sendNetlinkMessage(sock, nlh);
        if (ok) break;
    }

    close(sock);
    return ok;
}

/**
 * @brief Delete the local MCTP EID for the named interface.
 *
 * Sends an `RTM_DELADDR` message with `IFA_LOCAL` and `IFA_ADDRESS`
 * attributes containing the single-byte EID value for the specified
 * interface index.
 *
 * @param ifname Interface name to modify.
 * @param eid Local MCTP endpoint identifier (single byte).
 * @return `true` if the kernel acknowledged the update, `false` on error.
 */
bool deleteMctpLocalEid(const std::string &ifname, uint8_t eid)
{
    uint32_t ifindex = ifindexByName(ifname);
    if (ifindex == 0) {
        std::cerr << "interface not found: " << ifname << "\n";
        return false;
    }

    int sock = openNetlink();
    if (sock < 0) return false;

    std::vector<char> buffer(256);
    struct nlmsghdr *nlh = (struct nlmsghdr *)buffer.data();
    memset(nlh, 0, buffer.size());

    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_type = RTM_DELADDR;

    struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(nlh);
    ifa->ifa_family = AF_MCTP;
    ifa->ifa_prefixlen = 0;
    ifa->ifa_index = ifindex;
    ifa->ifa_scope = 0;

    // IFA_LOCAL -> single byte EID
    addAttribute(nlh, IFA_LOCAL, &eid, 1);
    // Add IFA_ADDRESS for compatibility
    addAttribute(nlh, IFA_ADDRESS, &eid, 1);

    bool ok = sendNetlinkMessage(sock, nlh);
    close(sock);
    return ok;
}

/**
 * @brief Rename an existing interface from `oldname` to `newname`.
 *
 * Sends an `RTM_NEWLINK` update with an `IFLA_IFNAME` attribute for the
 * target interface index in order to request a rename.
 *
 * @param oldname The current name of the interface.
 * @param newname The desired new name for the interface.
 * @return `true` if the kernel acknowledged the request, `false` on error.
 */
bool setMctpInterfaceName(const std::string &oldname, const std::string &newname)
{
    // Rename an existing interface from `oldname` to `newname` by sending
    // RTM_NEWLINK with the target ifindex and an IFLA_IFNAME attribute.
    uint32_t ifindex = ifindexByName(oldname);
    if (ifindex == 0) {
        std::cerr << "interface not found: " << oldname << "\n";
        return false;
    }

    // Attempt a raw netlink RTM_NEWLINK update (best-effort).
    int sock = openNetlink();
    if (sock < 0) return false;

    std::vector<char> buffer(512);
    struct nlmsghdr *nlh = (struct nlmsghdr *)buffer.data();
    memset(nlh, 0, buffer.size());

    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    // Send an RTM_NEWLINK request with ACK
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_type = RTM_NEWLINK;

    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = ifindex;
    ifi->ifi_change = 0;

    // IFLA_IFNAME -> new name
    addAttribute(nlh, IFLA_IFNAME, newname.c_str(), (int)newname.size() + 1);

    bool ok = sendNetlinkMessage(sock, nlh);
    close(sock);
    return ok;
}

/**
 * @brief Set the operational status (up/down) of the named interface.
 *
 * Constructs and sends an `RTM_NEWLINK` message where `ifi_flags` is set
 * to `IFF_UP` when `up` is true and cleared when `up` is false. The
 * `ifi_change` field indicates which flags are changing.
 *
 * @param ifname Interface name to modify.
 * @param up True to bring the interface up, false to bring it down.
 * @return `true` on success (ACK received), `false` on failure.
 */
bool setMctpInterfaceStatus(const std::string &ifname, bool up)
{
    uint32_t ifindex = ifindexByName(ifname);
    if (ifindex == 0) {
        std::cerr << "interface not found: " << ifname << "\n";
        return false;
    }

    int sock = openNetlink();
    if (sock < 0) return false;

    std::vector<char> buffer(256);
    struct nlmsghdr *nlh = (struct nlmsghdr *)buffer.data();
    memset(nlh, 0, buffer.size());

    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    nlh->nlmsg_type = RTM_NEWLINK;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = ifindex;
    ifi->ifi_flags = up ? IFF_UP : 0;
    ifi->ifi_change = IFF_UP; // indicate which flags change

    bool ok = sendNetlinkMessage(sock, nlh);
    close(sock);
    return ok;
}

/**
 * @brief Set the local MCTP EID for the named interface.
 *
 * Sends an `RTM_NEWADDR` message with `IFA_LOCAL` and `IFA_ADDRESS`
 * attributes containing the single-byte EID value for the specified
 * interface index.
 *
 * @param ifname Interface name to modify.
 * @param eid Local MCTP endpoint identifier (single byte).
 * @return `true` if the kernel acknowledged the update, `false` on error.
 */
bool setMctpLocalEid(const std::string &ifname, uint8_t eid)
{
    uint32_t ifindex = ifindexByName(ifname);
    if (ifindex == 0) {
        std::cerr << "interface not found: " << ifname << "\n";
        return false;
    }

    int sock = openNetlink();
    if (sock < 0) return false;

    std::vector<char> buffer(256);
    struct nlmsghdr *nlh = (struct nlmsghdr *)buffer.data();
    memset(nlh, 0, buffer.size());

    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_type = RTM_NEWADDR;

    struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(nlh);
    ifa->ifa_family = AF_MCTP;
    ifa->ifa_prefixlen = 0;
    ifa->ifa_index = ifindex;
    ifa->ifa_scope = 0;

    // IFA_LOCAL -> single byte EID
    addAttribute(nlh, IFA_LOCAL, &eid, 1);
    // Add IFA_ADDRESS for compatibility
    addAttribute(nlh, IFA_ADDRESS, &eid, 1);

    bool ok = sendNetlinkMessage(sock, nlh);
    close(sock);
    return ok;
}

/**
 * @brief Add a unicast MCTP route for `dest_eid` using the specified interface.
 *
 * Constructs an `RTM_NEWROUTE` message in the `AF_MCTP` family where the
 * destination (RTA_DST) is a single-byte EID and RTA_OIF is the output
 * interface index.
 *
 * @param ifname Interface name to use as the outgoing interface.
 * @param dest_eid Destination MCTP EID to route to.
 * @return `true` on success (ACK received), `false` on failure.
 */
bool addMctpRoute(const std::string &ifname, uint8_t dest_eid)
{
    uint32_t ifindex = ifindexByName(ifname);
    if (ifindex == 0) {
        std::cerr << "interface not found: " << ifname << "\n";
        return false;
    }

    int sock = openNetlink();
    if (sock < 0) return false;

    std::vector<char> buffer(512);
    struct nlmsghdr *nlh = (struct nlmsghdr *)buffer.data();
    memset(nlh, 0, buffer.size());

    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    nlh->nlmsg_type = RTM_NEWROUTE;

    struct rtmsg *rt = (struct rtmsg *)NLMSG_DATA(nlh);
    rt->rtm_family = AF_MCTP;
    rt->rtm_table = RT_TABLE_MAIN;
    rt->rtm_protocol = RTPROT_BOOT;
    rt->rtm_scope = RT_SCOPE_UNIVERSE;
    rt->rtm_type = RTN_UNICAST;
    rt->rtm_dst_len = 0; // module treats this as an extent; adjust if needed

    // RTA_DST (single byte)
    addAttribute(nlh, RTA_DST, &dest_eid, 1);
    // RTA_OIF (u32)
    uint32_t oif = ifindex;
    addAttribute(nlh, RTA_OIF, &oif, sizeof(oif));

    bool ok = sendNetlinkMessage(sock, nlh);
    close(sock);
    return ok;
}

/**
 * @brief Remove an existing MCTP route for `dest_eid` on the specified interface.
 *
 * Sends an `RTM_DELROUTE` message in the `AF_MCTP` family with RTA_DST set
 * to the single-byte destination EID and RTA_OIF set to the interface index.
 *
 * @param ifname Interface name where the route exists.
 * @param dest_eid Destination MCTP EID for the route to remove.
 * @return `true` on success (ACK received), `false` on failure.
 */
bool removeMctpRoute(const std::string &ifname, uint8_t dest_eid)
{
    uint32_t ifindex = ifindexByName(ifname);
    if (ifindex == 0) {
        std::cerr << "interface not found: " << ifname << "\n";
        return false;
    }

    int sock = openNetlink();
    if (sock < 0) return false;

    std::vector<char> buffer(512);
    struct nlmsghdr *nlh = (struct nlmsghdr *)buffer.data();
    memset(nlh, 0, buffer.size());

    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_type = RTM_DELROUTE;

    struct rtmsg *rt = (struct rtmsg *)NLMSG_DATA(nlh);
    rt->rtm_family = AF_MCTP;
    rt->rtm_table = RT_TABLE_MAIN;
    rt->rtm_protocol = RTPROT_BOOT;
    rt->rtm_scope = RT_SCOPE_UNIVERSE;
    rt->rtm_type = RTN_UNICAST;
    rt->rtm_dst_len = 0;

    addAttribute(nlh, RTA_DST, &dest_eid, 1);
    uint32_t oif = ifindex;
    addAttribute(nlh, RTA_OIF, &oif, sizeof(oif));

    bool ok = sendNetlinkMessage(sock, nlh);
    close(sock);
    return ok;
}

} // namespace mctpnet
