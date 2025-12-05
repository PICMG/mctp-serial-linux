// Test-only netlink message builders and parsers.
// These live under tests/ only and do not modify src/.

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>

static void addAttribute(struct nlmsghdr *nlh, int attrtype, const void *data, int datalen)
{
    int cur_len = nlh->nlmsg_len;
    struct rtattr *rta = (struct rtattr *)(((char *)nlh) + NLMSG_ALIGN(cur_len));
    rta->rta_type = attrtype;
    rta->rta_len = RTA_LENGTH(datalen);
    memcpy(RTA_DATA(rta), data, datalen);
    nlh->nlmsg_len = NLMSG_ALIGN(cur_len) + RTA_LENGTH(datalen);
}

static std::vector<char> build_rtm_newlink(uint32_t ifindex, const std::string &newname)
{
    std::vector<char> buffer(512);
    struct nlmsghdr *nlh = (struct nlmsghdr *)buffer.data();
    memset(nlh, 0, buffer.size());
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_type = RTM_NEWLINK;
    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = ifindex;
    ifi->ifi_change = 0;
    addAttribute(nlh, IFLA_IFNAME, newname.c_str(), (int)newname.size() + 1);
    buffer.resize(nlh->nlmsg_len);
    return buffer;
}

static std::vector<char> build_rtm_newaddr(uint32_t ifindex, uint8_t eid)
{
#ifndef AF_MCTP
#define AF_MCTP 40
#endif
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
    addAttribute(nlh, IFA_LOCAL, &eid, 1);
    addAttribute(nlh, IFA_ADDRESS, &eid, 1);
    buffer.resize(nlh->nlmsg_len);
    return buffer;
}

static std::vector<char> build_rtm_newroute(uint32_t ifindex, uint8_t dest_eid)
{
#ifndef AF_MCTP
#define AF_MCTP 40
#endif
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
    rt->rtm_dst_len = 0;
    addAttribute(nlh, RTA_DST, &dest_eid, 1);
    uint32_t oif = ifindex;
    addAttribute(nlh, RTA_OIF, &oif, sizeof(oif));
    buffer.resize(nlh->nlmsg_len);
    return buffer;
}

static bool parse_ifname_from_newlink(const std::vector<char> &buf, uint32_t expect_ifindex, const std::string &expect_name)
{
    if (buf.size() < sizeof(struct nlmsghdr)) return false;
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf.data();
    if (nlh->nlmsg_type != RTM_NEWLINK) return false;
    if ((size_t)nlh->nlmsg_len > buf.size()) return false;
    struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
    if ((uint32_t)ifi->ifi_index != expect_ifindex) return false;
    int attrlen = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi));
    struct rtattr *rta = (struct rtattr *)(((char *)ifi) + NLMSG_ALIGN(sizeof(*ifi)));
    while (RTA_OK(rta, attrlen)) {
        if (rta->rta_type == IFLA_IFNAME) {
            const char *name = (const char *)RTA_DATA(rta);
            if (strcmp(name, expect_name.c_str()) == 0) return true;
        }
        rta = RTA_NEXT(rta, attrlen);
    }
    return false;
}

static bool parse_eid_from_newaddr(const std::vector<char> &buf, uint32_t expect_ifindex, uint8_t expect_eid)
{
    if (buf.size() < sizeof(struct nlmsghdr)) return false;
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf.data();
    if (nlh->nlmsg_type != RTM_NEWADDR) return false;
    if ((size_t)nlh->nlmsg_len > buf.size()) return false;
    struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(nlh);
    if ((uint32_t)ifa->ifa_index != expect_ifindex) return false;
    int attrlen = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*ifa));
    struct rtattr *rta = (struct rtattr *)(((char *)ifa) + NLMSG_ALIGN(sizeof(*ifa)));
    while (RTA_OK(rta, attrlen)) {
        if (rta->rta_type == IFA_LOCAL || rta->rta_type == IFA_ADDRESS) {
            const uint8_t *v = (const uint8_t *)RTA_DATA(rta);
            if (*v == expect_eid) return true;
        }
        rta = RTA_NEXT(rta, attrlen);
    }
    return false;
}

static bool parse_route(const std::vector<char> &buf, uint32_t expect_oif, uint8_t expect_dst)
{
    if (buf.size() < sizeof(struct nlmsghdr)) return false;
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf.data();
    if (nlh->nlmsg_type != RTM_NEWROUTE) return false;
    if ((size_t)nlh->nlmsg_len > buf.size()) return false;
    struct rtmsg *rt = (struct rtmsg *)NLMSG_DATA(nlh);
    if ((int)rt->rtm_family != AF_MCTP) return false;
    int attrlen = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*rt));
    struct rtattr *rta = (struct rtattr *)(((char *)rt) + NLMSG_ALIGN(sizeof(*rt)));
    bool found_oif = false, found_dst = false;
    while (RTA_OK(rta, attrlen)) {
        if (rta->rta_type == RTA_OIF) {
            uint32_t oif; memcpy(&oif, RTA_DATA(rta), sizeof(oif));
            if (oif == expect_oif) found_oif = true;
        }
        if (rta->rta_type == RTA_DST) {
            const uint8_t *d = (const uint8_t *)RTA_DATA(rta);
            if (*d == expect_dst) found_dst = true;
        }
        rta = RTA_NEXT(rta, attrlen);
    }
    return found_oif && found_dst;
}

// Single entrypoint for test-runner
bool MctpNetlink_builder_tests()
{
    bool ok = true;
    ok &= parse_ifname_from_newlink(build_rtm_newlink(5, "mctp0"), 5, "mctp0");
    ok &= parse_eid_from_newaddr(build_rtm_newaddr(7, 42), 7, 42);
    ok &= parse_route(build_rtm_newroute(10, 9), 10, 9);
    // malformed: truncated newlink
    auto b = build_rtm_newlink(3, "x");
    std::vector<char> trunc(b.begin(), b.begin() + (b.size()/2));
    ok &= !parse_ifname_from_newlink(trunc, 3, "x");
    return ok;
}
