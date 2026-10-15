# Linux deployment

`erpsd` is a foreground C11 daemon for one or more ERPS instances. It needs `CAP_NET_RAW` for
the ring-port packet sockets and `CAP_NET_ADMIN` for bridge VLAN and FDB netlink operations.
The control socket defaults to `/run/erpsd.sock` and is created with mode `0660`; set its
directory and process group for your operators.

## Dataplane setup

Create a Linux bridge with `vlan_filtering=1`, enslave both ring netdevs, and add each protected
VID as a **tagged** member on both ports. Put host ports in the relevant VLANs. Remove the
default VLAN 1 from ring ports if it is not protected. Keep the R-APS VID off the bridge:
`erpsd` receives and sends tagged R-APS directly on AF_PACKET sockets, while the core forwards
R-APS in software. Use the same ring ID and R-APS VID on every node in a ring, and a distinct
node ID for each node. Do not
assign the same protected VID to two instances sharing a bridge port.

Example for one node, with ring ports `ring0` and `ring1` on bridge `br0`:

```sh
ip link add name br0 type bridge vlan_filtering 1
ip link set ring0 master br0
ip link set ring1 master br0
bridge vlan add dev ring0 vid 10
bridge vlan add dev ring1 vid 10
ip link set br0 up
ip link set ring0 up
ip link set ring1 up
```

The daemon requires both ports to have the same bridge master and reads
`/sys/class/net/<bridge>/bridge/vlan_filtering` to confirm VLAN filtering. It uses the bridge
master's MAC as the automatic node ID when it can identify the bridge from the most recently
configured ring port; otherwise it takes the first non-loopback Ethernet MAC. Explicit `node-id`
is recommended, especially when configuring several rings before enabling them.

## Configuration and control

Save one CLI command per line in a configuration script:

```text
erps ring R1 ring-id 1 raps-vid 100 port0 ring0 port1 ring1
erps ring R1 node-id 02:00:00:00:00:01
erps ring R1 protected-vlans 10
erps ring R1 role owner rpl-port 1
erps ring R1 enable
```

On the RPL neighbour, configure `role neighbour` and its matching `rpl-port`; ordinary nodes
omit the role line. Production timers use the defaults. For short lab tests only, use
`erps ring R1 relaxed-timers on` and `erps ring R1 timers wtr 10s guard 100ms` before enabling.

```sh
build/gcc/erpsd -c /etc/erpsd.conf -s /run/erpsd.sock -l notice
build/gcc/erpsctl -s /run/erpsd.sock show erps summary json
build/gcc/erpsctl -s /run/erpsd.sock erps ring R1 force-switch port 0
build/gcc/erpsctl -s /run/erpsd.sock    # interactive terminal or stdin script
```

`erpsd` logs to stderr with monotonic timestamps. SIGINT and SIGTERM stop it cleanly. By
default, it removes protected VLAN membership from enabled ring ports on exit so an
unprotected loop cannot remain forwarding. `-n` leaves the current memberships in place.

The control socket accepts multiple newline-terminated commands per connection. Replies are
the command output followed by ASCII Record Separator, the decimal command result, and a
newline. Requests are limited to 2048 bytes. Each client's pending output is capped at 128 KiB;
larger replies are marked as truncated and return a nonzero result.

## Limits

This adapter supports a single ring per ERPS instance and Linux software bridges with VLAN
filtering. It does not program switch hardware or protect unlisted VLANs. The R-APS VID must
not be configured on the bridge. Bulk FDB deletion uses the Linux 6.0 API when available;
older kernels fall back to a bounded FDB dump (up to 8192 matching dynamic entries per flush).
The daemon's netlink operations are synchronous with a five-second timeout. During a large FDB
flush the event loop can pause for that operation. Port configuration changes after enabling
an instance require disabling and re-enabling it.

Run the four-node test with `tests/linux/ring4.sh`. It uses `unshare -Urn`; the kernel must
allow unprivileged user namespaces and the host must have `ip`, `bridge`, and Python 3.
