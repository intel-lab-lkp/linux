.. SPDX-License-Identifier: GPL-2.0

==============
Flow Dissector
==============

Overview
========

The flow dissector parses a packet's headers into a ``struct flow_keys``: the
addresses, ports, and other fields that identify the flow the packet belongs
to. Its entry point is ``__skb_flow_dissect()`` in
``net/core/flow_dissector.c``.

The dissected keys feed ``skb->hash`` (via ``flow_hash_from_keys()``), which is
the canonical flow identity consumed all over the stack, including:

* receive-side steering: RSS, RPS, RFS and accelerated RFS (aRFS);
* transmit-side spreading: ECMP / multipath route selection and bonding / LAG
  ``xmit_hash_policy``;
* classification and offload: tc-flower.

A single dissection therefore feeds every one of these, so the dissector sits
on a hot path in most networking workloads.

Two standard dissectors are built in: ``flow_keys_dissector`` (the general one)
and ``flow_keys_dissector_symmetric`` (same source and destination hashed
symmetrically). In addition, subsystems such as tc-flower build **custom**
dissector instances at run time that request only the keys they need. A custom
instance carries its own ``used_keys`` bitmask and per-key offset table, so its
field accesses are resolved dynamically at each dissect.

struct flow_keys
================

``struct flow_keys`` (see ``include/net/flow_dissector.h``) collects the
dissected metadata. Its main members are:

* ``control`` -- address type and control flags (for example
  ``FLOW_DIS_ENCAPSULATION`` when the keys came from inside a tunnel, and
  ``FLOW_DIS_IS_FRAGMENT``);
* ``basic`` -- L3 protocol (``n_proto``) and L4 protocol (``ip_proto``);
* ``addrs`` -- source and destination addresses (IPv4 or IPv6);
* ``ports`` -- source and destination ports;
* ``vlan`` / ``cvlan`` -- 802.1Q / 802.1AD tags;
* ``tags``, ``keyid``, ``icmp`` -- MPLS/flow-label tags, GRE/tunnel key, and
  ICMP identifiers.

The dissection path
===================

The built-in dissector is a generic protocol-graph parser: it walks the header
chain in a loop, dispatching on the current L2/L3 ethertype and then the L4
protocol, following encapsulation (IP-in-IP, GRE, ...) by re-entering the loop
on the inner header. The number of headers it will descend through is bounded
by ``MAX_FLOW_DISSECT_HDRS`` so that a crafted deeply nested packet cannot make
it loop without limit.

This generic parser is referred to below as the *slow path*: it is table- and
loop-driven and handles every protocol the dissector knows about.

The BPF flow dissector
======================

A per-network-namespace BPF program can be attached to replace the built-in
dissector for the protocols it chooses to handle. When one is attached,
``__skb_flow_dissect()`` runs it **first**; if the program returns a verdict
other than ``BPF_FLOW_DISSECTOR_CONTINUE``, its result is used and the built-in
dissector (including the fast paths below) is not run for that packet. A system
running an attached BPF dissector therefore sees behaviour defined entirely by
its program.

See :doc:`/bpf/prog_flow_dissector` for the BPF program type, its API, and its
reference implementation.

Opt-in fast paths
=================

For the packet shapes that dominate real deployments, the built-in dissector
also provides an opt-in, straight-line *fast path* that produces the same
result as the slow-path graph walk with far fewer instructions. Restricting the
fast path to the two standard dissectors turns their per-key offsets into
compile-time constants, so the parse becomes branch-light straight-line code;
the isolated per-dissect cost of the eligible shapes drops by roughly half,
with the largest wins on in-order cores.

Each shape has its own gate -- a ``static_branch`` exposed as a sysctl under
``/proc/sys/net/flow_dissector/`` -- and **every gate is off by default**. When
a gate is off, the only added cost is one not-taken branch per dissect, and the
dissector's output is exactly what it was before. The fast path runs only
*after* the BPF hook described above, so an attached BPF program always takes
precedence.

The fast paths are **byte-identical by contract**: for any packet, a fast path
either writes exactly the ``flow_keys`` the slow path would have written, or it
returns false and the slow path runs. There are only these two outcomes, and
the equivalence is enforced in-tree by a KUnit test suite
(``CONFIG_FLOW_DISSECTOR_KUNIT_TEST``). Enabling a byte-identical shape gate
never changes any consumer's hash; it only makes the dissection cheaper.

The eligible byte-identical shapes are plain Ethernet + IPv4/IPv6 + TCP/UDP,
single and stacked VLAN (QinQ), PPPoE sessions, a single MPLS label, the
IP-in-IP family, and plain GRE. The individual knobs are documented in
:doc:`/admin-guide/sysctl/net` under ``/proc/sys/net/flow_dissector``.

When to enable
==============

Turning a byte-identical shape gate on is worthwhile when enough traffic matches
that shape to repay the small cost a non-matching packet pays while the gate is
on. Concretely, if a matching packet saves ``S`` and a non-matching packet costs
``C`` (both measured per shape and per micro-architecture), enabling the gate is
net-positive once the fraction of traffic matching the shape exceeds the
break-even ``p_be = C / (S + C)``.

That fraction is observable: the read-only file
``/proc/net/flow_dissector_stats`` reports, per shape, how many packets the slow
path handled, how many the fast body handled, and the resulting eligible
fraction, so ``(occurrences + fast_hits) / dissects`` is a gate-invariant
measure of how much traffic each shape would accelerate.

A separate RFC proposes an optional ``auto`` mode built on these counters: the
kernel would sample them over a packet-count window and flip the
byte-identical gates itself, with hysteresis and a flip-rate cap, so the
operator does not have to tune each shape by hand. That controller is not part
of this series.

References
==========

* :doc:`/admin-guide/sysctl/net` -- the ``/proc/sys/net/flow_dissector/``
  per-shape knob reference and the ``/proc/net/flow_dissector_stats`` file.
* :doc:`/bpf/prog_flow_dissector` -- the BPF flow dissector program type.
