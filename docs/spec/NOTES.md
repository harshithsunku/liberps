# G.8032 (03/2020) — implementer's notes for liberps (single ring)

These are our own notes, written to implement ITU-T G.8032/Y.1344 (03/2020). They paraphrase the Recommendation and
cite its clause numbers. The authoritative text is the ITU-T publication. The full text and the figures sit locally
under `docs/spec/` (gitignored, © ITU).

Scope of liberps v1: **one major ring**. These parts are not implemented:
- interconnection, sub-rings and R-APS virtual channels (§9.7)
- interconnection flush logic (§10.1.11) and topology-change propagation (§10.1.12)
- the sub-ring branch of R-APS block logic (§10.1.14)
- Appendix VIII (flush optimisation, non-normative)

## 1. Model (§9, §10.1, Fig. 10-1)

- A node has two ring ports, `0` and `1`. An **ERP instance** protects one set of VLANs and uses one R-APS VLAN.
  Instances are independent.
- **Blocking a port** (§9.4/§9.5) removes it from forwarding, for the instance's traffic VIDs *and* its R-APS VID.
  - A blocked port still delivers received R-APS to the control process (§9.5c).
  - Locally generated R-APS is still transmitted on both ports, blocked or not (§9.5b).
  - Single ring: the traffic channel and the R-APS channel are always blocked together.
- **Loop-avoidance rule** (§10): a blocked ring port may be unblocked only if at least one other blocked ring port is
  known to remain in the ring.
- **FDB flush** (§9.6): remove MACs learned on the ring ports, for this instance's VIDs only.
- **Fig. 10-1 data flow:**
  - Rx R-APS → **validity check** → (a) **flush logic**, which sees every valid message, before the guard timer, and
    (b) **guard timer** → **priority logic**.
  - Local SSF per port → **local defect logic** (hold-off) → priority logic.
  - ExtCMD → backward-compatibility logic → **local priority logic** → priority logic.
  - WTR/WTB timers → priority logic (Running/Expires signals).
  - Priority logic → **R-APS request processing** (Table 10-2), which drives Tx, port blocking, flushes, the timers,
    NodeState and PortState.

## 2. Management information (MI), ranges and defaults

| MI | Meaning | Range / default | § |
|---|---|---|---|
| RAPS_RingID | ring ID; last DA octet 01-19-A7-00-00-**ID** | 1..239, default 1 | 10.1.1, 10.3 |
| R-APS VID | dedicated R-APS VLAN | 1..4094 | 9.5, 10.1.1 |
| MEL | ring MEG level | 0..7 | 10.3 |
| RPL_Owner_Node | is owner + which port is RPL | off | 10.1 |
| RPL_Neighbour_Node | is neighbour + which port is RPL; never both owner and neighbour | off | 10.1 |
| Revertive | revertive mode | default **true** | 10.1.13 |
| Compatible_Version | 1 or 2 | default **2** | 10.1.13 |
| WTR | wait-to-restore | 1..12 min, 1-min steps, default **5 min** | 10.1.4 |
| GuardTime | guard timer | 10 ms..2 s, 10-ms steps, default **500 ms** | 10.1.5 |
| WTB | wait-to-block | **guard + 5 s** (not configurable) | 10.1.4 |
| HoTime | hold-off | 0..10 s, 100-ms steps (±5 ms), default **0** | 10.1.8 |
| ExtCMD | FS / MS / Clear on a port | — | 8 |
| NodeState (read) | `–`, Idle, Protection, Manual switch, Forced switch, Pending | — | 10.1.2 |
| PortState[0..1] (read) | Blocked / Unblocked | — | 10.1.2 |

**Ring-ID/VID rules (§10.1.1):**
- Every instance in the network is uniquely identified by its (ring ID, R-APS VID) pair.
- Instances on the same physical ring must use different R-APS VIDs.

**Compatible version (§10.1.13):**
- **v1:** MS and FS are filtered and revertive is forced on.
- **v2:** MS and FS pass, and revertive follows the MI.

## 3. Priority (Table 10-1), highest → lowest

1. Clear (local)
2. FS (local)
3. R-APS(FS)
4. local SF ¹
5. local clear SF
6. R-APS(SF)
7. R-APS(MS)
8. MS (local)
9. WTR Expires
10. WTR Running
11. WTB Expires
12. WTB Running
13. R-APS(NR,RB)
14. R-APS(NR)

¹ Local SF is ignored while the node is in state FS.

**Priority logic (§10.1.1):**
- An SF or a command stays the top request until something higher arrives, or until its matching clear (Clear for
  FS/MS, local clear SF for SF).
- Local clear SF becomes top only if nothing higher is pending, such as SF or FS still present on the other port.
- Received R-APS are *not stored*.
- R-APS messages carrying our own node ID are ignored by the priority logic.

**Local priority logic (§10.1.9):**
- Clear is valid only when (a) a local FS or MS is in effect, or (b) the node is the RPL owner and the top request is
  neither R-APS(FS) nor R-APS(MS).
- A local command overridden by a higher top request is forgotten, without any Clear being generated.
- MS while an MS exists in the ring is rejected, and the operator is notified (§10.2.3 rule 1).
- A new FS is accepted unless this node already has a local FS; several FS in a ring are allowed (§10.2.4).

## 4. State machine (Table 10-2)

States: **A** Idle, **B** Protection, **C** Manual switch, **D** Forced switch, **E** Pending.

Each row lists its actions in execution order. "owner" means the RPL owner node; "neigh" means the RPL neighbour.

| St | Top request | Row | Actions | Next |
|---|---|---|---|---|
| – | init | 1 | stop guard, WTR, WTB; owner: block RPL, unblock non-RPL, Tx(NR), if revertive start WTR; neigh: block RPL, unblock non-RPL, Tx(NR); else: block one port, unblock other, Tx(NR) | E |
| A | Clear | 2 | — | A |
| A | FS | 3 | if requested port already blocked: Tx(FS,DNF), unblock non-requested; else: block requested, Tx(FS), unblock non-requested, flush | D |
| A | R-APS(FS) | 4 | unblock ring ports; stop Tx | D |
| A | local SF | 5 | if failed port already blocked: Tx(SF,DNF), unblock non-failed; else: block failed, Tx(SF), unblock non-failed, flush | B |
| A | local clear SF | 6 | — | A |
| A | R-APS(SF) | 7 | unblock non-failed; stop Tx | B |
| A | R-APS(MS) | 8 | unblock non-failed; stop Tx | C |
| A | MS | 9 | like row 3 with MS | C |
| A | WTR Exp / WTR Run / WTB Exp / WTB Run | 10–13 | — | A |
| A | R-APS(NR,RB) | 14 | unblock non-RPL; if not owner: stop Tx | A |
| A | R-APS(NR) | 15 | if neither owner nor neigh and remote ID > own: unblock non-failed, stop Tx | A |
| B | Clear | 16 | — | B |
| B | FS | 17 | = row 3 | D |
| B | R-APS(FS) | 18 | = row 4 | D |
| B | local SF | 19 | = row 5 | B |
| B | local clear SF | 20 | start guard; Tx(NR); if owner and revertive: start WTR | E |
| B | R-APS(SF), R-APS(MS), MS, WTR*, WTB* | 21–27 | — | B |
| B | R-APS(NR,RB) | 28 | — | E |
| B | R-APS(NR) | 29 | if owner and revertive: start WTR | E |
| C | Clear | 30 | if any port blocked: start guard, Tx(NR), if owner and revertive: start WTB | E |
| C | FS | 31 | = row 3 | D |
| C | R-APS(FS) | 32 | = row 4 | D |
| C | local SF | 33 | = row 5 | B |
| C | local clear SF | 34 | — | C |
| C | R-APS(SF) | 35 | unblock non-failed; stop Tx | B |
| C | R-APS(MS) | 36 | if any port blocked: start guard, Tx(NR), if owner and revertive: start WTB | E, **or C if both ports unblocked** |
| C | MS, WTR*, WTB* | 37–41 | — | C |
| C | R-APS(NR,RB) | 42 | — | E |
| C | R-APS(NR) | 43 | if owner and revertive: start WTB | E |
| D | Clear | 44 | = row 30 | E |
| D | FS | 45 | block requested; Tx(FS); flush | D |
| D | R-APS(FS) … WTB Run | 46–55 | — | D |
| D | R-APS(NR,RB) | 56 | — | E |
| D | R-APS(NR) | 57 | if owner and revertive: start WTB | E |
| E | Clear | 58 | owner: stop WTR, WTB; if RPL blocked: Tx(NR,RB,DNF), unblock non-RPL; else: block RPL, Tx(NR,RB), unblock non-RPL, flush | A |
| E | FS | 59 | = row 3; owner: stop WTR, WTB | D |
| E | R-APS(FS) | 60 | unblock ring ports; stop Tx; owner: stop WTR, WTB | D |
| E | local SF | 61 | = row 5; owner: stop WTR, WTB | B |
| E | local clear SF | 62 | — | E |
| E | R-APS(SF) | 63 | unblock non-failed; stop Tx; owner: stop WTR, WTB | B |
| E | R-APS(MS) | 64 | unblock non-failed; stop Tx; owner: stop WTR, WTB | C |
| E | MS | 65 | owner: stop WTR, WTB; then = row 9 | C |
| E | WTR Expires | 66 | owner: stop WTB; if RPL blocked: Tx(NR,RB,DNF), unblock non-RPL; else: block RPL, Tx(NR,RB), unblock non-RPL, flush | A |
| E | WTR Running | 67 | — | E |
| E | WTB Expires | 68 | owner: stop WTR; then as row 66 | A |
| E | WTB Running | 69 | — | E |
| E | R-APS(NR,RB) | 70 | owner: stop WTR, WTB; neither: unblock ring ports, stop Tx; neigh: block RPL, unblock non-RPL, stop Tx | A |
| E | R-APS(NR) | 71 | if remote ID > own: unblock non-failed, stop Tx | E |

**Actions a)–r) (§10.1.2):**
- **Block/unblock "requested", "failed", "RPL", "one/other" and "ring ports":** each affects both the traffic and the
  R-APS channel, and a port already in the target state stays there.
- **"Unblock non-failed":** only ports without SF are unblocked.
- **"Unblock non-RPL":** every ring port except the RPL port; for a node with no RPL role, that means both ports.
- **WTR/WTB:**
  - Start does not restart a running timer.
  - Stop resets it, and no Expires signal is produced.
- **Guard timer:** Start (re)starts it; Stop cancels it.
- **Tx(msg, bits):** starts continuous transmission on both ports and replaces any previous continuous message; an
  event message is not replaced.
- **Stop Tx:** stops continuous transmission.
- **Flush FDB:** see §9.6.

## 5. Timers

- **Tx (§10.1.3):**
  - When the information to send changes, a burst of 3 messages goes out as fast as possible, at most 3.33 ms
    apart. After that, one message every **5 s**.
  - An "event" message goes out as a single burst of 3, in parallel with the continuous message, and is never
    repeated.
  - A flush event is request 1110, sub-code 0000, status 0x00.
- **WTR (§10.1.4):** used only by the owner. Started on recovery from SF; on expiry the owner reverts by sending
  (NR,RB).
- **WTB (§10.1.4):** the owner starts it after FS or MS is cleared, so that a latent FS/SF/MS is caught before
  reverting.
- **Guard (§10.1.5):**
  - Started on local clear SF and on Clear (rows 20, 30, 36, 44).
  - While it runs, received R-APS is *not* passed to the priority logic, **except** Event (1110) messages.
  - Once started, it expires on its own.
- **Hold-off (§10.1.8):** when a new SF appears, the timer starts. If the defect is still present at expiry, SF is
  reported. Clearing is reported immediately.
- **Running/Expires signals:** a Running signal is asserted while its timer runs. An Expires signal is a one-shot at
  timeout, but not when the timer was stopped.

## 6. Validity check (§10.1.6) and flush logic (§10.1.10)

**Validity check:**
- The request must be one of NR 0000, MS 0111, SF 1011, FS 1101, Event 1110; anything else is dropped.
- The ring ID (the DA's last octet) must match the instance.
- Event + sub-code 0000 + status 0x00 → a flush indication goes to the flush logic. It is not affected by the guard
  timer.

**Flush logic (per port, initial pair = (00:00:00:00:00:00, 0)):**
- For each R-APS received on port p:
  - **(NR) without RB:** delete port p's stored pair, store nothing, no flush.
  - **Otherwise,** if (node, BPR) differs from the stored pair[p]:
    - store it as pair[p];
    - if it also differs from pair[other], flush the FDB, **unless** the message has DNF or carries our own node ID.
- When any ring port becomes blocked, delete the stored pairs on **both** ports.
- A flush indication from the validity check triggers a flush.

## 7. R-APS PDU (§10.3, G.8013)

```
DA 01-19-A7-00-00-<ringID> | SA | 0x8100 PCP/DEI/VID(R-APS VID) | 0x8902
off 0  : MEL(3b, high) | Version(5b)          Version: 0x01 this edition (0x00 = 2008 v1)
off 1  : OpCode = 40 (R-APS)
off 2  : Flags = 0 (ignored on Rx)
off 3  : TLV Offset = 32
off 4  : Request/State(4b, high) | Sub-code(4b)  (sub-code 0000 except Event)
off 5  : Status: RB(bit8=0x80) DNF(bit7=0x40) BPR(bit6=0x20) reserved(5b, 0, ignored)
off 6  : Node ID (6 octets, MAC)
off 12 : Reserved 2 (24 octets, 0, ignored)
off 36 : End TLV = 0
```

- **RB:** sent as 1 only by the owner, and only when the RPL is blocked.
- **BPR:** 0 means ring link 0 is blocked, 1 means link 1. With both blocked, either value may be used.
- **Node ID comparisons:** a "higher" ID is the larger 48-bit number, with the first octet most significant.

## 8. Defects (§10.4)

- **FOP-PM:** the owner receives (NR,RB) carrying a node ID other than its own. The equipment fault-management
  process is notified, and the node keeps operating.
- **FOP-TO:** no R-APS received on a ring port for more than K message cycles. The details are in G.8021; we use
  3.5 × 5 s.
  - It is not reported when the port is SF, or blocked from R-APS reception, or when both RPL ends are RB.

## 9. Behaviour notes (§10.2, App. III, App. IV)

- **SF:**
  - The detecting node blocks the failed port, sends (SF) continuously and flushes. That flush happens only on the
    transition from Idle.
  - Other nodes accept the (SF), unblock non-failed ports (opening the RPL), stop transmitting and flush through the
    flush logic.
- **Recovery:**
  - The recovered node keeps its port blocked, sends (NR) and starts the guard timer.
  - A node holding a block unblocks if it receives (NR) from a higher node ID.
  - The owner (revertive) starts WTR on (NR). At expiry it blocks the RPL, sends (NR,RB) and flushes.
  - Everyone then unblocks non-failed ports, stops (NR), and flushes through the flush logic.
- **Non-revertive:** the owner ignores (NR). An operator Clear at the owner reverts.
- **MS/FS:**
  - Block the requested port, unblock the other one, send continuously, and flush if coming from Idle.
  - Clear keeps the port blocked and sends (NR) with the guard timer; the owner then uses WTB instead of WTR.
  - Two MS commands cancel each other: the node with the local MS that receives (MS) from another node clears its MS,
    sends (NR) and keeps its port blocked.
  - Multiple FS can segment the ring.
- **App. IV.2:** the guard timer prevents simultaneous recoveries from acting on each other's stale (SF) and forming a
  loop.

## 10. liberps interpretations (also in `docs/CONFORMANCE.md`)

1. **State-machine invocation.**
   - The state machine runs for: (a) each local event (Clear, accepted FS/MS, SF declared, local clear SF, WTR/WTB
     expiry); (b) each received R-APS that passes the validity check and the guard timer and outranks every
     persistent local condition (latched FS/MS, SF, WTR/WTB Running).
   - When a persistent condition outranks the R-APS and is the same top request already signalled, the state machine
     is not re-run; the R-APS is counted as `suppressed`.
   - **Why:** re-running row 19 on every received (SF) would turn a stable (SF) into (SF,DNF), which contradicts
     App. III.
2. **Latched commands.**
   - FS/MS are latched while the node is in D/C, and forgotten when it leaves D/C (§10.1.9).
   - An MS that would land on a "No action" row (23/37/51), or that is outranked by a local condition, is rejected
     and notified rather than latched.
3. **Re-evaluation after a state change.** After each state change, persistent local conditions are re-evaluated.
   - Example: an SF ignored in D becomes the top request after Clear, via row 44 then row 61.
4. **Dual SF.** Two failed ports count as one "failed port" set: all of them are blocked. "Already blocked" means every
   failed port is blocked.
5. **BPR.**
   - The port the row refers to (requested, failed or RPL port) if that port is blocked;
   - else the single blocked port;
   - else 0.
6. **Tx Version.** 0x00 when compatible-version = 1, 0x01 when it is 2 (overridable). Rx accepts any version.
7. **SW R-APS forwarding** (optional, for software dataplanes).
   - A frame is forwarded rx-port → other port only when neither port is blocked and the frame carries a foreign node
     ID.
   - Forwarding happens before control processing, as a hardware R-APS_FF would do.
