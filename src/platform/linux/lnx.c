/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "lnx_priv.h"

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <linux/if.h>
#include <net/if_arp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define LNX_MAX_CLIENTS 64u

erps_time_t lnx_now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (erps_time_t)ts.tv_sec * 1000000u + (erps_time_t)ts.tv_nsec / 1000u;
}

void lnx_log(lnx_t *l, erps_log_level_t level, const char *fmt, ...)
{
    if (level > l->level)
        return;
    erps_time_t now = lnx_now();
    FILE *out = l->log ? l->log : stderr;
    fprintf(out, "[%llu.%06llu] %-7s ", (unsigned long long)(now / 1000000u),
            (unsigned long long)(now % 1000000u), erps_log_level_str(level));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    fflush(out);
}

static int ep_ctl(lnx_t *l, int op, int fd, uint32_t events)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(l->epfd, op, fd, &ev);
}

int lnx_ep_add(lnx_t *l, int fd, uint32_t events)
{
    return ep_ctl(l, EPOLL_CTL_ADD, fd, events);
}

int lnx_ep_mod(lnx_t *l, int fd, uint32_t events)
{
    return ep_ctl(l, EPOLL_CTL_MOD, fd, events);
}

static erps_time_t cli_now(void *ctx)
{
    (void)ctx;
    return lnx_now();
}

static int port_mac(const char *name, uint8_t mac[6])
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    if (snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", name) >= (int)sizeof ifr.ifr_name) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    int rc = ioctl(fd, SIOCGIFHWADDR, &ifr);
    int saved = errno;
    if (close(fd) < 0 && rc == 0)
        return -1;
    if (rc < 0) {
        errno = saved;
        return -1;
    }
    if (ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    return 0;
}

static int cli_port_lookup(void *ctx, const char *name, uint32_t *handle, uint8_t mac[6])
{
    lnx_t *l = ctx;
    unsigned index = if_nametoindex(name);
    if (!index || port_mac(name, mac) < 0)
        return -1;
    *handle = index;
    l->last_lookup = index;
    return 0;
}

static int cli_system_mac(void *ctx, uint8_t mac[6])
{
    lnx_t *l = ctx;
    lnx_link_info_t link;
    if (l->last_lookup && lnx_nl_get_link(l, (int)l->last_lookup, &link) == 0 && link.master) {
        lnx_link_info_t bridge;
        if (lnx_nl_get_link(l, link.master, &bridge) == 0 && bridge.has_mac) {
            memcpy(mac, bridge.mac, 6);
            return 0;
        }
    }
    struct if_nameindex *ifs = if_nameindex();
    if (!ifs)
        return -1;
    int rc = -1;
    for (struct if_nameindex *i = ifs; i->if_index; i++) {
        if (lnx_nl_get_link(l, (int)i->if_index, &link) == 0 &&
            !(link.flags & IFF_LOOPBACK) && link.arptype == ARPHRD_ETHER && link.has_mac) {
            memcpy(mac, link.mac, 6);
            rc = 0;
            break;
        }
    }
    if_freenameindex(ifs);
    return rc;
}

static int op_tx(void *user, erps_port_t port, const uint8_t *frame, size_t len)
{
    lnx_inst_t *inst = user;
    if (port > ERPS_PORT1)
        return -EINVAL;
    int rc = lnx_port_tx(inst->port[port], frame, len);
    if (rc < 0)
        lnx_log(inst->lnx, ERPS_LOG_ERR, "%s Tx port %d: %s", inst->name, (int)port,
                strerror(errno));
    return rc;
}

static int op_set_port(void *user, erps_port_t port, unsigned mask)
{
    lnx_inst_t *inst = user;
    if (port > ERPS_PORT1)
        return -EINVAL;
    if (inst->lnx->shutting_down && !inst->lnx->block_on_exit)
        return 0;
    int rc = lnx_nl_vlan(inst->lnx, inst->port[port]->ifindex, &inst->extra,
                         !(mask & ERPS_BLOCK_TRAFFIC));
    if (rc < 0)
        lnx_log(inst->lnx, ERPS_LOG_ERR, "%s VLAN state port %d: %s", inst->name, (int)port,
                strerror(-rc));
    return rc;
}

static int op_flush(void *user)
{
    lnx_inst_t *inst = user;
    int rc = lnx_nl_flush(inst->lnx, inst);
    if (rc < 0)
        lnx_log(inst->lnx, ERPS_LOG_ERR, "%s FDB flush: %s", inst->name, strerror(-rc));
    return rc;
}

static void op_notify(void *user, const erps_event_t *ev)
{
    lnx_inst_t *inst = user;
    lnx_t *l = inst->lnx;
    switch (ev->type) {
    case ERPS_EV_STATE_CHANGE:
        lnx_log(l, ERPS_LOG_NOTICE, "%s state %s -> %s row %u request %s", inst->name,
                erps_state_str(ev->u.state.from), erps_state_str(ev->u.state.to),
                ev->u.state.row, erps_req_type_str(ev->u.state.req));
        break;
    case ERPS_EV_PORT_STATE:
        if (l->shutting_down && !l->block_on_exit)
            lnx_log(l, ERPS_LOG_NOTICE, "%s port %d core state %s; VLAN membership unchanged",
                    inst->name, (int)ev->u.port.port,
                    ev->u.port.blocked ? "blocked" : "forwarding");
        else
            lnx_log(l, ERPS_LOG_NOTICE, "%s port %d %s", inst->name,
                    (int)ev->u.port.port, ev->u.port.blocked ? "blocked" : "forwarding");
        break;
    case ERPS_EV_FLUSH:
        lnx_log(l, ERPS_LOG_NOTICE, "%s FDB flush", inst->name);
        break;
    case ERPS_EV_FOP_PM:
    case ERPS_EV_FOP_TO:
        lnx_log(l, ERPS_LOG_NOTICE, "%s %s %s port %d", inst->name,
                erps_event_str(ev->type), ev->u.fop.raised ? "raised" : "cleared",
                (int)ev->u.fop.port);
        break;
    case ERPS_EV_CMD_REJECTED:
        lnx_log(l, ERPS_LOG_NOTICE, "%s command %s rejected: %s", inst->name,
                erps_cmd_str(ev->u.reject.cmd), erps_reject_str(ev->u.reject.reason));
        break;
    case ERPS_EV_SF:
        lnx_log(l, ERPS_LOG_NOTICE, "%s signal fail port %d %s", inst->name,
                (int)ev->u.sf.port, ev->u.sf.sf ? "on" : "off");
        break;
    case ERPS_EV_PLATFORM_ERROR:
        lnx_log(l, ERPS_LOG_NOTICE, "%s platform %s error %d port %d", inst->name,
                ev->u.platform.op, ev->u.platform.rc, (int)ev->u.platform.port);
        break;
    case ERPS_EV_COUNT:
        break;
    }
}

static void op_log(void *user, erps_log_level_t level, const char *message)
{
    lnx_inst_t *inst = user;
    lnx_log(inst->lnx, level, "%s %s", inst->name, message);
}

static void bind_error(char *err, size_t len, const char *what, int code)
{
    snprintf(err, len, "%s: %s", what, strerror(code));
}

static int cli_bind(void *ctx, erps_config_t *cfg, const erps_cli_ring_extra_t *x,
                    erps_ops_t *ops, void **user, char *err, size_t errlen)
{
    lnx_t *l = ctx;
    unsigned indexes[2];
    lnx_link_info_t links[2];
    for (int p = 0; p < 2; p++) {
        indexes[p] = if_nametoindex(cfg->port[p].name);
        if (!indexes[p]) {
            bind_error(err, errlen, cfg->port[p].name, errno ? errno : ENODEV);
            return -1;
        }
        int rc = lnx_nl_get_link(l, (int)indexes[p], &links[p]);
        if (rc < 0 || !links[p].has_mac || links[p].arptype != ARPHRD_ETHER) {
            int reason = rc < 0 ? -rc : links[p].arptype != ARPHRD_ETHER ? EAFNOSUPPORT
                                                                      : EADDRNOTAVAIL;
            bind_error(err, errlen, cfg->port[p].name, reason);
            return -1;
        }
    }
    if (indexes[0] == indexes[1] || links[0].master == 0 ||
        links[0].master != links[1].master) {
        snprintf(err, errlen, "ring ports must be distinct ports of the same bridge");
        return -1;
    }
    int rc = lnx_nl_bridge_filtering(l, links[0].master);
    if (rc < 0) {
        bind_error(err, errlen, "bridge vlan_filtering=1 required", -rc);
        return -1;
    }
    if (erps_cli_vlan_test(x, cfg->raps_vid)) {
        snprintf(err, errlen, "R-APS VID %u must not be protected", cfg->raps_vid);
        return -1;
    }
    lnx_inst_t *inst = calloc(1, sizeof *inst);
    if (!inst) {
        bind_error(err, errlen, "instance", ENOMEM);
        return -1;
    }
    inst->lnx = l;
    inst->extra = *x;
    snprintf(inst->name, sizeof inst->name, "%s", cfg->name);
    for (int p = 0; p < 2; p++) {
        if (lnx_port_ref(l, (int)indexes[p], &inst->port[p]) < 0) {
            int saved = errno;
            for (int j = 0; j < p; j++)
                lnx_port_unref(l, inst->port[j]);
            free(inst);
            bind_error(err, errlen, "packet socket", saved);
            return -1;
        }
        inst->port[p]->sf = !(links[p].flags & IFF_UP && links[p].flags & IFF_LOWER_UP);
        cfg->port[p].handle = indexes[p];
        memcpy(cfg->port[p].mac, links[p].mac, 6);
    }
    cfg->sw_raps_forwarding = true;
    cfg->log_level = l->level;
    inst->initial_sf_pending = true;
    inst->next = l->instances;
    l->instances = inst;
    ops->tx = op_tx;
    ops->set_port_state = op_set_port;
    ops->flush_fdb = op_flush;
    ops->notify = op_notify;
    ops->log = op_log;
    *user = inst;
    return 0;
}

static void cli_unbind(void *ctx, void *user)
{
    lnx_t *l = ctx;
    lnx_inst_t *inst = user;
    if (!inst)
        return;
    lnx_inst_t **it = &l->instances;
    while (*it && *it != inst)
        it = &(*it)->next;
    if (*it)
        *it = inst->next;
    for (int p = 0; p < 2; p++)
        lnx_port_unref(l, inst->port[p]);
    free(inst);
}

static void sync_initial_sf(lnx_t *l)
{
    for (lnx_inst_t *inst = l->instances; inst; inst = inst->next) {
        if (!inst->initial_sf_pending || !erps_mgr_find(erps_cli_mgr(l->cli), inst->name))
            continue;
        inst->initial_sf_pending = false;
        for (int p = 0; p < 2; p++)
            (void)erps_mgr_set_signal_fail(erps_cli_mgr(l->cli),
                                           (uint32_t)inst->port[p]->ifindex, inst->port[p]->sf,
                                           lnx_now());
    }
}

static void client_close(lnx_t *l, lnx_client_t *c)
{
    lnx_client_t **it = &l->clients;
    while (*it && *it != c)
        it = &(*it)->next;
    if (*it)
        *it = c->next;
    l->client_count--;
    if (epoll_ctl(l->epfd, EPOLL_CTL_DEL, c->fd, NULL) < 0)
        lnx_log(l, ERPS_LOG_WARN, "epoll client remove: %s", strerror(errno));
    if (close(c->fd) < 0)
        lnx_log(l, ERPS_LOG_WARN, "close client: %s", strerror(errno));
    free(c);
}

static void client_compact(lnx_client_t *c)
{
    if (c->out_off) {
        memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
        c->out_len -= c->out_off;
        c->out_off = 0;
    }
}

static void client_append(lnx_client_t *c, const char *data, size_t len)
{
    client_compact(c);
    size_t limit = sizeof c->out - 64;
    size_t avail = c->out_len < limit ? limit - c->out_len : 0;
    if (len > avail) {
        len = avail;
        c->overflow = true;
    }
    if (len) {
        memcpy(c->out + c->out_len, data, len);
        c->out_len += len;
    }
}

static void cli_output(void *ctx, const char *message)
{
    lnx_client_t *c = ctx;
    client_append(c, message, strlen(message));
}

/* The normal output cap leaves space for this marker and the mandatory trailer. */
static void client_append_final(lnx_client_t *c, const char *data, size_t len)
{
    client_compact(c);
    if (len > sizeof c->out - c->out_len)
        return;
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;
}

static void client_line(lnx_t *l, lnx_client_t *c)
{
    if (c->in_len && c->in[c->in_len - 1] == '\r')
        c->in_len--;
    c->in[c->in_len] = '\0';
    c->overflow = false;
    erps_err_t rc;
    if (c->discard_line) {
        client_append(c, "% line too long\n", strlen("% line too long\n"));
        rc = ERPS_E_INVAL;
    } else {
        rc = erps_cli_exec(l->cli, c->in, cli_output, c);
        sync_initial_sf(l);
    }
    if (c->overflow) {
        client_append_final(c, "\n% output truncated\n", strlen("\n% output truncated\n"));
        rc = ERPS_E_NOSPACE;
    }
    char trailer[32];
    int n = snprintf(trailer, sizeof trailer, "%c%d\n", LNX_CTL_TRAILER, (int)rc);
    if (n > 0)
        client_append_final(c, trailer, (size_t)n);
    c->in_len = 0;
    c->discard_line = false;
}

static int client_flush(lnx_client_t *c)
{
    while (c->out_off < c->out_len) {
        ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off,
                         MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            c->out_off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        return -1;
    }
    c->out_len = c->out_off = 0;
    return 0;
}

static int client_read(lnx_t *l, lnx_client_t *c)
{
    for (unsigned i = 0; i < 4096 && c->out_len - c->out_off < 65536; i++) {
        char ch;
        ssize_t n = recv(c->fd, &ch, 1, MSG_DONTWAIT);
        if (n == 0) {
            c->read_closed = true;
            return 0;
        }
        if (n < 0) {
            if (errno == EINTR) {
                i--;
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            return -1;
        }
        if (ch == '\n') {
            client_line(l, c);
        } else if (!c->discard_line && c->in_len < 2048) {
            c->in[c->in_len++] = ch;
        } else {
            c->discard_line = true;
        }
    }
    return 0;
}

static void client_event(lnx_t *l, lnx_client_t *c, uint32_t events)
{
    if (events & (EPOLLERR | EPOLLHUP)) {
        client_close(l, c);
        return;
    }
    if ((events & EPOLLOUT) && client_flush(c) < 0) {
        client_close(l, c);
        return;
    }
    if ((events & (EPOLLIN | EPOLLRDHUP)) && client_read(l, c) < 0) {
        client_close(l, c);
        return;
    }
    if (c->out_len > c->out_off && client_flush(c) < 0) {
        client_close(l, c);
        return;
    }
    if (c->read_closed && c->out_len == c->out_off) {
        client_close(l, c);
        return;
    }
    uint32_t wanted = EPOLLRDHUP;
    if (!c->read_closed && c->out_len - c->out_off < 65536)
        wanted |= EPOLLIN;
    if (c->out_len > c->out_off)
        wanted |= EPOLLOUT;
    if (lnx_ep_mod(l, c->fd, wanted) < 0) {
        lnx_log(l, ERPS_LOG_ERR, "epoll client update: %s", strerror(errno));
        client_close(l, c);
    }
}

static void accept_clients(lnx_t *l)
{
    for (unsigned count = 0; count < 32; count++) {
        int fd = accept4(l->ctl_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) {
                count--;
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                lnx_log(l, ERPS_LOG_ERR, "accept: %s", strerror(errno));
            return;
        }
        if (l->client_count == LNX_MAX_CLIENTS) {
            close(fd);
            continue;
        }
        lnx_client_t *c = calloc(1, sizeof *c);
        if (!c) {
            close(fd);
            return;
        }
        c->fd = fd;
        if (lnx_ep_add(l, fd, EPOLLIN | EPOLLRDHUP) < 0) {
            lnx_log(l, ERPS_LOG_ERR, "epoll client add: %s", strerror(errno));
            close(fd);
            free(c);
            continue;
        }
        c->next = l->clients;
        l->clients = c;
        l->client_count++;
    }
}

static int arm_timer(lnx_t *l)
{
    erps_time_t deadline = erps_mgr_next_deadline(erps_cli_mgr(l->cli));
    struct itimerspec ts;
    memset(&ts, 0, sizeof ts);
    if (deadline != ERPS_TIME_NEVER) {
        ts.it_value.tv_sec = (time_t)(deadline / 1000000u);
        ts.it_value.tv_nsec = (long)((deadline % 1000000u) * 1000u);
    }
    return timerfd_settime(l->timer_fd, TFD_TIMER_ABSTIME, &ts, NULL);
}

void lnx_opts_init(lnx_opts_t *o)
{
    memset(o, 0, sizeof *o);
    o->ctl_path = "/run/erpsd.sock";
    o->log_level = ERPS_LOG_NOTICE;
    o->log = stderr;
    o->block_on_exit = true;
}

static int create_control(lnx_t *l)
{
    struct stat st;
    if (lstat(l->ctl_path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            errno = EEXIST;
            return -1;
        }
        int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (probe < 0)
            return -1;
        struct sockaddr_un old;
        memset(&old, 0, sizeof old);
        old.sun_family = AF_UNIX;
        snprintf(old.sun_path, sizeof old.sun_path, "%s", l->ctl_path);
        int connected;
        do {
            connected = connect(probe, (const struct sockaddr *)&old, sizeof old);
        } while (connected < 0 && errno == EINTR);
        int saved = errno;
        if (close(probe) < 0)
            return -1;
        if (connected == 0 || saved == EINPROGRESS) {
            errno = EADDRINUSE;
            return -1;
        }
        if (saved != ECONNREFUSED) {
            errno = saved;
            return -1;
        }
        if (unlink(l->ctl_path) < 0)
            return -1;
    } else if (errno != ENOENT) {
        return -1;
    }
    l->ctl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (l->ctl_fd < 0)
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", l->ctl_path);
    if (bind(l->ctl_fd, (const struct sockaddr *)&addr, sizeof addr) < 0)
        return -1;
    l->ctl_bound = true;
    if (chmod(l->ctl_path, 0660) < 0)
        return -1;
    if (listen(l->ctl_fd, 32) < 0)
        return -1;
    return lnx_ep_add(l, l->ctl_fd, EPOLLIN);
}

lnx_t *lnx_create(const lnx_opts_t *options)
{
    lnx_opts_t defaults;
    lnx_opts_init(&defaults);
    const lnx_opts_t *o = options ? options : &defaults;
    lnx_t *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    l->epfd = l->timer_fd = l->sig_fd = l->ctl_fd = l->nl_fd = l->link_fd = -1;
    l->level = o->log_level;
    l->log = o->log ? o->log : stderr;
    l->block_on_exit = o->block_on_exit;
    const char *path = o->ctl_path ? o->ctl_path : defaults.ctl_path;
    if (strlen(path) >= sizeof l->ctl_path || !path[0]) {
        lnx_log(l, ERPS_LOG_ERR, "control socket path too long or empty");
        free(l);
        return NULL;
    }
    snprintf(l->ctl_path, sizeof l->ctl_path, "%s", path);
    l->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (l->epfd < 0)
        goto fail;
    if (lnx_nl_open(l) < 0 || lnx_ep_add(l, l->link_fd, EPOLLIN) < 0)
        goto fail;
    l->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (l->timer_fd < 0 || lnx_ep_add(l, l->timer_fd, EPOLLIN) < 0)
        goto fail;
    sigset_t mask;
    if (sigemptyset(&mask) < 0 || sigaddset(&mask, SIGINT) < 0 ||
        sigaddset(&mask, SIGTERM) < 0 || sigprocmask(SIG_BLOCK, &mask, &l->old_mask) < 0)
        goto fail;
    l->saved_mask = true;
    l->sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (l->sig_fd < 0 || lnx_ep_add(l, l->sig_fd, EPOLLIN) < 0)
        goto fail;
    erps_cli_platform_t platform = { .ctx = l, .now = cli_now, .port_lookup = cli_port_lookup,
        .system_mac = cli_system_mac, .instance_bind = cli_bind,
        .instance_unbind = cli_unbind };
    l->cli = erps_cli_create(&platform);
    if (!l->cli) {
        errno = ENOMEM;
        goto fail;
    }
    if (create_control(l) < 0)
        goto fail;
    return l;
fail:
    lnx_log(l, ERPS_LOG_ERR, "Linux platform setup: %s", strerror(errno));
    lnx_destroy(l);
    return NULL;
}

erps_cli_t *lnx_cli(lnx_t *l)
{
    return l ? l->cli : NULL;
}

int lnx_run(lnx_t *l)
{
    if (!l)
        return 1;
    sync_initial_sf(l);
    while (!l->stop) {
        (void)erps_mgr_poll(erps_cli_mgr(l->cli), lnx_now());
        if (arm_timer(l) < 0) {
            lnx_log(l, ERPS_LOG_ERR, "timer arm: %s", strerror(errno));
            return 1;
        }
        struct epoll_event events[64];
        int n = epoll_wait(l->epfd, events, 64, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            lnx_log(l, ERPS_LOG_ERR, "epoll wait: %s", strerror(errno));
            return 1;
        }
        for (int i = 0; i < n && !l->stop; i++) {
            int fd = events[i].data.fd;
            if (fd == l->sig_fd) {
                struct signalfd_siginfo info;
                ssize_t got;
                do {
                    got = read(fd, &info, sizeof info);
                } while (got < 0 && errno == EINTR);
                if (got == (ssize_t)sizeof info &&
                    (info.ssi_signo == SIGINT || info.ssi_signo == SIGTERM))
                    l->stop = true;
                else if (got < 0 && errno != EAGAIN)
                    lnx_log(l, ERPS_LOG_ERR, "signalfd: %s", strerror(errno));
            } else if (fd == l->timer_fd) {
                uint64_t expirations;
                ssize_t got;
                do {
                    got = read(fd, &expirations, sizeof expirations);
                } while (got < 0 && errno == EINTR);
                if (got < 0 && errno != EAGAIN)
                    lnx_log(l, ERPS_LOG_ERR, "timerfd: %s", strerror(errno));
            } else if (fd == l->ctl_fd) {
                accept_clients(l);
            } else if (fd == l->link_fd) {
                lnx_nl_link_events(l);
            } else {
                bool found = false;
                for (lnx_port_t *p = l->ports; p; p = p->next) {
                    if (p->fd == fd) {
                        lnx_port_read(l, p);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    for (lnx_client_t *c = l->clients; c; c = c->next) {
                        if (c->fd == fd) {
                            client_event(l, c, events[i].events);
                            break;
                        }
                    }
                }
            }
            sync_initial_sf(l);
            (void)erps_mgr_poll(erps_cli_mgr(l->cli), lnx_now());
            if (arm_timer(l) < 0) {
                lnx_log(l, ERPS_LOG_ERR, "timer arm: %s", strerror(errno));
                return 1;
            }
        }
    }
    return 0;
}

void lnx_destroy(lnx_t *l)
{
    if (!l)
        return;
    while (l->clients)
        client_close(l, l->clients);
    l->shutting_down = true;
    if (l->cli)
        erps_cli_destroy(l->cli);
    while (l->instances)
        cli_unbind(l, l->instances);
    while (l->ports)
        lnx_port_unref(l, l->ports);
    if (l->ctl_fd >= 0 && close(l->ctl_fd) < 0)
        lnx_log(l, ERPS_LOG_WARN, "close control socket: %s", strerror(errno));
    if (l->ctl_bound && unlink(l->ctl_path) < 0 && errno != ENOENT)
        lnx_log(l, ERPS_LOG_WARN, "unlink control socket: %s", strerror(errno));
    if (l->sig_fd >= 0 && close(l->sig_fd) < 0)
        lnx_log(l, ERPS_LOG_WARN, "close signalfd: %s", strerror(errno));
    if (l->timer_fd >= 0 && close(l->timer_fd) < 0)
        lnx_log(l, ERPS_LOG_WARN, "close timerfd: %s", strerror(errno));
    lnx_nl_close(l);
    if (l->epfd >= 0 && close(l->epfd) < 0)
        lnx_log(l, ERPS_LOG_WARN, "close epoll: %s", strerror(errno));
    if (l->saved_mask && sigprocmask(SIG_SETMASK, &l->old_mask, NULL) < 0)
        lnx_log(l, ERPS_LOG_WARN, "restore signal mask: %s", strerror(errno));
    free(l);
}
