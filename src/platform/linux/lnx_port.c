/* SPDX-License-Identifier: Apache-2.0 */
#include "lnx_priv.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

/* The first five destination octets are 01:19:a7:00:00. */
static struct sock_filter raps_filter[] = {
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x0119a700u, 0, 3),
    BPF_STMT(BPF_LD | BPF_B | BPF_ABS, 4),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, 0xffffffffu),
    BPF_STMT(BPF_RET | BPF_K, 0)
};

int lnx_port_ref(lnx_t *l, int ifindex, lnx_port_t **out)
{
    for (lnx_port_t *p = l->ports; p; p = p->next) {
        if (p->ifindex == ifindex) {
            p->refs++;
            *out = p;
            return 0;
        }
    }
    lnx_port_t *p = calloc(1, sizeof *p);
    if (!p)
        return -1;
    p->fd = -1;
    p->ifindex = ifindex;
    /* Protocol 0: the socket receives nothing until bind() below. Created with ETH_P_ALL it
     * would queue frames from every interface before the bind and filter take effect, and
     * those would be attributed to this ring port. */
    p->fd = socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (p->fd < 0)
        goto fail;
    int one = 1;
    if (setsockopt(p->fd, SOL_PACKET, PACKET_AUXDATA, &one, sizeof one) < 0)
        goto fail;
    /* Older kernels may lack this option. The receive path also checks pkttype. */
    if (setsockopt(p->fd, SOL_PACKET, PACKET_IGNORE_OUTGOING, &one, sizeof one) < 0 &&
        errno != ENOPROTOOPT && errno != EINVAL)
        goto fail;
    struct sock_fprog prog = { .len = (unsigned short)(sizeof raps_filter / sizeof raps_filter[0]),
                               .filter = raps_filter };
    if (setsockopt(p->fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof prog) < 0)
        goto fail;
    struct sockaddr_ll addr;
    memset(&addr, 0, sizeof addr);
    addr.sll_family = AF_PACKET;
    addr.sll_protocol = htons(ETH_P_ALL);
    addr.sll_ifindex = ifindex;
    if (bind(p->fd, (const struct sockaddr *)&addr, sizeof addr) < 0)
        goto fail;
    if (lnx_ep_add(l, p->fd, EPOLLIN) < 0)
        goto fail;
    p->refs = 1;
    p->next = l->ports;
    l->ports = p;
    *out = p;
    return 0;
fail: {
    int saved = errno;
    if (p->fd >= 0)
        close(p->fd);
    free(p);
    errno = saved;
    return -1;
}
}

void lnx_port_unref(lnx_t *l, lnx_port_t *p)
{
    if (!p || --p->refs)
        return;
    lnx_port_t **pp = &l->ports;
    while (*pp && *pp != p)
        pp = &(*pp)->next;
    if (*pp)
        *pp = p->next;
    if (epoll_ctl(l->epfd, EPOLL_CTL_DEL, p->fd, NULL) < 0)
        lnx_log(l, ERPS_LOG_WARN, "epoll port remove: %s", strerror(errno));
    if (close(p->fd) < 0)
        lnx_log(l, ERPS_LOG_WARN, "close packet socket: %s", strerror(errno));
    free(p);
}

int lnx_port_tx(lnx_port_t *p, const uint8_t *frame, size_t len)
{
    if (!p || !frame || len < 14) {
        errno = EINVAL;
        return -1;
    }
    struct sockaddr_ll addr;
    memset(&addr, 0, sizeof addr);
    addr.sll_family = AF_PACKET;
    addr.sll_protocol = htons(ETH_P_8021Q);
    addr.sll_ifindex = p->ifindex;
    addr.sll_halen = ETH_ALEN;
    memcpy(addr.sll_addr, frame, ETH_ALEN);
    ssize_t n;
    do {
        n = sendto(p->fd, frame, len, 0, (const struct sockaddr *)&addr, sizeof addr);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return -1;
    if ((size_t)n != len) {
        errno = EIO;
        return -1;
    }
    return 0;
}

void lnx_port_read(lnx_t *l, lnx_port_t *p)
{
    for (unsigned count = 0; count < 64; count++) {
        uint8_t frame[4096 + 4];
        char control[CMSG_SPACE(sizeof(struct tpacket_auxdata))];
        struct sockaddr_ll from;
        struct iovec iov = { .iov_base = frame, .iov_len = sizeof frame - 4 };
        struct msghdr msg;
        memset(&msg, 0, sizeof msg);
        memset(&from, 0, sizeof from);
        msg.msg_name = &from;
        msg.msg_namelen = sizeof from;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof control;
        ssize_t n = recvmsg(p->fd, &msg, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR) {
                count--;
                continue;
            }
            lnx_log(l, ERPS_LOG_ERR, "packet receive ifindex %d: %s", p->ifindex,
                    strerror(errno));
            return;
        }
        if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || n < 14 ||
            from.sll_pkttype == PACKET_OUTGOING)
            continue;
        size_t len = (size_t)n;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level != SOL_PACKET || c->cmsg_type != PACKET_AUXDATA ||
                c->cmsg_len < CMSG_LEN(sizeof(struct tpacket_auxdata)))
                continue;
            struct tpacket_auxdata aux;
            memcpy(&aux, CMSG_DATA(c), sizeof aux);
            if (!(aux.tp_status & TP_STATUS_VLAN_VALID))
                break;
            uint16_t current = (uint16_t)(((unsigned)frame[12] << 8) | frame[13]);
            if (current == ETH_P_8021Q || current == ETH_P_8021AD)
                break;
            uint16_t tpid = (aux.tp_status & TP_STATUS_VLAN_TPID_VALID) ? aux.tp_vlan_tpid
                                                                       : ETH_P_8021Q;
            memmove(frame + 16, frame + 12, len - 12);
            frame[12] = (uint8_t)(tpid >> 8);
            frame[13] = (uint8_t)tpid;
            frame[14] = (uint8_t)(aux.tp_vlan_tci >> 8);
            frame[15] = (uint8_t)aux.tp_vlan_tci;
            len += 4;
            break;
        }
        (void)erps_mgr_rx_frame(erps_cli_mgr(l->cli), (uint32_t)p->ifindex, frame, len,
                                lnx_now());
    }
}
