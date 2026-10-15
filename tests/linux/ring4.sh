#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
daemon=${ERPSD:-$repo/build/gcc/erpsd}
client=${ERPSCTL:-$repo/build/gcc/erpsctl}

fail() {
    echo "FAIL: $*" >&2
    local f
    for f in "${tmp:-/nonexistent}"/*.log; do
        [[ -f $f ]] || continue
        echo "--- $f ---" >&2
        tail -n 40 "$f" >&2
    done
    exit 1
}
pass() { echo "PASS: $*"; }

[[ -x $daemon && -x $client ]] || fail "build erpsd and erpsctl first (make CC=gcc all)"
for tool in ip bridge python3 unshare; do
    command -v "$tool" >/dev/null || fail "missing prerequisite: $tool"
done

if [[ ${ERPS_RING4_INSIDE:-0} != 1 ]]; then
    if [[ -r /proc/sys/kernel/unprivileged_userns_clone ]] &&
       [[ $(< /proc/sys/kernel/unprivileged_userns_clone) != 1 ]]; then
        fail "unprivileged user namespaces are disabled"
    fi
    unshare -Urnm -- true || fail "unshare -Urnm is not permitted here"
    exec unshare -Urnm --propagation private -- env ERPS_RING4_INSIDE=1 ERPSD="$daemon" \
        ERPSCTL="$client" bash "$repo/tests/linux/ring4.sh"
fi

# Our own mount namespace: remount sysfs so /sys/class/net shows this network namespace.
mount -t sysfs sysfs /sys || fail "cannot mount sysfs in the test namespace"

tmp=$(mktemp -d)
pids=()
cleanup() {
    local p
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
    for p in "${pids[@]}"; do wait "$p" 2>/dev/null || true; done
    for p in a1 b1 c1 d1 ha hb hc hd; do ip link del "$p" 2>/dev/null || true; done
    for p in brA brB brC brD; do ip link del "$p" 2>/dev/null || true; done
    rm -rf -- "$tmp"
}
trap cleanup EXIT INT TERM

for node in A B C D; do
    ip link add name "br$node" type bridge vlan_filtering 1
    ip link set "br$node" up
done

ip link add a1 type veth peer name b0
ip link add b1 type veth peer name c0
ip link add c1 type veth peer name d0
ip link add d1 type veth peer name a0

for spec in 'a0 brA' 'a1 brA' 'b0 brB' 'b1 brB' \
            'c0 brC' 'c1 brC' 'd0 brD' 'd1 brD'; do
    read -r port master <<<"$spec"
    ip link set "$port" master "$master"
    bridge vlan del dev "$port" vid 1 2>/dev/null || true
    bridge vlan add dev "$port" vid 10
    ip link set "$port" up
done

for spec in 'a A 1' 'b B 2' 'c C 3' 'd D 4'; do
    read -r lower upper last <<<"$spec"
    ip link add "h$lower" type veth peer name "h${lower}0"
    ip link set "h${lower}0" master "br$upper"
    bridge vlan del dev "h${lower}0" vid 1 2>/dev/null || true
    bridge vlan add dev "h${lower}0" vid 10 pvid untagged
    ip link set "h${lower}0" up
    ip address add "10.10.10.$last/24" dev "h$lower"
    ip link set "h$lower" up
done
pass "created four VLAN-filtering bridges and host ports"

for spec in 'A a0 a1 neighbour 0 0a' 'B b0 b1 none 0 0b' \
            'C c0 c1 none 0 0c' 'D d0 d1 owner 1 0d'; do
    read -r node p0 p1 role rpl mac <<<"$spec"
    cfg="$tmp/$node.conf"
    {
        echo "erps ring R1 ring-id 1 raps-vid 100 port0 $p0 port1 $p1"
        echo "erps ring R1 node-id 02:00:00:00:00:$mac"
        echo 'erps ring R1 protected-vlans 10'
        echo 'erps ring R1 relaxed-timers on'
        echo 'erps ring R1 timers wtr 10s guard 100ms'
        if [[ $role != none ]]; then
            echo "erps ring R1 role $role rpl-port $rpl"
        fi
        echo 'erps ring R1 enable'
    } >"$cfg"
    "$daemon" -c "$cfg" -s "$tmp/$node.sock" >"$tmp/$node.log" 2>&1 &
    pids+=("$!")
done

check_summary() {
    local phase=$1 node json expected
    for node in A B C D; do
        json=$("$client" -s "$tmp/$node.sock" show erps summary json 2>/dev/null) || return 1
        python3 -c '
import json, sys
phase, node = sys.argv[1:3]
rings = json.load(sys.stdin)["rings"]
if len(rings) != 1: sys.exit(1)
r = rings[0]
blocked = [p["blocked"] for p in r["ports"]]
if not r["enabled"]: sys.exit(1)
if phase == "idle":
    expected = {"A": [True, False], "B": [False, False],
                "C": [False, False], "D": [False, True]}[node]
    ok = r["state"] == "Idle" and blocked == expected
else:
    expected = {"A": [False, False], "B": [False, True],
                "C": [True, False], "D": [False, False]}[node]
    ok = r["state"] == "Protection" and blocked == expected
sys.exit(0 if ok else 1)
' "$phase" "$node" <<<"$json" || return 1
    done
}

wait_state() {
    local phase=$1 timeout=$2 p
    local end=$((SECONDS + timeout)) # separate: expansions run before assignments
    while (( SECONDS < end )); do
        if check_summary "$phase"; then return 0; fi
        for p in "${pids[@]}"; do
            kill -0 "$p" 2>/dev/null || fail "erpsd exited; logs in $tmp"
        done
        sleep 0.5
    done
    for p in A B C D; do
        echo "--- $p ---" >&2
        cat "$tmp/$p.log" >&2
        "$client" -s "$tmp/$p.sock" show erps summary json >&2 || true
    done
    fail "timed out waiting for $phase"
}

has_vid() {
    bridge vlan show dev "$1" | grep -Eq '(^|[[:space:]])10([[:space:]]|$)'
}

assert_vlan_state() {
    local phase=$1 port
    local blocked=()
    if [[ $phase == idle ]]; then blocked=(a0 d1); else blocked=(b1 c0); fi
    for port in a0 a1 b0 b1 c0 c1 d0 d1; do
        local should_block=0 item
        for item in "${blocked[@]}"; do
            if [[ $item == "$port" ]]; then should_block=1; fi
        done
        # explicit ifs: a false `a && b` as a function's last status would trip set -e
        if (( should_block )); then
            if has_vid "$port"; then fail "VLAN 10 still present on blocked $port"; fi
        else
            if ! has_vid "$port"; then fail "VLAN 10 missing from forwarding $port"; fi
        fi
    done
}

rx_total() {
    local port sum=0 count
    for port in a0 a1 b0 b1 c0 c1 d0 d1; do
        count=$(<"/sys/class/net/$port/statistics/rx_packets")
        sum=$((sum + count))
    done
    echo "$sum"
}

send_broadcasts() {
    python3 - <<'PY'
import socket
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
s.bind(("ha", 0))
source = bytes.fromhex(open("/sys/class/net/ha/address").read().strip().replace(":", ""))
frame = b"\xff" * 6 + source + b"\x88\xb5" + b"liberps-ring4".ljust(46, b"\x00")
for _ in range(4):
    s.send(frame)
s.close()
PY
}

wait_state idle 30
assert_vlan_state idle
pass "Idle convergence and RPL VLAN 10 blocking"

before=$(rx_total)
remote_before=$(< /sys/class/net/b0/statistics/rx_packets)
send_broadcasts
sleep 2
after=$(rx_total)
remote_after=$(< /sys/class/net/b0/statistics/rx_packets)
(( remote_after > remote_before )) || fail "broadcast did not traverse ring"
(( after - before < 500 )) || fail "broadcast traffic exceeded bounded counter budget ($((after-before)))"
pass "broadcast traffic crossed the ring without a storm"

ip link set b1 down
wait_state protection 30
assert_vlan_state protection
pass "B-C failure entered Protection and opened the RPL"

ip link set b1 up
wait_state idle 35
assert_vlan_state idle
pass "WTR expiry restored Idle and RPL blocking"

echo 'PASS: ring4 end-to-end'
