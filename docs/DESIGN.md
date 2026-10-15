# liberps design

## Goals

1. **Correct by construction against G.8032.** The state machine is Table 10-2, transcribed with the
   Recommendation's row numbers, so every behaviour can be traced to a row.
2. **Pluggable.** The protocol core has no knowledge of the OS, the clock or the dataplane. The same objects run
   in a switch NOS, in `erpsd` on Linux, in the simulator and in unit tests.
3. **Deterministic and testable.** Time is an input. Given the same inputs, the library produces the same outputs.
   This is what makes property testing over 300k random ring histories practical.
4. **Debuggable in the field.** Every decision is observable: row number, triggering request, actions, counters,
   drop reasons, JSON dumps.

## Layers

```
 apps/erpsd, apps/erps-sim, host NOS CLI
        │ text commands
 src/cli/cli.c ─────────────── erps_cli: config model, enable/disable, show, running-config
        │ erps_mgr / erps_t API
 src/core/ ─────────────────── protocol (pure C11, no OS)
        │ erps_ops_t: tx, set_port_state, flush_fdb, notify, log
 src/platform/sim | linux ──── dataplane, I/O, clock, event loop
```

## Core data flow (Figure 10-1)

```
 erps_rx_frame ─► Ethernet/VID/EtherType ─► R-APS DA ─► MEL ≥ ring MEL ─► [software R-APS_FF]
                                                                 │
                                    validity check (ring ID, MEL, opcode, TLV offset, request)
                                                                 │
              ┌──── Event flush indication (10 ms) ──────────────┤
              ▼                                                  ▼
          flush logic ◄──────────────────────── (node ID, BPR) pairs, every valid message
                                                                 │
                                                own node ID? ─► ignored
                                                guard running? ─► discarded (counted)
                                                                 ▼
 erps_set_signal_fail ─► hold-off ─► local SF ─────────► PRIORITY LOGIC ◄─── WTR / WTB running, expires
 erps_command ─► compat filter ─► local priority logic ─►   (Table 10-1)
                                                                 │ top-priority request
                                                                 ▼
                                               R-APS REQUEST PROCESSING (Table 10-2)
                                         block/unblock │ Tx R-APS │ timers │ flush
                                                       ▼
                     set_port_state ◄─ port_set   tx ◄─ tx scheduler   flush_fdb ◄─ coalesced at end of input
```

## Key decisions

**Table-driven state machine (fsm.c).**
- The table is `fsm_table[state][request] = {row, action function, next state}`.
- Action functions are named after the action lists they implement (`fs_switch`, `local_sf`, `owner_revert`, …).
  Rows sharing an action list share the function.
- The primitives mirror spec actions a)–r): `block_failed`, `unblock_non_rpl`, `tx(code, rb, dnf)`, …
- Conditionals are written exactly as the table nests them: "if requested ring port is already blocked", "if RPL
  owner node and revertive".
- A unit test re-derives the (state, request) → row mapping independently and checks it against the table.

**Persistent requests versus events (priority.c).** The priority logic separates two kinds of input:
- Conditions that hold until cleared: latched FS/MS, local SF, WTR/WTB Running.
- One-shot events: commands, local clear SF, Expires signals, R-APS messages.

The top request is max(event, persistent), ordered by Table 10-1. Two refinements keep it faithful to the spec's
model without rerunning rows needlessly:
- An R-APS that loses to an already-signalled persistent request does not re-run its row.
- Persistent requests are re-evaluated after each state change.

**Caller-supplied time.**
- Every input carries `now`. Deadlines are absolute and saturating.
- The host calls `erps_poll` at `erps_next_deadline()`.
- Timer firing order within a poll is by deadline.
- There is no timer wheel, thread or clock dependency. The simulator therefore runs hours of ring time in
  milliseconds, and tests are exact to the microsecond.

**Fail-safe dataplane.**
- The state machine records the port state it wants; `hw_synced` records whether the platform confirmed it.
- New R-APS information is held until all requested blocks are confirmed: peers unblock in response to it.
- Failed blocks and failed flushes are retried every 100 ms.
- `erps_stop(…, true)` blocks both ports and reports a block that failed.

**Coalesced flushes.** The flush requests produced while processing one input (a row's "Flush FDB" plus the flush
logic's) become a single `flush_fdb()` call. It is issued after all port changes for that input, so relearning
happens on the new topology.

**Debuggability.**
- Each row execution produces an `erps_trace_t` in a ring buffer. It holds the time, the request (node ID, BPR,
  receiving port), the transition, the row, the actions in execution order, and the port state before and after.
- Repeated no-op rows (for example a periodic R-APS(SF) on row 21) fold into one entry with a repeat count. The
  buffer therefore keeps real transitions even after hours in Protection.
- Every dropped frame increments a reason counter.

**Re-entrancy safety.**
- Callbacks run inside the call that caused them.
- A nested call into the same instance returns `ERPS_E_BUSY`.
- `erps_destroy` from a callback is deferred until the outer call unwinds.

**Memory.**
- One instance is about 11 KB. Most of that is the 128-entry history; `ERPS_HISTORY_MAX` sets the size.
- Nothing is allocated after `erps_create`. `erps_init_static` allows fully static allocation.

## Simulator (src/platform/sim)

- **Model.** A discrete-event simulation: a heap of frame deliveries and SSF changes, with each node polled at its
  own deadline.
- **Links.** Per-direction state, delay, jitter (FIFO preserved) and loss.
- **Node failure.** Power-off semantics: frames already on the wire are still delivered.
- **Invariant.** After every simulated instant the checker looks for a forwarding loop in each direction: every
  link up in that direction, with both end ports unblocked on live nodes. A violation is counted and described.
- **pcap.** Every transmitted frame can be captured; Wireshark decodes R-APS.

## Linux platform (src/platform/linux)

See [LINUX.md](LINUX.md). In summary:
- **Ring ports** are bridge ports.
- **Blocking** is VID membership: the protected VIDs are removed from or added to the port.
- **The R-APS VLAN** is not bridged. AF_PACKET with a BPF filter receives it, and the core forwards it in software.
- **FDB flush** uses netlink bulk delete.
- **Signal fail** comes from the carrier state via rtnetlink.
- **The event loop** is epoll with a timerfd.
- **Control** is a Unix socket carrying the command layer.

## Extension points (v2)

- **Sub-rings / interconnection.**
  - `ERPS_BLOCK_TRAFFIC` and `ERPS_BLOCK_RAPS` are already separate bits for the §10.1.14 R-APS block logic.
  - The flush logic would need its "sub-ring without virtual channel" variant.
  - A Topology_Change output and an interconnection flush input would sit on `erps_mgr`.
- **Appendix VIII flush optimisation.** It needs an RPL next-neighbour port in the config, a DNF status bit, and a
  per-port Tx mask in tx.c.
- **CCM-based SF.** Any CFM implementation feeds `erps_set_signal_fail`.
