/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Linux platform for liberps (used by apps/erpsd).
 *
 * Dataplane model: each ring port is a netdev enslaved to a Linux bridge with vlan_filtering=1.
 *   - Traffic channel blocking (§9.4) = removing the instance's protected VIDs from the bridge
 *     port's VLAN membership (VID filtering); unblocking re-adds them as tagged members.
 *   - The R-APS VLAN is NOT configured on the bridge: R-APS frames are received on each ring
 *     port with an AF_PACKET socket (+ BPF filter, VLAN tag recovered from PACKET_AUXDATA) and
 *     forwarded between ring ports by the core's software R-APS_FF (sw_raps_forwarding is
 *     forced on by instance_bind).
 *   - FDB flush (§9.6) = netlink bulk delete of dynamic entries per ring port and protected VID
 *     (RTM_DELNEIGH + NLM_F_BULK), with a dump-and-delete fallback for older kernels.
 *   - Signal fail = !(IFF_UP && IFF_LOWER_UP) of the ring port, tracked via RTNLGRP_LINK.
 * Event loop: epoll over the packet sockets, the rtnetlink socket, a timerfd armed to the
 * earliest erps deadline, a signalfd and the control socket (one command line per request).
 */
#ifndef ERPS_PLATFORM_LINUX_H
#define ERPS_PLATFORM_LINUX_H

#include "erps/erps.h"
#include "erps/erps_cli.h"

#include <stdio.h>

typedef struct lnx lnx_t;

typedef struct {
    const char *ctl_path;        /* control socket path (default "/run/erpsd.sock") */
    erps_log_level_t log_level;  /* default ERPS_LOG_NOTICE */
    FILE *log;                   /* default stderr */
    bool block_on_exit;          /* block ring ports of enabled rings on exit (default true) */
} lnx_opts_t;

void lnx_opts_init(lnx_opts_t *o);

/* Create the platform: rtnetlink socket, epoll, control socket. NULL on failure (logged). */
lnx_t *lnx_create(const lnx_opts_t *o);

/* The command layer bound to this platform (port names = netdev names). */
erps_cli_t *lnx_cli(lnx_t *l);

/* Run the event loop until SIGINT/SIGTERM. Returns a process exit code. */
int lnx_run(lnx_t *l);

void lnx_destroy(lnx_t *l);

/* CLOCK_MONOTONIC in microseconds. */
erps_time_t lnx_now(void);

/* Control protocol (erpsd <-> erpsctl): the client writes one command line terminated by '\n';
 * the server writes the command output followed by a trailer line "\x1e<rc>\n" (ASCII RS, the
 * decimal erps_err_t). A connection may carry several requests. */
#define LNX_CTL_TRAILER '\x1e'

#endif /* ERPS_PLATFORM_LINUX_H */
