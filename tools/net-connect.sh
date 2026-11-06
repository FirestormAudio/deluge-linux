#!/usr/bin/env bash
# Bring up the host side of the Deluge USB NCM link and check connectivity.
#
# The platform image presents a CDC-NCM gadget with a fixed host-side MAC
# (deluge-gadget's host_addr). The host interface NAME varies by USB port, so we
# locate it by that MAC and give it the static host address. Idempotent.
#
# Usage:  ./tools/net-connect.sh            # connect (re-execs via sudo)
#         ./tools/net-connect.sh down       # remove the host address
# Env overrides: DELUGE_HOST_MAC, DELUGE_HOST_IP, DELUGE_DEV_IP
set -euo pipefail

HOST_MAC="${DELUGE_HOST_MAC:-02:22:82:ff:ff:22}"   # = deluge-gadget host_addr
HOST_IP="${DELUGE_HOST_IP:-192.168.7.1/24}"
DEV_IP="${DELUGE_DEV_IP:-192.168.7.2}"
ACTION="${1:-up}"

# The `ip` commands need root — re-exec once under sudo, preserving overrides.
if [ "$(id -u)" -ne 0 ]; then
    exec sudo DELUGE_HOST_MAC="$HOST_MAC" DELUGE_HOST_IP="$HOST_IP" \
        DELUGE_DEV_IP="$DEV_IP" -- "$0" "$ACTION"
fi

iface=$(ip -o link | grep -i "$HOST_MAC" | awk -F': ' '{print $2}' | head -1)
if [ -z "$iface" ]; then
    echo "ERROR: no host interface with MAC $HOST_MAC." >&2
    echo "  Is the board booted into the platform image (NCM) and in peripheral mode?" >&2
    exit 1
fi
echo "Deluge NCM interface: $iface"

if [ "$ACTION" = "down" ]; then
    ip addr del "$HOST_IP" dev "$iface" 2>/dev/null || true
    echo "removed $HOST_IP from $iface"
    exit 0
fi

# NetworkManager will otherwise DHCP the gadget and drop our static address.
if command -v nmcli >/dev/null 2>&1; then
    nmcli device set "$iface" managed no || true
fi

ip link set "$iface" up
if ip -o addr show dev "$iface" | grep -qw "${HOST_IP%/*}"; then
    echo "host address ${HOST_IP} already present"
else
    ip addr add "$HOST_IP" dev "$iface"
    echo "host ${HOST_IP} up on $iface"
fi

if ping -c 2 -W 2 "$DEV_IP" >/dev/null 2>&1; then
    echo "OK: $DEV_IP reachable  ->  ssh root@$DEV_IP"
else
    echo "WARN: $DEV_IP not answering yet (give the board a moment, or re-check the boot log)."
fi
