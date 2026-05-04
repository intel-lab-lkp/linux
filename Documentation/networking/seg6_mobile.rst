.. SPDX-License-Identifier: GPL-2.0

=================================
SRv6 Mobile User Plane (RFC 9433)
=================================

This document describes the SRv6 Mobile User Plane (MUP) behaviors
implemented by the ``seg6_local`` lightweight tunnel.  Six of the
seven behaviors defined in `RFC 9433`_ are supported and configurable
through ``ip route ... encap seg6local action ...``: End.MAP,
End.M.GTP6.D, End.M.GTP6.D.Di, End.M.GTP6.E, End.M.GTP4.E, and
H.M.GTP4.D.

End.Limit (RFC 9433 Section 6.8) is unimplemented.

.. _`RFC 9433`: https://www.rfc-editor.org/rfc/rfc9433

Behaviors
=========

End.MAP (`RFC 9433`_ Section 6.2)
---------------------------------

Endpoint with SID mapping.  Replaces the IPv6 destination address with
the next SID; the SRH is left untouched.  Standard SRv6 endpoint hop
limit handling applies (an ICMP Time Exceeded is emitted when the IPv6
Hop Limit would reach zero per RFC 9433 Section 6.2 S01-S03; the Hop
Limit is decremented per S04 before forwarding).  ``nh6`` selects the
replacement SID::

    ip -6 route add 2001:db8:f::/64 \
        encap seg6local action End.MAP nh6 2001:db8:2::e \
        dev <dev>

End.M.GTP6.D (`RFC 9433`_ Section 6.3)
--------------------------------------

Encapsulation endpoint that consumes IPv6/UDP/GTP-U and emits SRv6.
The new SRH is built from the configured segment list, with the
original outer IPv6 destination ``D`` of the inbound GTP-U packet
stamped at SRH ``segments[0]`` (the ultimate destination of the SR
Policy).  The configured ``srh segs`` last entry is the remote
End.M.GTP6.E SID and lands at SRH ``segments[1]``, the penultimate
position required by RFC 9433 Section 6.5 Note; the kernel encodes
``Args.Mob.Session`` into the locator-relative slice of that SID so
that the egress End.M.GTP6.E peer can recover ``D`` (and hence the
original gNB-side GTP-U destination) from ``segments[0]`` after its
SRv6 strip::

    ip -6 route add 2001:db8:f::/64 \
        encap seg6local action End.M.GTP6.D \
            srh segs 2001:db8:2::e \
            src 2001:db8:2::1 \
            sr_prefix_len 64 \
        dev <dev>

``sr_prefix_len`` declares the locator length used by the remote
End.M.GTP6.E SID and must match the prefix length configured on that
remote endpoint; the SR Gateway has no way to discover the remote
SID's locator length on its own.

The wire SRH for the example above is
``[D, 2001:db8:2::e | Args.Mob.Session]``: ``segments[0]`` is the
saved original outer IPv6 DA of the GTP-U packet, and
``segments[1]`` is the End.M.GTP6.E SID at the egress UPF in the
penultimate position required by RFC 9433 Section 6.5.

End.M.GTP6.D.Di (`RFC 9433`_ Section 6.4)
-----------------------------------------

Drop-in variant of End.M.GTP6.D that preserves the original IPv6
destination address as ``segments[0]`` (the last in-transit SID in the
new SRH) and discards TEID/QFI rather than folding them into
Args.Mob.Session.  Useful when the upstream service expects the
original destination to survive untouched::

    ip -6 route add 2001:db8:f::/64 \
        encap seg6local action End.M.GTP6.D.Di \
            srh segs 2001:db8:2::e,2001:db8:3::e \
            src 2001:db8:2::1 \
        dev <dev>

End.M.GTP6.E (`RFC 9433`_ Section 6.5)
--------------------------------------

Egress endpoint that decapsulates SRv6 and emits IPv6/UDP/GTP-U.  The
active SID carries the 40-bit ``Args.Mob.Session`` field defined in
RFC 9433 Section 6.1 immediately after the locator; TEID and QFI are
extracted from it.  The route prefix length implicitly declares the
locator length on this end of the tunnel; no explicit
``sr_prefix_len`` is required because the SID is locally instantiated
by this route::

    ip -6 route add 2001:db8:e::/64 \
        encap seg6local action End.M.GTP6.E src 2001:db8:2::1 \
        dev <dev>

The route prefix length must leave room for the 40-bit
``Args.Mob.Session`` that immediately follows the locator, so the
constraint ``prefix_len + 40 <= 128`` (i.e. ``prefix_len <= 88``)
is enforced at install time.

The optional ``pdu_type {dl|ul|<num>}`` attribute supplies the PDU
Type field (3GPP TS 38.415 Section 5.5.2) of the GTP-U PDU Session
Container.  When set, every emitted packet carries the container with
that PDU Type and the QFI from ``Args.Mob.Session``; when unset the
kernel emits a short GTPv1-U header with no container, regardless of
the QFI.  ``pdu_type`` MUST be set on routes serving 5G N3 traffic;
omitting it targets LTE-only / S1-U deployments.  Numeric ``<num>``
in 0..15 is also accepted (per TS 38.415 the field is 4 bits wide;
2..15 are currently reserved).

End.M.GTP4.E (`RFC 9433`_ Section 6.6)
--------------------------------------

Egress endpoint that decapsulates SRv6 and emits IPv4/UDP/GTP-U.
The SID encodes the IPv4 destination per RFC 9433 Section 6.6
Figure 9.  ``v4_mask_len`` declares the width of the IPv4 DA slice
that immediately follows the locator (in 1..32, the constraint
``locator + v4_mask_len + 40 <= 128`` is enforced at install time)::

    ip -6 route add 2001:db8::/32 \
        encap seg6local action End.M.GTP4.E \
            src 2001:db8:2::1 v4_mask_len 32 \
        dev <dev>

The IPv6 source address carries the IPv4 SA per RFC 9433 Section 6.6
Figure 10.  ``v6_src_prefix_len`` declares the Source UPF Prefix
length P in bits (1..127, default 64); the IPv4 SA slice is then
``v4_mask_len`` bits wide starting at bit offset P.  The kernel
always reads a 32-bit window from the configured ``src`` template at
offset P (the upper ``v4_mask_len`` bits are overlaid with the
recovered IPv4 SA), so the constraint ``v6_src_prefix_len <= 96``
(equivalently ``v6_src_prefix_len + 32 <= 128``) is enforced at
install time.  Bits outside the IPv4 SA slice are taken verbatim
from the configured ``src`` template::

    ip -6 route add 2001:db8::/32 \
        encap seg6local action End.M.GTP4.E \
            src 2001:db8:2::1 v4_mask_len 32 v6_src_prefix_len 64 \
        dev <dev>

``pdu_type`` takes the same values and has the same effect as on
End.M.GTP6.E (see above).

H.M.GTP4.D (`RFC 9433`_ Section 6.7)
------------------------------------

Headend behavior that consumes IPv4/UDP/GTP-U and emits IPv6 with
the SID encoding the original IPv4 destination plus Args.Mob.Session
per RFC 9433 Section 6.7 Figure 11::

    ip -4 route add 10.99.0.0/24 \
        encap seg6local action H.M.GTP4.D \
            nh6 2001:db8:: \
            src 2001:db8:2::1 \
            v4_mask_len 32 sr_prefix_len 32 \
        dev <dev>

The inbound IPv4 SA is encoded into the IPv6 SA using the same
Figure 10 layout as End.M.GTP4.E (controlled by ``v6_src_prefix_len``,
default 64).

Per-route VRF / interface binding
=================================

The five GTP-related behaviors (End.M.GTP4.E, End.M.GTP6.E,
End.M.GTP6.D, End.M.GTP6.D.Di and H.M.GTP4.D) accept the standard
``oif`` ``seg6_local`` attribute to bind their egress lookup to a
specific output interface or VRF device.  This lets operators keep
the SRv6 underlay, the N3 reference point (toward gNB) and the N6
reference point (toward the data network) on separate routing tables
or VLAN sub-interfaces, which matches typical multi-tenant
deployments::

    ip -6 route add 2001:db8:e::/64 \
        encap seg6local action End.M.GTP6.E \
            src 2001:db8:2::1 \
            oif vrf-n3 \
        dev <dev>

Without ``oif`` the egress lookup uses the default routing table.

Netfilter integration
=====================

The five GTP-related behaviors expose IPv4 / IPv6 inner T-PDUs to
``NF_INET_PRE_ROUTING`` between the outer strip and the new outer
push (mirroring ``End.DX4`` / ``End.DX6``), so iptables / nftables /
conntrack can apply policy on the inner 5-tuple before
re-encapsulation.  Non-IP inner payloads bypass the hook and are
re-encapsulated unchanged.

Enable with::

    sysctl -w net.netfilter.nf_hooks_lwtunnel=1

This sysctl is one-way: it cannot be cleared without reloading the
kernel (see :doc:`nf_conntrack-sysctl`).

Example: drop traffic from a UE address on an ``End.M.GTP6.D`` SR
Gateway::

    nft add table ip filter
    nft 'add chain ip filter prerouting \
        { type filter hook prerouting priority 0; }'
    nft add rule ip filter prerouting \
        ip saddr 10.0.0.42 counter drop

GTP-U non-T-PDU handling
========================

The three GTP-U decap behaviors (``End.M.GTP6.D``,
``End.M.GTP6.D.Di`` and ``H.M.GTP4.D``) only encapsulate ``T-PDU``
frames (3GPP TS 29.281 Section 5.1, message type 255) into SRv6.
Any other GTP-U message (``Echo Request``/``Echo Response``,
``Error Indication``, ``Supported Extension Headers Notification``,
...) is forwarded unchanged toward its IPv4 / IPv6 destination as
a regular packet, so a downstream UPF that owns the GTP-U control
plane can process it.

The downstream UPF must therefore live behind the SR Gateway, on
the IPv4 / IPv6 network the GTP-U destination is reachable through.
Assigning the UPF's address to a local interface on the SR Gateway
is not a supported topology: the SR Gateway cannot at the same
time intercept ``T-PDU`` for SRv6 encapsulation and deliver
non-T-PDU GTP-U to a local userspace socket addressed to the same
destination.

Debugging
=========

The SRv6 Mobile data path drops malformed or out-of-policy packets via
the standard skb drop-reason infrastructure (``SEG6_MOBILE_*`` in
``include/net/dropreason-core.h``) rather than silent ``-EINVAL``, so
the cause is observable via the ``skb:kfree_skb`` tracepoint.
