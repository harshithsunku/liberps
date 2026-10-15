/* SPDX-License-Identifier: Apache-2.0 */
#include "lnx_priv.h"

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <linux/if.h>
#include <linux/if_bridge.h>
#include <linux/if_link.h>
#include <linux/neighbour.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NL_MAX_DGRAM (1024u * 1024u)
#define FDB_MAX_ENTRIES 8192u

typedef int (*nl_reply_fn)(const struct nlmsghdr *, void *);
enum nl_mode { NL_ACK, NL_SINGLE, NL_DUMP };

static int nl_socket(unsigned groups, bool nonblock)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | (nonblock ? SOCK_NONBLOCK : 0),
                    NETLINK_ROUTE);
    if (fd < 0)
        return -1;
    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof addr);
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = groups;
    if (bind(fd, (const struct sockaddr *)&addr, sizeof addr) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int lnx_nl_open(lnx_t *l)
{
    l->nl_fd = nl_socket(0, false);
    if (l->nl_fd < 0)
        return -1;
    l->link_fd = nl_socket(RTMGRP_LINK, true);
    if (l->link_fd < 0) {
        int saved = errno;
        close(l->nl_fd);
        l->nl_fd = -1;
        errno = saved;
        return -1;
    }
    return 0;
}

void lnx_nl_close(lnx_t *l)
{
    if (l->nl_fd >= 0 && close(l->nl_fd) < 0)
        lnx_log(l, ERPS_LOG_WARN, "close netlink: %s", strerror(errno));
    if (l->link_fd >= 0 && close(l->link_fd) < 0)
        lnx_log(l, ERPS_LOG_WARN, "close link monitor: %s", strerror(errno));
    l->nl_fd = l->link_fd = -1;
}

/* Peek at the datagram length, then receive the entire message. */
static int nl_read_datagram(int fd, bool nonblock, uint8_t **data, size_t *length,
                            struct sockaddr_nl *sender)
{
    uint8_t byte;
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    int flags = MSG_PEEK | MSG_TRUNC | (nonblock ? MSG_DONTWAIT : 0);
    ssize_t n;
    do {
        n = recvmsg(fd, &msg, flags);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return -errno;
    if (n == 0)
        return -EIO;
    if ((size_t)n > NL_MAX_DGRAM) {
        do {
            n = recvmsg(fd, &msg, MSG_TRUNC | (nonblock ? MSG_DONTWAIT : 0));
        } while (n < 0 && errno == EINTR);
        return -EMSGSIZE;
    }
    uint8_t *buf = malloc((size_t)n);
    if (!buf)
        return -ENOMEM;
    iov.iov_base = buf;
    iov.iov_len = (size_t)n;
    memset(&msg, 0, sizeof msg);
    memset(sender, 0, sizeof *sender);
    msg.msg_name = sender;
    msg.msg_namelen = sizeof *sender;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    ssize_t got;
    do {
        got = recvmsg(fd, &msg, nonblock ? MSG_DONTWAIT : 0);
    } while (got < 0 && errno == EINTR);
    if (got < 0 || (msg.msg_flags & MSG_TRUNC) || got != n) {
        int saved = got < 0 ? errno : EMSGSIZE;
        free(buf);
        return -saved;
    }
    *data = buf;
    *length = (size_t)n;
    return 0;
}

static bool nl_attr(struct nlmsghdr *h, size_t capacity, unsigned type, const void *value,
                    size_t value_len)
{
    size_t off = NLMSG_ALIGN(h->nlmsg_len);
    size_t step = RTA_ALIGN(RTA_LENGTH(value_len));
    if (off > capacity || step > capacity - off || value_len > UINT16_MAX - RTA_LENGTH(0))
        return false;
    struct rtattr *a = (struct rtattr *)((uint8_t *)h + off);
    a->rta_type = (unsigned short)type;
    a->rta_len = (unsigned short)RTA_LENGTH(value_len);
    if (value_len)
        memcpy(RTA_DATA(a), value, value_len);
    h->nlmsg_len = (uint32_t)(off + step);
    return true;
}

static int nl_request_inner(lnx_t *l, struct nlmsghdr *h, size_t capacity, enum nl_mode mode,
                            nl_reply_fn callback, void *ctx)
{
    (void)capacity;
    h->nlmsg_seq = ++l->nl_seq;
    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof addr);
    addr.nl_family = AF_NETLINK;
    ssize_t sent;
    do {
        sent = sendto(l->nl_fd, h, h->nlmsg_len, 0, (const struct sockaddr *)&addr, sizeof addr);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0)
        return -errno;
    if ((size_t)sent != h->nlmsg_len)
        return -EIO;
    erps_time_t until = lnx_now() + ERPS_SEC(5);
    int callback_error = 0;
    for (;;) {
        erps_time_t now = lnx_now();
        if (now >= until)
            return -ETIMEDOUT;
        int wait_ms = (int)((until - now + 999u) / 1000u);
        struct pollfd pfd = { .fd = l->nl_fd, .events = POLLIN };
        int ready;
        do {
            ready = poll(&pfd, 1, wait_ms);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0)
            return -errno;
        if (!ready)
            return -ETIMEDOUT;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -EIO;
        uint8_t *data = NULL;
        size_t len = 0;
        struct sockaddr_nl sender;
        int rc = nl_read_datagram(l->nl_fd, false, &data, &len, &sender);
        if (rc < 0)
            return rc;
        if (sender.nl_pid != 0) {
            free(data);
            return -EPROTO;
        }
        size_t remain = len;
        for (uint8_t *cursor = data; remain >= sizeof(struct nlmsghdr);) {
            struct nlmsghdr *m = (struct nlmsghdr *)cursor;
            if (m->nlmsg_len < sizeof *m || NLMSG_ALIGN(m->nlmsg_len) > remain) {
                free(data);
                return -EPROTO;
            }
            size_t step = NLMSG_ALIGN(m->nlmsg_len);
            if (m->nlmsg_seq != h->nlmsg_seq) {
                free(data);
                return -EPROTO;
            }
            if (m->nlmsg_type == NLMSG_ERROR) {
                if (m->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
                    free(data);
                    return -EPROTO;
                }
                const struct nlmsgerr *e = NLMSG_DATA(m);
                rc = e->error;
                free(data);
                return rc ? rc : callback_error;
            }
            if (m->nlmsg_type == NLMSG_DONE) {
                rc = (m->nlmsg_flags & NLM_F_DUMP_INTR) ? -EINTR : 0;
                free(data);
                return rc ? rc : callback_error;
            }
            if (callback && !callback_error) {
                rc = callback(m, ctx);
                if (rc < 0) {
                    if (mode != NL_DUMP) {
                        free(data);
                        return rc;
                    }
                    callback_error = rc;
                }
            }
            if (mode == NL_SINGLE) {
                free(data);
                return 0;
            }
            cursor += step;
            remain -= step;
        }
        free(data);
        if (remain != 0)
            return -EPROTO;
    }
}

static int nl_request(lnx_t *l, struct nlmsghdr *h, size_t capacity, enum nl_mode mode,
                      nl_reply_fn callback, void *ctx)
{
    int rc = nl_request_inner(l, h, capacity, mode, callback, ctx);
    /* A timed-out or malformed multipart reply can leave old sequence numbers queued. */
    if (rc == -ETIMEDOUT || rc == -EPROTO || rc == -EMSGSIZE || rc == -EIO ||
        rc == -EBADF) {
        if (l->nl_fd >= 0 && close(l->nl_fd) < 0)
            lnx_log(l, ERPS_LOG_WARN, "close failed netlink socket: %s", strerror(errno));
        l->nl_fd = nl_socket(0, false);
        if (l->nl_fd < 0)
            lnx_log(l, ERPS_LOG_ERR, "reopen netlink socket: %s", strerror(errno));
    }
    return rc;
}

/* Attribute type without the NLA_F_NESTED / NLA_F_NET_BYTEORDER flag bits (NLA_TYPE_MASK is a
 * negative int in the uapi header, which -Wsign-conversion rejects). */
#define LNX_NLA_TYPE(t) ((unsigned)(t) & 0x3FFFu)

/* Walk the attributes of a nested rtattr payload; returns false on malformed input. */
static bool nested_next(const uint8_t **cur, size_t *len, const struct rtattr **out)
{
    if (*len < sizeof(struct rtattr))
        return false;
    const struct rtattr *a = (const struct rtattr *)*cur;
    if (a->rta_len < sizeof *a || a->rta_len > *len)
        return false;
    size_t step = RTA_ALIGN(a->rta_len);
    if (step > *len)
        step = *len;
    *cur += step;
    *len -= step;
    *out = a;
    return true;
}

/* IFLA_LINKINFO { IFLA_INFO_KIND "bridge", IFLA_INFO_DATA { IFLA_BR_VLAN_FILTERING u8 } }.
 * Read from rtnetlink rather than sysfs: sysfs shows the network namespace it was mounted in,
 * not necessarily ours (containers, `unshare -n`). */
static void parse_linkinfo(const uint8_t *p, size_t len, lnx_link_info_t *info)
{
    const struct rtattr *a;
    const uint8_t *data = NULL;
    size_t data_len = 0;
    while (nested_next(&p, &len, &a)) {
        unsigned type = LNX_NLA_TYPE(a->rta_type);
        const uint8_t *payload = (const uint8_t *)a + RTA_LENGTH(0);
        size_t plen = RTA_PAYLOAD(a);
        if (type == IFLA_INFO_KIND && plen >= 6 && memcmp(payload, "bridge", 6) == 0 &&
            (plen == 6 || payload[6] == '\0'))
            info->is_bridge = true;
        else if (type == IFLA_INFO_DATA) {
            data = payload;
            data_len = plen;
        }
    }
    if (!info->is_bridge || !data)
        return;
    while (nested_next(&data, &data_len, &a)) {
        if (LNX_NLA_TYPE(a->rta_type) == IFLA_BR_VLAN_FILTERING && RTA_PAYLOAD(a) >= 1)
            info->vlan_filtering = *((const uint8_t *)a + RTA_LENGTH(0)) ? 1 : 0;
    }
}

static int parse_link(const struct nlmsghdr *m, void *ctx)
{
    lnx_link_info_t *info = ctx;
    if (m->nlmsg_type != RTM_NEWLINK || m->nlmsg_len < NLMSG_LENGTH(sizeof(struct ifinfomsg)))
        return -EPROTO;
    const struct ifinfomsg *ifi = (const void *)((const uint8_t *)m + NLMSG_HDRLEN);
    info->ifindex = ifi->ifi_index;
    info->vlan_filtering = -1;
    info->flags = ifi->ifi_flags;
    info->arptype = ifi->ifi_type;
    size_t len = m->nlmsg_len - NLMSG_LENGTH(sizeof *ifi);
    const uint8_t *cursor = (const uint8_t *)ifi + NLMSG_ALIGN(sizeof *ifi);
    while (len >= sizeof(struct rtattr)) {
        const struct rtattr *a = (const struct rtattr *)cursor;
        if (a->rta_len < sizeof *a || RTA_ALIGN(a->rta_len) > len)
            return -EPROTO;
        if (a->rta_type == IFLA_MASTER && RTA_PAYLOAD(a) >= sizeof(uint32_t)) {
            uint32_t master;
            memcpy(&master, (const uint8_t *)a + RTA_LENGTH(0), sizeof master);
            info->master = (int)master;
        } else if (a->rta_type == IFLA_ADDRESS && RTA_PAYLOAD(a) >= 6) {
            memcpy(info->mac, (const uint8_t *)a + RTA_LENGTH(0), 6);
            info->has_mac = true;
        } else if (LNX_NLA_TYPE(a->rta_type) == IFLA_LINKINFO) {
            parse_linkinfo((const uint8_t *)a + RTA_LENGTH(0), RTA_PAYLOAD(a), info);
        }
        size_t step = RTA_ALIGN(a->rta_len);
        cursor += step;
        len -= step;
    }
    return len == 0 ? 0 : -EPROTO;
}

int lnx_nl_get_link(lnx_t *l, int ifindex, lnx_link_info_t *info)
{
    struct {
        struct nlmsghdr n;
        struct ifinfomsg ifi;
    } req;
    memset(&req, 0, sizeof req);
    memset(info, 0, sizeof *info);
    req.n.nlmsg_len = NLMSG_LENGTH(sizeof req.ifi);
    req.n.nlmsg_type = RTM_GETLINK;
    req.n.nlmsg_flags = NLM_F_REQUEST;
    req.ifi.ifi_family = AF_UNSPEC;
    req.ifi.ifi_index = ifindex;
    int rc = nl_request(l, &req.n, sizeof req, NL_SINGLE, parse_link, info);
    return rc == 0 && info->ifindex != ifindex ? -ENODEV : rc;
}

int lnx_nl_bridge_filtering(lnx_t *l, int master)
{
    lnx_link_info_t info;
    int rc = lnx_nl_get_link(l, master, &info);
    if (rc < 0)
        return rc;
    if (!info.is_bridge)
        return -ENOTSUP;
    if (info.vlan_filtering < 0)
        return -EOPNOTSUPP; /* kernel did not report it */
    return info.vlan_filtering ? 0 : -EINVAL;
}

void lnx_nl_link_events(lnx_t *l)
{
    for (unsigned batch = 0; batch < 64; batch++) {
        uint8_t *data = NULL;
        size_t len = 0;
        struct sockaddr_nl sender;
        int rc = nl_read_datagram(l->link_fd, true, &data, &len, &sender);
        if (rc == -EAGAIN || rc == -EWOULDBLOCK)
            return;
        if (rc < 0) {
            lnx_log(l, ERPS_LOG_ERR, "link netlink receive: %s", strerror(-rc));
            return;
        }
        if (sender.nl_pid != 0) {
            free(data);
            continue;
        }
        size_t remain = len;
        for (uint8_t *cursor = data; remain >= sizeof(struct nlmsghdr);) {
            const struct nlmsghdr *m = (const struct nlmsghdr *)cursor;
            if (m->nlmsg_len < sizeof *m || NLMSG_ALIGN(m->nlmsg_len) > remain)
                break;
            size_t step = NLMSG_ALIGN(m->nlmsg_len);
            cursor += step;
            remain -= step;
            if ((m->nlmsg_type != RTM_NEWLINK && m->nlmsg_type != RTM_DELLINK) ||
                m->nlmsg_len < NLMSG_LENGTH(sizeof(struct ifinfomsg)))
                continue;
            const struct ifinfomsg *ifi = (const void *)((const uint8_t *)m + NLMSG_HDRLEN);
            bool sf = m->nlmsg_type == RTM_DELLINK ||
                      !(ifi->ifi_flags & IFF_UP && ifi->ifi_flags & IFF_LOWER_UP);
            for (lnx_port_t *p = l->ports; p; p = p->next) {
                if (p->ifindex != ifi->ifi_index || p->sf == sf)
                    continue;
                p->sf = sf;
                lnx_log(l, ERPS_LOG_NOTICE, "port ifindex %d signal fail %s", p->ifindex,
                        sf ? "on" : "off");
                (void)erps_mgr_set_signal_fail(erps_cli_mgr(l->cli), (uint32_t)p->ifindex, sf,
                                               lnx_now());
            }
        }
        if (remain != 0)
            lnx_log(l, ERPS_LOG_WARN, "malformed link netlink datagram");
        free(data);
    }
}

static int vlan_run(lnx_t *l, int ifindex, unsigned first, unsigned last, bool add)
{
    struct {
        struct nlmsghdr n;
        struct ifinfomsg ifi;
        uint8_t attrs[64];
    } req;
    memset(&req, 0, sizeof req);
    req.n.nlmsg_len = NLMSG_LENGTH(sizeof req.ifi);
    req.n.nlmsg_type = add ? RTM_SETLINK : RTM_DELLINK;
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.ifi.ifi_family = AF_BRIDGE;
    req.ifi.ifi_index = ifindex;
    size_t nest_off = NLMSG_ALIGN(req.n.nlmsg_len);
    if (!nl_attr(&req.n, sizeof req, IFLA_AF_SPEC, NULL, 0))
        return -E2BIG;
    struct bridge_vlan_info v = { .flags = first == last ? 0 : BRIDGE_VLAN_INFO_RANGE_BEGIN,
                                  .vid = (uint16_t)first };
    if (!nl_attr(&req.n, sizeof req, IFLA_BRIDGE_VLAN_INFO, &v, sizeof v))
        return -E2BIG;
    if (first != last) {
        v.flags = BRIDGE_VLAN_INFO_RANGE_END;
        v.vid = (uint16_t)last;
        if (!nl_attr(&req.n, sizeof req, IFLA_BRIDGE_VLAN_INFO, &v, sizeof v))
            return -E2BIG;
    }
    struct rtattr *nest = (struct rtattr *)((uint8_t *)&req + nest_off);
    nest->rta_len = (unsigned short)(req.n.nlmsg_len - nest_off);
    return nl_request(l, &req.n, sizeof req, NL_ACK, NULL, NULL);
}

int lnx_nl_vlan(lnx_t *l, int ifindex, const erps_cli_ring_extra_t *x, bool add)
{
    for (unsigned vid = 1; vid <= 4094;) {
        if (!erps_cli_vlan_test(x, vid)) {
            vid++;
            continue;
        }
        unsigned first = vid;
        while (vid < 4094 && erps_cli_vlan_test(x, vid + 1))
            vid++;
        unsigned last = vid++;
        int rc = vlan_run(l, ifindex, first, last, add);
        if (rc == (add ? -EEXIST : -ENOENT) && first != last) {
            for (unsigned one = first; one <= last; one++) {
                rc = vlan_run(l, ifindex, one, one, add);
                if (rc == (add ? -EEXIST : -ENOENT))
                    rc = 0;
                if (rc < 0)
                    return rc;
            }
        } else if (rc == (add ? -EEXIST : -ENOENT)) {
            rc = 0;
        }
        if (rc < 0)
            return rc;
    }
    return 0;
}

typedef struct {
    int ifindex;
    uint16_t vid;
    uint8_t mac[6];
} fdb_entry_t;

typedef struct {
    const lnx_inst_t *inst;
    fdb_entry_t *entries;
    size_t count;
} fdb_dump_t;

static int collect_fdb(const struct nlmsghdr *m, void *arg)
{
    fdb_dump_t *d = arg;
    if (m->nlmsg_type != RTM_NEWNEIGH || m->nlmsg_len < NLMSG_LENGTH(sizeof(struct ndmsg)))
        return -EPROTO;
    const struct ndmsg *ndm = (const void *)((const uint8_t *)m + NLMSG_HDRLEN);
    if (ndm->ndm_family != AF_BRIDGE ||
        (ndm->ndm_state & (NUD_PERMANENT | NUD_NOARP)) ||
        (ndm->ndm_flags & (NTF_EXT_LEARNED | NTF_STICKY)) ||
        (ndm->ndm_ifindex != d->inst->port[0]->ifindex &&
         ndm->ndm_ifindex != d->inst->port[1]->ifindex))
        return 0;
    uint16_t vid = 0;
    uint8_t mac[6];
    bool has_mac = false;
    size_t len = m->nlmsg_len - NLMSG_LENGTH(sizeof *ndm);
    const uint8_t *cursor = (const uint8_t *)ndm + NLMSG_ALIGN(sizeof *ndm);
    while (len >= sizeof(struct rtattr)) {
        const struct rtattr *a = (const struct rtattr *)cursor;
        if (a->rta_len < sizeof *a || RTA_ALIGN(a->rta_len) > len)
            return -EPROTO;
        if (a->rta_type == NDA_VLAN && RTA_PAYLOAD(a) >= sizeof vid)
            memcpy(&vid, (const uint8_t *)a + RTA_LENGTH(0), sizeof vid);
        if (a->rta_type == NDA_LLADDR && RTA_PAYLOAD(a) >= sizeof mac) {
            memcpy(mac, (const uint8_t *)a + RTA_LENGTH(0), sizeof mac);
            has_mac = true;
        }
        size_t step = RTA_ALIGN(a->rta_len);
        cursor += step;
        len -= step;
    }
    if (len != 0)
        return -EPROTO;
    if (!has_mac || !erps_cli_vlan_test(&d->inst->extra, vid))
        return 0;
    if (d->count == FDB_MAX_ENTRIES)
        return -ENOSPC;
    fdb_entry_t *e = &d->entries[d->count++];
    e->ifindex = ndm->ndm_ifindex;
    e->vid = vid;
    memcpy(e->mac, mac, sizeof mac);
    return 0;
}

static int flush_fallback(lnx_t *l, const lnx_inst_t *inst)
{
    fdb_dump_t d;
    memset(&d, 0, sizeof d);
    d.inst = inst;
    d.entries = calloc(FDB_MAX_ENTRIES, sizeof *d.entries);
    if (!d.entries)
        return -ENOMEM;
    struct {
        struct nlmsghdr n;
        struct ndmsg ndm;
    } req;
    memset(&req, 0, sizeof req);
    req.n.nlmsg_len = NLMSG_LENGTH(sizeof req.ndm);
    req.n.nlmsg_type = RTM_GETNEIGH;
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.ndm.ndm_family = AF_BRIDGE;
    int rc = nl_request(l, &req.n, sizeof req, NL_DUMP, collect_fdb, &d);
    for (size_t i = 0; rc == 0 && i < d.count; i++) {
        struct {
            struct nlmsghdr n;
            struct ndmsg ndm;
            uint8_t attrs[32];
        } del;
        memset(&del, 0, sizeof del);
        del.n.nlmsg_len = NLMSG_LENGTH(sizeof del.ndm);
        del.n.nlmsg_type = RTM_DELNEIGH;
        del.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
        del.ndm.ndm_family = AF_BRIDGE;
        del.ndm.ndm_ifindex = d.entries[i].ifindex;
        del.ndm.ndm_flags = NTF_MASTER;
        if (!nl_attr(&del.n, sizeof del, NDA_LLADDR, d.entries[i].mac, 6) ||
            !nl_attr(&del.n, sizeof del, NDA_VLAN, &d.entries[i].vid, sizeof(uint16_t))) {
            rc = -E2BIG;
            break;
        }
        rc = nl_request(l, &del.n, sizeof del, NL_ACK, NULL, NULL);
        if (rc == -ENOENT)
            rc = 0;
    }
    free(d.entries);
    return rc;
}

int lnx_nl_flush(lnx_t *l, const lnx_inst_t *inst)
{
    for (int port = 0; port < 2; port++) {
        for (unsigned vid = 1; vid <= 4094; vid++) {
            if (!erps_cli_vlan_test(&inst->extra, vid))
                continue;
            struct {
                struct nlmsghdr n;
                struct ndmsg ndm;
                uint8_t attrs[48];
            } req;
            memset(&req, 0, sizeof req);
            req.n.nlmsg_len = NLMSG_LENGTH(sizeof req.ndm);
            req.n.nlmsg_type = RTM_DELNEIGH;
            req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_BULK;
            req.ndm.ndm_family = AF_BRIDGE;
            req.ndm.ndm_ifindex = inst->port[port]->ifindex;
            req.ndm.ndm_flags = NTF_MASTER;
            uint16_t vlan = (uint16_t)vid;
            uint16_t state_mask = NUD_NOARP | NUD_PERMANENT;
            uint8_t flags_mask = NTF_EXT_LEARNED | NTF_STICKY;
            if (!nl_attr(&req.n, sizeof req, NDA_VLAN, &vlan, sizeof vlan) ||
                !nl_attr(&req.n, sizeof req, NDA_NDM_STATE_MASK, &state_mask, sizeof state_mask) ||
                !nl_attr(&req.n, sizeof req, NDA_NDM_FLAGS_MASK, &flags_mask, sizeof flags_mask))
                return -E2BIG;
            int rc = nl_request(l, &req.n, sizeof req, NL_ACK, NULL, NULL);
            if (rc == -EOPNOTSUPP || rc == -EINVAL) {
                lnx_log(l, ERPS_LOG_INFO, "kernel bulk FDB delete unavailable; using dump fallback");
                return flush_fallback(l, inst);
            }
            if (rc < 0)
                return rc;
        }
    }
    return 0;
}
