# G.8032 (03/2020) conformance — single ring

For each clause: where it is implemented and which tests exercise it. "Out of scope" items belong to the
interconnection (multi-ring / sub-ring) part of the Recommendation, which v1 does not implement.

Test files:
- unit: `tests/unit/test_fsm.c` (FSM), `test_core.c` (CORE), `test_pdu.c` (PDU), `test_audit.c` (AUDIT),
  `test_cli.c`, `test_mgr.c`
- scenario: `tests/scenario/test_app3.c` (APP3), `test_switch.c` (SW), `test_misc.c` (MISC), `test_smoke.c`
- chaos: `tests/chaos/test_chaos.c` (CHAOS)
- fuzz: `tests/fuzz/`

## Clause matrix

| Clause | Requirement | Implementation | Tests |
|---|---|---|---|
| 7.1 | SF / OK per ring link, monitoring method agnostic | `erps_set_signal_fail` (erps.c) | CORE, APP3 |
| 7.3 | < 50 ms switching (no congestion, < 16 nodes, < 1200 km) | Immediate Tx on change, 3-message burst at 3.33 ms, row actions synchronous with input | sim timestamps (APP3: protection completes within ~1 ms of SF in the sim) |
| 8 | SF, NR; FS, MS, Clear commands | `erps_command`, priority.c | CORE, SW |
| 9.1 | revertive / non-revertive | `revertive`; rows 20/29/30/36/43/44/57 | APP3 2/3, SW, AUDIT3 |
| 9.2 | protection switching triggers | priority.c `evaluate()` | CORE |
| 9.4 | traffic channel blocking per instance VIDs | `set_port_state` contract (`ERPS_BLOCK_TRAFFIC`) | FSM, Linux adapter |
| 9.5 | R-APS channel blocking; Rx delivered to control even when blocked; local Tx on both ports; ring-ID filtering | `ERPS_BLOCK_RAPS`, `sw_forward()` (erps.c), tx.c `send_both` | CORE `rx_pdu_api_and_sw_forwarding`, AUDIT9 |
| 9.6 | FDB flush per instance VIDs | `flush_fdb` contract, flush.c coalescing | CORE, APP3 1/5 |
| 9.7 | interconnection, sub-rings | **out of scope** | — |
| 10 | loop avoidance: unblock only if another block remains | the whole state machine; tx held until blocks confirmed (AUDIT1) | CHAOS P1 (300k seeds, no loop ever), sim invariant in every scenario |
| 10.1 | RPL owner / neighbour MI; not both | `role`, `rpl_port` (config.c) | FSM row 1 |
| 10.1.1 | priority logic, Table 10-1; local SF ignored in FS; local clear SF vs other port; R-APS not stored; own node ID ignored; ring ID 1..239 | priority.c, `erps_req_type_t` order | CORE `prio_*`, FSM |
| 10.1.2 | Table 10-2, all 71 rows; actions a)–r); NodeState / PortState | fsm.c `fsm_table` (row numbers), `erps_get_status` | FSM (every row and both branches; `all_71_rows_reachable`) |
| 10.1.3 | Tx: burst of 3 on change, then every 5 s; Stop Tx; event messages parallel, burst only | tx.c | CORE `tx_*`, AUDIT7 |
| 10.1.4 | WTR 1–12 min (default 5); WTB = guard + 5 s; start does not restart; stop resets, no Expires; Clear stops | fsm.c `start_wtr/stop_wtr/...`, config.c | CORE `timer_*`, SW 7/9 |
| 10.1.5 | guard 10 ms–2 s (default 500 ms); blocks R-APS except Event | erps.c `rx_common`, fsm.c `start_guard` | CORE `timer_guard_*`, MISC guard race |
| 10.1.6 | validity check: request/state values, ring ID; Event+0000+0x00 = flush indication (10 ms) | pdu.c `erps_pdu_decode`, erps.c `rx_common` | PDU, CORE `rx_validity_drops`, AUDIT11 |
| 10.1.7 | local defect logic | erps.c `declare_sf/clear_sf` | CORE |
| 10.1.8 | hold-off 0–10 s (default 0), report if still present | erps.c `start_holdoff_or_declare`, `fire_one_timer` | CORE `timer_holdoff`, MISC hold-off |
| 10.1.9 | local priority logic: Clear validity; command forgotten when overridden | priority.c `erps__prio_command`, fsm.c `set_state` latch clearing | CORE `cmd_*`, SW 8/11/12 |
| 10.1.10 | flush logic: (node ID, BPR) pairs, NR deletes, DNF / own ID, clear on block | flush.c | CORE `flush_logic_*`, APP3 1/5 |
| 10.1.11 | interconnection flush logic | **out of scope** | — |
| 10.1.12 | topology change propagation | **out of scope** | — |
| 10.1.13 | compatible version 1/2: FS/MS filtered, revertive forced | priority.c, `erps__revertive()` | CORE `cmd_fs_rules_and_compat_v1`, SW 13 |
| 10.1.14 | R-APS block logic | single ring: traffic and R-APS channels always blocked together; sub-ring branch out of scope | FSM |
| 10.2.1 | link SF behaviour (detecting and accepting nodes) | rows 5/7/19/… | APP3 1, MISC |
| 10.2.2 | recovery; revertive (WTR) / non-revertive (Clear at owner) | rows 20/29/66/58/70/71 | APP3 2/3/4 |
| 10.2.3 | MS: rules 1–3, clearing, WTB reversion | rows 9/30/36/43/65; reject on rows 23/37/51 | SW 7/8/11, CORE `cmd_ms_rules` |
| 10.2.4 | FS: multiple FS allowed, clearing, WTB reversion | rows 3/17/31/45/59/44/57 | SW 9/10, CORE |
| 10.3 | R-APS PDU: OpCode 40, Version 1, Flags 0, TLV offset 32, R-APS info, End TLV, DA with ring ID | pdu.c | PDU (golden vector, round trip, every drop), fuzz_pdu |
| 10.4 | FOP-PM (owner receives foreign NR,RB); FOP-TO (no R-APS for K cycles) | fop.c | CORE `fop_*`, MISC FOP-PM |
| App. III | scenarios A–D | — | APP3 (all, incl. DNF / no-flush of scenario C) |
| App. IV | timers, guard against outdated R-APS | — | MISC guard race, CHAOS |
| App. VIII | flush optimisation; Table VIII.1 row 1 (WTB at start-up) | flush optimisation **not implemented** (non-normative); row 1 variant available as `init_wtb` | `review_cold_start_wtr_vs_init_wtb`, CHAOS |

## Interpretations

The Recommendation leaves these points open. Each choice and its reason (also in `docs/spec/NOTES.md` §10):

1. **When the state machine runs.** It runs for every local event and for every R-APS that passes the validity
   check and the guard timer and outranks the persistent local requests. A persistent local request (latched
   FS/MS, SF, WTR/WTB running) that outranks a received R-APS, and has already been signalled, does not re-run its
   row. Re-running row 19 on every received R-APS(SF) would turn a stable (SF) into (SF,DNF), which contradicts
   Appendix III. The R-APS is counted as `rx_suppressed`.
2. **Command latches.** FS/MS are latched while the node is in state D/C and are forgotten when it leaves (§10.1.9).
   An MS that would hit a no-action row (23/37/51) is rejected with a notification rather than latched.
3. **Re-evaluation.** Persistent requests are re-evaluated after every state change. For example, an SF ignored in
   state D becomes the top request after Clear (row 44 → row 61).
4. **Dual SF.** Two failed ports are one "failed ring port" set.
5. **BPR.** BPR is the port the row is about if that port is blocked; otherwise the single blocked port; otherwise 0.
6. **Tx Version** follows `compatible_version`: 0x00 for 1, 0x01 for 2. `tx_version` overrides it.
   - §10.3 says this edition transmits 0x01. The compatibility MI exists so that 2008-only nodes, which send and
     expect version 0, can share the ring. Set `tx_version = 1` to follow §10.3 literally.
7. **Receive leniency.** Any Version is accepted, Flags and the reserved fields are ignored, and TLV offset ≥ 32 is
   accepted. An End TLV is not required: only the 32 octets of R-APS information are read. This keeps
   forward-compatibility, as G.8013 asks of receivers.
8. **Clear at the RPL owner** is rejected in states C/D without a local command.
   - These states exist only while an MS/FS is in effect. Otherwise a periodic R-APS(SF) on no-op row 49 would make
     Clear "valid", row 44 would leave the owner Pending without WTB, and the ring would never revert.
   - The chaos test found this.
9. **Software R-APS forwarding** forwards like a hardware R-APS_FF: independently of the local validity check,
   subject to MEG-level filtering (MEL ≥ ring MEL) and to channel blocking. Own frames are never forwarded.
10. **Fail-safe dataplane.** New R-APS information is not transmitted until the platform confirms every block the
    state machine asked for. A failed block is retried every 100 ms. Advertising (NR,RB) or (SF) while the block is
    not in place could let a peer unblock and close a loop.
11. **FOP-TO is off by default.**
    - With an RPL neighbour configured, the owner's (NR,RB) is stopped at both RPL ends, so in Idle the ports facing
      the RPL legitimately receive nothing.
    - §10.4 excludes "when both ends are RB", but the detailed rule is in G.8021.
    - When enabled, FOP-TO is suppressed for SF ports and for the RPL port of the owner/neighbour in Idle.
12. **Restart (row 1)** stops the guard timer, per the table. A node must therefore stay down longer than the
    ring's R-APS propagation time before restarting, as any real reboot does. The simulator and chaos test model
    this.
13. **Row 71 applies to every node in Pending**, including the RPL owner and neighbour. The table
    gives row 71 no role condition, unlike row 15, and Appendix III scenario C step E has the owner
    unblock on a higher node ID.
    - The node with the highest node ID therefore keeps the only block. A second independent
      review proposed restricting row 71 to non-RPL nodes. That would keep the owner's RPL block
      *and* the highest node's block, which segments the ring. The literal row is kept and pinned
      by `tests/scenario/test_review.c`.
14. **Cold start.** Row 1 starts WTR at a revertive owner.
    - On a ring powered up together, the owner's RPL block plus the init block of the
      highest-ID node can split the ring for the whole WTR (5 min by default). This happens when
      that node is not the owner.
    - `init_wtb` (CLI `init-timer wtb`) starts WTB instead, as in Appendix VIII Table VIII.1 row 1,
      so convergence takes guard + 5 s. It is off by default, to stay with normative Table 10-2.
    - A single node rebooting in a running ring is unaffected.
15. **Clear at the owner in Idle or Protection** is accepted and runs no-action rows 2 / 16. Those
    rows exist for exactly this input (§10.1.9 b).

## Known limitations

- Sub-rings and interconnection (§9.7, §10.1.11, §10.1.12, sub-ring R-APS block logic) are not implemented.
- Appendix VIII flush optimisation is not implemented (non-normative); only its row 1 start-up timer is offered, as `init_wtb`.
- FOP-TO approximates G.8021's dFOP-TO (see interpretation 11).
- Performance (§7.3, < 50 ms) depends on the platform's SF detection, CPU trap latency and timer resolution. The
  library's processing is synchronous and O(1) per input.
