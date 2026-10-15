# liberps — ITU-T G.8032 Ethernet Ring Protection Switching in C

liberps implements **ITU-T G.8032/Y.1344 (03/2020) ERPS for a single ring** as a small, portable C11
library. It is a deterministic state machine: you feed it frames, link state and operator commands, and it
tells your dataplane what to do through four callbacks. It performs no I/O, reads no clock and spawns no threads,
so it drops into a switch NOS, a Linux daemon or a simulator unchanged.

```
 operator / CLI / NOS ──► erps_cli (embeddable command layer, text + JSON)
                              │
 R-APS frames, SF ──────► liberps core (G.8032 state machine, Table 10-2 by row number)
                              │ tx · set_port_state · flush_fdb · notify · log
                              ▼
          your dataplane:  ASIC SDK │ Linux bridge (erpsd) │ simulator (erps-sim)
```

## What is implemented

**State machine.** All of G.8032 (03/2020) for one major ring:
- **Table 10-2:** all 71 rows, transcribed literally with the spec's row numbers.
- **Inputs and commands:** SF, NR, NR-RB, FS, MS and Clear.
- **Modes:** revertive and non-revertive.
- **Roles:** RPL owner, RPL neighbour and ordinary node.

**Timers.** WTR, WTB (= guard + 5 s), guard and hold-off, with spec ranges and defaults.

**R-APS messaging:**
- **Transmission:** a burst of 3 at 3.33 ms, then one message every 5 s; event bursts run in parallel.
- **Validity check:** validated on receive.
- **PDU format:** the G.8013 OAM PDU, opcode 40.

**Flush logic.** (node ID, BPR) pairs, DNF, and flushes coalesced per input.

**Alarms.** FOP-PM, plus an optional FOP-TO.

**Interworking.** Compatible-version 1 (G.8032v1 interworking) and 2.

**Multiple instances.** Several ERP instances share the same ring ports, each with its own R-APS VID and VLAN set.

**R-APS forwarding:**
- **Hardware mode:** your ASIC forwards R-APS.
- **Software mode:** the library forwards between the ring ports and honours R-APS channel blocking.

**Fail-safe platform handling:**
- New R-APS information is never advertised before the platform has confirmed the port blocks it relies on.
- Failed blocks and flushes are retried.

Not in v1: sub-rings and interconnection (§9.7, §10.1.11–12), and the Appendix VIII flush optimisation.

## Quick start

```bash
make                                  # liberps.a/.so, erps-sim, erpsd, erpsctl (Linux)
make test                             # unit + scenario + chaos tests
build/cc/erps-sim -f examples/appendix3.erps   # Appendix III scenario A, live
```

```
[  301.001000] n2 Idle -> Protection (row 5, local-SF)
[  301.001000] n3 Idle -> Protection (row 5, local-SF)
[  301.001050] n1 Idle -> Protection (row 7, R-APS(SF))
[  301.001150] n6 port 1 unblocked                       <- RPL owner opens the RPL
...
t=608.000000 n0:A[B-] n1:A[--] n2:A[--] n3:A[--] n4:A[--] n5:A[--] n6:A[-B]
```

To use the shell interactively, run `build/cc/erps-sim`, then for example:
- `sim create 5`
- `run 6m`
- `link 2 down`
- `run 1s`
- `show erps ring n4 detail`
- `pcap ring.pcap`

Wireshark decodes the capture natively.

## Embedding the library

```c
#include <erps/erps.h>

static const erps_ops_t ops = {
    .tx = my_tx,                         /* send frame on ring port 0/1          */
    .set_port_state = my_block,          /* block/unblock the instance's VLANs   */
    .flush_fdb = my_flush,               /* flush MACs learned on the ring ports */
    .notify = my_alarm, .log = my_log,   /* optional                             */
};

erps_config_t cfg;
erps_config_init(&cfg);                  /* spec defaults                        */
cfg.raps_vid = 100;  memcpy(cfg.node_id, sys_mac, 6);
cfg.role = ERPS_ROLE_RPL_OWNER;  cfg.rpl_port = ERPS_PORT1;

erps_t *e;
erps_create(&cfg, &ops, my_ctx, &e);
erps_start(e, now_us());

/* event loop */
erps_rx_frame(e, port, frame, len, now_us());          /* R-APS received       */
erps_set_signal_fail(e, port, link_down, now_us());    /* link / CCM state     */
erps_command(e, ERPS_CMD_FORCED_SWITCH, ERPS_PORT0, now_us(), NULL);
if (now_us() >= erps_next_deadline(e)) erps_poll(e, now_us());
```

**Contract:**
- An instance is single-threaded: serialise all calls to it.
- Callbacks must not re-enter their own instance. Re-entry is detected and refused, and `erps_destroy` called from
  a callback is deferred safely.
- No-malloc builds are supported through `erps_init_static()`.

The full walkthrough is in [docs/API.md](docs/API.md), and [examples/two_nodes.c](examples/two_nodes.c) is a
runnable integration.

## Command layer (for any CLI)

```
erps ring R1 ring-id 1 raps-vid 100 port0 eth1 port1 eth2
erps ring R1 role owner rpl-port 1
erps ring R1 timers wtr 5m guard 500ms holdoff 0
erps ring R1 protected-vlans 10-20,30
erps ring R1 enable
erps ring R1 force-switch port 0
show erps
show erps ring R1 detail json
show running-config erps
```

`erps_cli_exec(cli, line, out, ctx)` is transport-agnostic. `erps_cli_commands()` exports the grammar for host CLI
frameworks. A configuration file is simply a command script.

## Debugging

- **Transition history.** Every executed Table 10-2 row is recorded with its time, the input that triggered it, the
  row number, the actions it took and the port states. Repeated no-op rows are folded into one entry:
  ```
  #4 t=301.001150 A->B row 7 R-APS(SF) 02:..:3e bpr0 @p0 [unblock-p1 stop-tx] ports -B->--
  #5 t=301.004483 B->B row 21 R-APS(SF) 02:..:59 bpr1 @p1 [] ports --->-- (x3 more, last t=301.007816)
  ```
- **Counters.** Per port and per message kind, plus drops by validity reason (ring-ID, VID, MEL, opcode, reserved
  request…), guard discards, suppressed requests, flushes and platform errors.
- **Dumps.** Text and JSON (`erps_dump`, `show erps ring R1 detail json`), plus a row-coverage bitmap.
- **Simulator.** A loop-invariant checker and pcap capture, deterministic per seed.

## Quality

| Check | Command |
|---|---|
| 84 unit tests: every Table 10-2 row, priority logic, timers, Tx, flush, validity, FOP, CLI, audit regressions | `make test` |
| 21 scenario tests from the spec: Appendix III A–D, MS/FS/Clear, compat-v1, node failures, guard race | `make test` |
| Chaos/property test: random rings and faults; no loop ever, convergence to Idle (300k seeds clean) | `ERPS_CHAOS_SEEDS=300000 build/gcc/test-chaos` |
| gcc + clang × {plain, ASan+UBSan}, `-Wall -Wextra -Wconversion -Werror` | `make check` |
| Line coverage of the core ≥ 95 % | `make cov` |
| libFuzzer: PDU decoder, instance Rx path, simulator event sequences, CLI | `make fuzz` |

[docs/CONFORMANCE.md](docs/CONFORMANCE.md) maps every clause to its code and tests, and lists each spec
interpretation with its rationale.

## Layout

```
include/erps/   erps.h (core API) erps_pdu.h (codec) erps_mgr.h (multi-instance) erps_cli.h erps_sim.h
src/core/       fsm.c (Table 10-2) priority.c tx.c flush.c fop.c erps.c trace.c pdu.c mgr.c config.c
src/cli/        command layer
src/platform/   sim/ (discrete-event ring simulator), linux/ (AF_PACKET + bridge netlink, erpsd)
apps/           erps-sim, erpsd, erpsctl
tests/          unit/ scenario/ chaos/ fuzz/ linux/
docs/           API.md DESIGN.md CONFORMANCE.md LINUX.md spec/NOTES.md
```

## License

Apache-2.0. G.8032 is © ITU. This repository contains only original notes about it, never the Recommendation's
text.
