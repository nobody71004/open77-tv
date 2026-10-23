#!/bin/sh
# Let the Android inside Waydroid reach the internet.
#
# Waydroid gets its own half of the network right: dnsmasq hands the container
# 192.168.240.112 with 192.168.240.1 as DNS and gateway, and Android's
# ConnectivityService reports an Ethernet network CONNECTED with INTERNET. What
# was missing is the host's forward chain: Docker's nft rules end FORWARD with
# `policy drop`, so every packet from waydroid0 to the uplink was dropped and
# Android sat on "Connect to a network to continue" while its lease, route and
# DNS were all in place (measured 2026-10-23). The MASQUERADE it needs is
# Waydroid's own (iptables-legacy, written by waydroid-net.sh at container
# start); these two accepts are the half that belongs to this host.
#
# iptables-nft and iptables-legacy both hook the kernel, so an ACCEPT in the
# legacy table does not save a packet from the nft table's policy -- the rule
# has to be in the nft chain that drops it.
#
# Idempotent, and safe to run before waydroid0 exists: the rules are matched by
# interface name.
set -eu

# The chain, not the whole ruleset: install.sh already admits waydroid0 on the
# host's INPUT chain, and a ruleset-wide match sees that rule and concludes the
# FORWARD half is done too -- which is how one of the two was missing after a
# flush (measured 2026-10-23).
chain() { nft list chain ip filter FORWARD 2>/dev/null; }

for direction in iifname oifname; do
  if chain | grep -q "$direction \"waydroid0\""; then
    echo "== forward $direction waydroid0: already admitted"
  else
    nft insert rule ip filter FORWARD $direction waydroid0 counter accept
    echo "== forward $direction waydroid0: admitted"
  fi
done

# Say it with counters, so "is it admitted" and "has anything used it" are the
# same command.
chain | grep "waydroid0" | sed 's/^/   /'
