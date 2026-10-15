/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ERPS_LINUX_PRIVATE_H
#define ERPS_LINUX_PRIVATE_H

#include "lnx.h"

#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <sys/types.h>

typedef struct lnx_port {
    struct lnx_port *next;
    int fd;
    int ifindex;
    unsigned refs;
    bool sf;
} lnx_port_t;

typedef struct lnx_inst {
    struct lnx_inst *next;
    struct lnx *lnx;
    lnx_port_t *port[2];
    erps_cli_ring_extra_t extra;
    char name[ERPS_NAME_MAX];
    bool initial_sf_pending;
} lnx_inst_t;

typedef struct lnx_client {
    struct lnx_client *next;
    int fd;
    char in[2049];
    size_t in_len;
    char out[131072];
    size_t out_len;
    size_t out_off;
    bool overflow;
    bool discard_line;
    bool read_closed;
} lnx_client_t;

struct lnx {
    int epfd;
    int timer_fd;
    int sig_fd;
    int ctl_fd;
    int nl_fd;
    int link_fd;
    unsigned nl_seq;
    erps_cli_t *cli;
    lnx_port_t *ports;
    lnx_inst_t *instances;
    lnx_client_t *clients;
    unsigned client_count;
    unsigned last_lookup;
    bool block_on_exit;
    bool stop;
    bool shutting_down;
    bool ctl_bound;
    bool saved_mask;
    sigset_t old_mask;
    char ctl_path[108];
    erps_log_level_t level;
    FILE *log;
};

void lnx_log(lnx_t *l, erps_log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int lnx_ep_add(lnx_t *l, int fd, uint32_t events);
int lnx_ep_mod(lnx_t *l, int fd, uint32_t events);

int lnx_port_ref(lnx_t *l, int ifindex, lnx_port_t **out);
void lnx_port_unref(lnx_t *l, lnx_port_t *p);
void lnx_port_read(lnx_t *l, lnx_port_t *p);
int lnx_port_tx(lnx_port_t *p, const uint8_t *frame, size_t len);

typedef struct {
    int ifindex;
    int master;
    unsigned flags;
    unsigned short arptype;
    uint8_t mac[6];
    bool has_mac;
    bool is_bridge;          /* IFLA_INFO_KIND == "bridge" */
    int vlan_filtering;      /* IFLA_BR_VLAN_FILTERING: 0/1, -1 if not reported */
} lnx_link_info_t;

int lnx_nl_open(lnx_t *l);
void lnx_nl_close(lnx_t *l);
int lnx_nl_get_link(lnx_t *l, int ifindex, lnx_link_info_t *info);
int lnx_nl_bridge_filtering(lnx_t *l, int master);
void lnx_nl_link_events(lnx_t *l);
int lnx_nl_vlan(lnx_t *l, int ifindex, const erps_cli_ring_extra_t *x, bool add);
int lnx_nl_flush(lnx_t *l, const lnx_inst_t *inst);

#endif
