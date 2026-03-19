.. SPDX-License-Identifier: GPL-2.0+
.. Copyright (C) 2026 Daniel Gröber <dxld@debian.org>

==============================================
IPXLAT - IPv6<>IPv4 IP/ICMP Translation (SIIT)
==============================================

ipxlat (``CONFIG_IPXLAT=y``) provides a virtual netdevice implementing
stateless IP packet translation between IP versions 6 and 4. This is a
building block for establishing layer 3 connectivity between otherwise
uncommunicative IPv6-only and/or IPv4-only networks.


Creation and Configuration Parameters
=====================================

An ipxlat netdevice can be created and configured using YNL like so::

    $ ip link add siit0 type ipxlat

    $ IID=$(cat /sys/class/net/siit0/ifindex)

    $ ADDR_HEX=$(python3 -c 'import ipaddress,sys; \
        print(ipaddress.IPv6Address(sys.argv[1]).packed.hex())' \
        64:ff9b:: | tee /dev/stderr)
    0064ff9b000000000000000000000000

    $ ./tools/net/ynl/pyynl/cli.py --family ipxlat --json '{"ifindex": $IID, \
        "config": {"xlat-prefix6": "'$HEX_ADDR'", "prefix-len": 96} }'

(TODO: Once implemented) A ipxlat netdevice can be configured using
iproute2::

    $ ip link add siit0 type ipxlat [ OPTIONS ]

    # where OPTIONS can include (TODO: iproute2 patch):
    #
    #   prefix ADDR          (default 64:ff9b::/96)
    #
    #   lowest-ipv6-mtu MTU  (default 1280)


Introduction to Packet-level IPv6<>IPv4 Translation
===================================================

Translatable packets delivered into an ipxlat device as either of the IP
protocol versions loop-back as the other. Untranslatable packets are
rejected with ICMP errors of the same IP version as appropriate or dropped
silently if required by RFC-SIIT_.

.. _RFC-SIIT: https://datatracker.ietf.org/doc/html/rfc7915

Supported upper layer protocols (TCP/UDP/ICMP) have their checksums
recomputed as-needed as part of translation. Unsupported IP protocols
(IPPROTO\_*) are passed through unmodified. This will make them fail at the
receiver except in special cases.

Differences in IP layer semantic concerns are handled using several
different strategies, here we'll only give a high-level summary in the
areas of most friction:
  Fragmentation approach, Path MTU Discovery (PMTUD), IP Options and Extension
  Headers.

**Fragmentation Approach** (v4: on-path vs v6: end-to-end) is smoothed over by:
 | 4->6: Fragmenting (DF=0) IPv4 packets when needed. See "lowest-ipv6-mtu".
 | 6->4: Using on-path frag. down the line for v4 pkts smaller than 1260.
 Details are tedious, check RFC-SIIT_.

**PMTUD** is maintained by recalculating advised MTU values in ICMP
PKT_TOO_BIG and FRAG_NEEDED messages as they're being translated. Taking
into account the necessary header re-sizing and post-translation nexthop
MTU in the main routing table.

**IP Options and IPv6 Extension Headers** except the Fragment Header are
dropped or ignored expept where more specific behaviour is specified in
RFC-SIIT_.


Address Translation
-------------------

The ipxlat address translation algorithm is stateless, per RFC-ADDR_, all
possible IPv4 addressess are mapped one-to-one into the translation prefix,
optionally including a non-standard "suffix". See `RFC-ADDR Section 2.2
<https://datatracker.ietf.org/doc/html/rfc6052#section-2.2>`_.

.. _RFC-ADDR: https://datatracker.ietf.org/doc/html/rfc6052

IPv6 addressess outside this prefix are rejected with ICMPv6 errors with
the notable exception of ICMPv6 errors originating from untranslatable
source addressess. These are translated to be sourced from the IPv4 Dummy
Address ``192.0.0.8`` (per I-D-dummy_) instead to maintain IPv4 traceroute
visibility.

.. _I-D-dummy:
   https://datatracker.ietf.org/doc/draft-ietf-v6ops-icmpext-xlat-v6only-source/

In a basic bidirectional 6<>4 connectivity scenario this means IPv6 hosts
must be addressed wholly from inside the translation prefix and per
RFC-ADDR_. Plain vanilla SLAAC doesn't cut it here, static addressing or
DHCPv6 is needed, unless that is we introduce statefulnes (RFC-NAT64_) into
the mix. See below on that.

.. _RFC-NAT64: https://datatracker.ietf.org/doc/html/rfc6146


Stateful Translation (NAT64)
----------------------------

Using NAT64 has several drawbacks, it's necessary only when your control
over IPv4 or IPv6 addressing of hosts is limited.

Using nftables we can turn a system into a stateful translator. For example
to make the IPv4 internet reachable to a IPv6-only LAN having this system
as it's default route, further assuming we have an IPv4 default route and
``192.0.2.1/32`` is routed to this system::

 $ ip link add siit0 type ipxlat
 $ ip link set dev siit0 up
 $ ip route 192.0.2.1/32 dev siit0
 $ ip route 64:ff9b::/96 dev siit0
 $ sysctl -w net.ipv4.conf.all.forwarding=1
 $ sysctl -w net.ipv6.conf.all.forwarding=1
 $ nft -f- <<EOF
 table ip6 nat {
         chain postrouting {
                 type nat hook postrouting priority filter; policy accept;
                 oifname "siit0" snat to 64:ff9b::c002:1 comment "::192.0.2.1"
         }
 }
 table ip nat {
         chain postrouting {
                 type nat hook postrouting priority filter; policy accept;
                 iifname "siit0" masquerade
         }
 }
 EOF

Note: Keep reading when replacing the 192.0.2.0/24 documentation
placeholder with RFC 1918 "private IPv4" space.


Translation Prefix Choice and Complications
-------------------------------------------

Several prefix sizes between /32 and /96 are supported by ipxlat. Using
a /96 prefix is often convenient as it allows using the dotted quad IPv6
notation, eg.: "64:ff9b::192.0.2.1". RFC-ADDR_ "3.3. Choice of Prefix for
Stateless Translation Deployments" has more detailed recommendations.

The "Well-Known Prefix" (WKP) 64:ff9b::/96, while a convenient and short
choice for LANs, comes with some IETF baggage. As specified (at time of
writing) addressess drawn from RFC 1918 "private IPv4" space "MUST NOT" be
used with the WKP. While ipxlat does not enforce this other network
elements may.

If I-D-WKP-1918_ makes it through the IETF process this complication for
the cautious network engineer may dissapear in the future.

.. _I-D-WKP-1918:
   https://datatracker.ietf.org/doc/draft-ietf-v6ops-nat64-wkp-1918/

In the meantime the newer and more lax prefix allocated by RFC-LWKP_ or an
entirely Network-Specific Prefix may be a better fit. We'd recommend using
the checksum-neutral ``64:ff9b:1:fffe::/96`` prefix from the larger /48
allocation.

.. _RFC-LWKP: https://datatracker.ietf.org/doc/html/rfc8215


RFC Considerations for Userspace
--------------------------------

- Per `RFC 7915
  <https://datatracker.ietf.org/doc/html/rfc7915#section-4.5>`_,
  ipxlat SHOULD drop UDPv4 zero checksum packets, yet we chose to always
  recalculate checksums for unfragmented packets.

  If you want your translator to follow the SHOULD add a netfilter rule
  dropping such packets. For example using ``nft(8)`` syntax::

    nft add rule filter ip postrouting -- oifkind ipxlat udp checksum 0 log drop

- Per `RFC 6146
  <https://datatracker.ietf.org/doc/html/rfc6146#section-3.4>`_,
  Fragmented UDPv4 zero checksum recalculation by reassembly is not
  supported.

- I-D-dummy_: Adding a Node Identity Object to for IPv4-side traceroute
  disambiguation is not yet supported.
