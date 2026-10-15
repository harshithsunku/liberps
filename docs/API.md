# liberps API guide

This guide covers integrating liberps with a dataplane: a switch SDK, a Linux bridge, or anything else.
The headers are the reference: `include/erps/erps.h`, `erps_pdu.h`, `erps_mgr.h`, `erps_cli.h` and `erps_sim.h`.
[examples/two_nodes.c](../examples/two_nodes.c) is a complete integration in about 200 lines.

## 1. Mental model

One `erps_t` is one **ERP control process**: one ERP instance on one node (G.8032 Figure 10-1).

You give it:
- the R-APS frames received on its two ring ports;
- the signal-fail state of those ports (ETH_CI_SSF: carrier, CCM loss, …);
- operator commands (FS, MS, Clear);
- the current time, as `erps_time_t`. This is a monotonic clock in microseconds whose origin is arbitrary, and you
  pass it with every call.

In return it calls you back:

| Callback | When | What the platform does |
|---|---|---|
| `tx(user, port, frame, len)` | the instance sends R-APS | transmit the complete tagged Ethernet frame (no FCS) on the ring port |
| `set_port_state(user, port, mask)` | a ring port is blocked or unblocked | `mask` = `ERPS_BLOCK_TRAFFIC\|ERPS_BLOCK_RAPS`, or 0 to forward: remove/add the instance's protected VIDs on that port (§9.4) and stop forwarding the R-APS VID through it (§9.5) |
| `flush_fdb(user)` | topology changed | flush MACs learned on both ring ports, for this instance's VIDs only (§9.6) |
| `notify(user, ev)` *(optional)* | state change, port change, flush, FOP-PM/TO, rejected command, signal fail, platform error | alarms, SNMP traps, logs |
| `log(user, level, msg)` *(optional)* | diagnostics | your logger; the level is set per instance |

**Return codes.** A callback returns 0 on success.
- A failed `set_port_state` is retried every 100 ms. Until the block is confirmed, the instance holds back any new
  R-APS information, because peers would unblock in response to it.
- A failed `flush_fdb` is retried.
- A failed `tx` is counted; periodic retransmission covers it.

## 2. Lifecycle

```c
erps_config_t cfg;
erps_config_init(&cfg);                 /* G.8032 defaults (WTR 5 min, guard 500 ms, ...) */
snprintf(cfg.name, sizeof cfg.name, "R1");
cfg.ring_id  = 1;                       /* DA 01-19-A7-00-00-01 */
cfg.raps_vid = 100;
cfg.mel      = 7;
memcpy(cfg.node_id, system_mac, 6);
cfg.role     = ERPS_ROLE_RPL_OWNER;     /* or _RPL_NEIGHBOUR / _NONE */
cfg.rpl_port = ERPS_PORT1;

char why[160];
if (erps_config_validate(&cfg, why, sizeof why) != ERPS_OK) { puts(why); ... }

erps_t *e;
erps_create(&cfg, &ops, platform_ctx, &e);  /* or erps_init_static(mem, size, ...) */
erps_start(e, now());                       /* Table 10-2 row 1: ports programmed, Tx starts */
...
erps_stop(e, now(), true);                  /* stop, leaving both ring ports blocked */
erps_destroy(e);
```

**Timer ranges.** These are validated strictly by default:
- WTR: 1–12 min, in 1 min steps
- guard: 10 ms–2 s, in 10 ms steps
- hold-off: 0–10 s, in 100 ms steps
- R-APS timing: a burst of three ≤ 3.33 ms apart, then one message every 5 s

`relaxed_timers = true` accepts lab values, such as a 10 s WTR.

**Runtime changes:**
- `erps_set_timers()` changes the timers.
- `erps_set_revertive()` changes the mode. Going non-revertive cancels a pending WTR/WTB.
- `erps_set_log_level()` changes the log level.
- Any other change needs stop, destroy and create. The command layer does this for you.

## 3. Driving it

```c
/* R-APS received on ring port p (full frame with the 802.1Q tag) */
erps_rx_frame(e, p, frame, len, now());

/* ...or, if your platform strips the tag / parses the header itself */
erps_rx_meta_t m = { .vid = vid, .pcp = pcp };   /* + dst/src MAC */
erps_rx_pdu(e, p, &m, pdu_starting_at_mel, pdu_len, now());

/* link / CCM state of ring port p: call on every change (repeats are harmless) */
erps_set_signal_fail(e, p, true_if_failed, now());

/* operator */
erps_reject_t why;
if (erps_command(e, ERPS_CMD_MANUAL_SWITCH, ERPS_PORT0, now(), &why) == ERPS_E_REJECTED)
    printf("rejected: %s\n", erps_reject_str(why));

/* timers: arm your timer to erps_next_deadline(e) and call erps_poll(e, now()) when it fires */
```

`erps_rx_frame()` returns `ERPS_OK` when the frame was accepted as an R-APS for this instance, and
`ERPS_E_DROPPED` when the validity check discarded it. The reason is counted in `erps_counters_t.port[p].rx_drop[]`.

**R-APS forwarding (R-APS_FF).** There are two modes:
- **Hardware forwards R-APS** (`sw_raps_forwarding = false`, the default). The ASIC forwards the R-APS VLAN between
  the ring ports, and also copies frames to the CPU. `set_port_state` must also remove the port from the R-APS
  VLAN's forwarding when blocking it.
- **Software forwards** (`sw_raps_forwarding = true`). The R-APS VLAN is not bridged at all. Each received frame
  is retransmitted by the instance on the other ring port when neither port is blocked, its MEG level is ≥ the ring
  MEL, and it did not originate here. The Linux adapter and the simulator use this mode.

**Several instances.**
- Use `erps_mgr`. `erps_mgr_rx_frame(mgr, port_handle, frame, len, now)` demultiplexes a frame by port handle,
  R-APS VID and ring ID.
- `erps_mgr_set_signal_fail` fans a port's state out to every instance on that port.
- `erps_mgr_next_deadline` / `erps_mgr_poll` drive all of them.

## 4. Threading and re-entrancy

- An instance is **not thread-safe**. Serialise all calls on it: one event loop, or a lock per instance.
  Distinct instances are independent.
- Callbacks run synchronously inside the call that triggered them. A callback must not call into the **same**
  instance; doing so returns `ERPS_E_BUSY`.
- `erps_destroy()` may be called from a callback: the release is deferred until the outer call returns.
- The library never blocks, sleeps, allocates after creation (with `erps_init_static` it never allocates at all)
  or reads a clock.
- A `now` earlier than a previous call is clamped and counted (`clock_backwards`).

## 5. Observability

```c
erps_status_t st;   erps_get_status(e, &st);     /* state, ports, top request, timers, Tx, FOP  */
erps_counters_t c;  erps_get_counters(e, &c);    /* per port/kind Rx/Tx, drops by reason, ...   */
erps_trace_t h[32]; size_t n = erps_get_history(e, h, 32);  /* last Table 10-2 rows executed     */
char buf[8192];     erps_dump(e, ERPS_DUMP_ALL, ERPS_FMT_JSON, buf, sizeof buf);
```

A history entry records the time, the triggering request (with the R-APS node ID, BPR and receiving port), the
state transition, the **Table 10-2 row number**, the actions in order and the port states before and after:

```
#9 t=302.001150 B->E row 29 R-APS(NR) 02:00:00:00:00:59 bpr1 @p1 [start-wtr] ports --->--
```

At log level `debug` the same line is also logged as each row runs.

`erps_get_row_coverage()` returns which of the 71 rows have run. It is useful for conformance testing on a target.

## 6. Command layer

`erps_cli` is a ready-made, transport-agnostic command interpreter for config, operator commands and show. It
supports text and JSON output and `show running-config`. A host provides:

```c
erps_cli_platform_t p = {
    .ctx = my,
    .now = my_now,                     /* required */
    .port_lookup = my_port_lookup,     /* "eth1" -> handle (+ MAC) */
    .system_mac = my_system_mac,       /* for node-id auto */
    .instance_bind = my_bind,          /* fill erps_ops_t + user for a ring being enabled */
    .instance_unbind = my_unbind,
};
erps_cli_t *cli = erps_cli_create(&p);
erps_cli_exec(cli, "erps ring R1 ring-id 1 raps-vid 100 port0 eth1 port1 eth2", out, ctx);
erps_cli_exec(cli, "erps ring R1 enable", out, ctx);
erps_cli_exec(cli, "show erps ring R1 detail json", out, ctx);
```

- `erps_cli_commands()` returns the grammar, one syntax and help string per command, so a NOS CLI framework can
  generate its own parser and completion.
- `erps_cli_attach()` exposes instances you created yourself to `show` and to the operator commands.

## 7. Porting checklist

1. **R-APS reception.** Trap frames with DA 01-19-A7-00-00-xx and EtherType 0x8902 on the R-APS VLAN to the CPU,
   per ring port. Keep the tag, or use `erps_rx_pdu`.
2. **Transmission.** Send the frames from `tx()` unchanged: they are tagged, with PCP 7 by default, and padded to
   60 octets.
3. **Blocking.** Implement it per instance: protected VIDs plus the R-APS VID. Do it synchronously; return non-zero
   if the hardware rejected it.
4. **FDB flush.** Flush per instance, ring ports only, protected VIDs only.
5. **Signal fail.** Feed SF from the fastest reliable source: CCM (G.8021 ETH trail SF) or carrier. Use `holdoff_ms`
   if a lower layer protects the link.
6. **Timers.** Arm a timer to `erps_next_deadline()`. The resolution should be ≤ 1 ms for the 3.33 ms burst.
7. **Node ID.** Use a MAC unique to the node, the same for all instances.
8. **Validation.** Run `tests/` on the target compiler, and check `erps_get_row_coverage()` after your system tests.
