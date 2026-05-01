.. SPDX-License-Identifier: GPL-2.0

==============================================
INTR_CTRL: The GPU's Interrupt Controller
==============================================

This document describes the interrupt controller which sits between
the GPU's internal engines (including GSP) and the host's MSI delivery path.
It is the first hardware block the host driver consults whenever an interrupt is
delivered, and it is responsible for telling software which engine interrupted.
It is also known as the "INTR_CTRL" block. The main evolution of interrupt
controller architecture is to support virtualization (multiple views of the
interrupt tree for PFs and VFs).

Per-function trees
==================

Each PCIe function has its own private interrupt tree:

* The Physical Function (PF) sees a tree at a fixed BAR0 offset.
* Each Virtual Function (VF) sees its own tree at the same BAR0 offset
  *within its own BAR0 view*, and it cannot observe the PF's tree.
* The GSP firmware also has its own logical tree in INTR_CTRL used for
  receiving interrupts to GSP from the engines, but we don't need to
  bother with those in nova-core (that's GSP's business).

.. note::
  The PF can also see the VF's tree at an aliased offset in BAR0, which
  is useful if the guest driver needs host help in configuring interrupts,
  but we currently do not use that in nova-core.

Two-level interrupt tree
========================

INTR_CTRL multiplexes up to 256 internal interrupt vectors onto the single
MSI line allocated to the PCIe function via a two-level tree of MMIO
registers, where each TOP bit covers exactly two adjacent leaves (known
as a "subtree"). As an example, on GA102, the tree looks like this::

    TOP register (32-bit)
     |   bit N == 1 => subtree N has at least one pending leaf
     |
     +-- bit 0 --> LEAF[0] (32-bit)  vectors    0..  31  (nonstall base)
     |             LEAF[1] (32-bit)  vectors   32..  63
     |
     +-- bit 1 --> LEAF[2] (32-bit)  vectors   64..  95
     |             LEAF[3] (32-bit)  vectors   96.. 127
     |
     +-- bit 2 --> LEAF[4] (32-bit)  vectors  128.. 159  (CPU doorbell @ 129)
     |             LEAF[5] (32-bit)  vectors  160.. 191
     |
     +-- bit 3 --> LEAF[6] (32-bit)  vectors  192.. 223  (engine stall base,
     |             LEAF[7] (32-bit)  vectors  224.. 255   GSP stall vector)
     |
     +-- bits 4..7 (Hopper+ only) --> LEAF[8..15]


The second level (LEAF registers) is where individual engines deposit
their interrupt events. The first level (TOP register) is a summary: bit
``N`` of TOP is set if and only if at least one bit is set in the two
leaves owned by subtree ``N``. Software can therefore start from TOP,
identify which subtrees have work, and then descend into just those leaves
- it never needs to read all 16 leaf registers blindly.

The advantage of this architecture is that it allows the host to mask
entire subtrees of interrupts at once, rather than having to mask each
leaf individually. Similar reasoning for determining which interrupt
source fired, the host can walk the tree without going through all 16
leaves.

Each TOP bit, called a **subtree**, is wired in hardware to exactly two
adjacent leaves (``leaves 2*N`` and ``2*N + 1``), so nova-core derives
``num_subtrees = num_leaves / 2`` rather than tracking both numbers
independently.

End-to-end engine interrupt routing to MSI
===============================================

The engine interrupt routing is done by the engine's INTR_CTRL(i)
register. This register is written once by GSP at boot and decides
which tree/leaf to activate in the INTR_CTRL. This model assists in
virtualization, as it is possible for the GSP to route engines to the
correct tree/leaf corresponding to the VF. GSP then provides the
information to the host via the INTR_GET_KERNEL_TABLE RPC so that
the host knows which leaf bits correspond to an engine's interrupt.

It roughly looks like the following::
              +--------------- Engine (CE, GR, NVDEC, ...) ---------------+
              |                                                           |
              |   internal work completes                                 |
              |          |                                                |
              |          v                                                |
              |   +-----------------------------------------+             |
              |   | INTR_CTRL(i): programmable register     |             |
              |   | (written once by GSP-RM at boot,        |             |
              |   |  one such reg per engine)               |             |
              |   |                                         |             |
              |   |   VECTOR  = 200   (-> which leaf bit)   |             |
              |   |   GFID    = 0     (-> which function's  |             |
              |   |                       tree: 0=PF, N=VF) |             |
              |   |   CPU     = 1     (-> copy to CPU tree?)|             |
              |   |   GSP     = 0     (-> copy to GSP tree?)|             |
              |   +--------------------+--------------------+             |
              |                        |                                  |
              |     engine builds      |                                  |
              |     interrupt ctrl     |                                  |
              |     command message    |                                  |
              |  (all2ctrl_intr_cmd)   |                                  |
              +------------------------|----------------------------------+
                                       |
                                       v
                   +-----------------------------------------+
                   | Central INTR_CTRL block                 |
                   |                                         |
                   | reads message; for the tree picked      |
                   | by GFID, sets:                          |
                   |   LEAF[ 200 / 32 ]  = LEAF[6]           |
                   |   bit  ( 200 % 32 ) = bit 8             |
                   | TOP subtree 3 = pending                 |
                   +--------------------+--------------------+
                                        |
                                        v
                                MSI to host (PF)

Vector encoding
---------------

A vector number ``v`` (0..255) maps to a unique ``(leaf, bit)`` pair::

    leaf_index = v / 32
    bit_in_leaf = v % 32

For example, vector 129 (the CPU doorbell self-test vector we use in
the INTR_CTRL self-test, see below) lives in ``LEAF[4]`` at bit 1,
which is reachable through subtree 2 in the TOP register.

Architecture differences
------------------------

The number of *active* leaves depends on the GPU architecture:

==================  =================  ==========  ================
Architecture        Active leaves      Subtrees    ``subtree_mask``
==================  =================  ==========  ================
Turing / Ampere     8                  4           ``0x0f``
Ada Lovelace        8                  4           ``0x0f``
Hopper / Blackwell  16                 8           ``0xff``
==================  =================  ==========  ================

Pre-Hopper chipsets only have leaves 0-7 wired up; the upper half of the
TOP register is unused and reads back as zero. Hopper widened the tree to
16 leaves to support more engines and more virtual functions.

Stall vs nonstall vector ranges
===============================

A common point of confusion: **stall and nonstall are NOT separate
interrupt trees**. They are two different *vector ranges* within the same
INTR_CTRL tree, and the source engine picks which range its interrupt
lands in.

* **Nonstall** vectors live in the low leaves (``LEAF[0..1]``, vectors
  0..63). The engine fires the interrupt and continues immediately,
  whether or not the host has acknowledged it. Used for "fire and
  forget" notifications - examples: vblank, semaphore wakeups, performance
  counter overflow).

* **Stall** vectors live in the high leaves.
  On Turing and Ampere:
  ``LEAF[6..7]`` (vectors 192..255, subtree 3).
  On Hopper:
  ``LEAF[6..11]`` (subtrees 3..5).
  The engine *blocks* (stalls) until the host writes a W1C (Write 1 to Clear)
  ack to the leaf bit. Example: MMU fault.

ISR operation flow
==================

When an MSI fires, the ISR walks the tree in a fixed sequence::

    1. UNARM    write subtree_mask -> TOP_EN_CLEAR  (stop MSI delivery)
    2. READ     pending = TOP                       (which subtrees fired?)
    3. ACK      for each pending leaf:
                   mask = LEAF[i]                   (read pending vectors)
                   LEAF[i] = mask                   (W1C the latches)
                   dispatch handlers for set bits
    4. REARM    write subtree_mask -> TOP_EN_SET    (resume MSI delivery)

A few important properties:

* **All pending leaf bits must be acked**, even bits that nova-core does
  not currently dispatch. Leaving a bit set keeps its subtree pending in
  TOP, which means the next REARM immediately fires another MSI - an
  interrupt storm. The handler therefore acks the full leaf mask, not
  just the bits it recognizes.

* **REARM happens only after every pending leaf has been acked.**
  Otherwise a still-set leaf bit would re-fire MSI on the next REARM
  even though the ISR is mid-processing.

Edge-trigger and rearm semantics
================================

Each LEAF bit is a sticky latch with edge-triggered SET behaviour:

* The latch SETS on the rising edge of the source signal (an engine
  message arriving on the interrupt control command interface, or a falcon
  output wire transitioning low->high).
* The latch CLEARS only when the host writes a 1 to that bit (W1C).
* A still-asserted source does **not** re-set the latch. There is no
  way to make a level-asserted signal "re-fire" except to drop and
  re-raise it.

There are two distinct rescue mechanisms, at two different layers,
for two different problems. They are easy to confuse, so first some
vocabulary as the rescues are entirely about how these pieces of
hardware are wired together:

* ``LEAF[i]``: each bit is a *sticky latch*: bit ``b`` SETs on the
  rising edge of an ``all2ctrl_intr_cmd`` message and CLEARs only
  when the host writes 1 to that bit (W1C ack).

* ``TOP[N]``: bit ``N`` of the read-only ``TOP`` register. Purely
  combinational: it reads 1 if and only if at least one bit is
  latched in either of the two leaves owned by subtree ``N``,
  i.e. ``LEAF[2N]`` or ``LEAF[2N+1]``. Software cannot write ``TOP``;
  the hardware tracks the leaves automatically.

* ``TOP_EN[N]``: a single host-controlled "armed?" bit per subtree,
  internal to INTR_CTRL. The host *sets* it by writing 1 to
  ``TOP_EN_SET`` and *clears* it by writing 1 to ``TOP_EN_CLEAR``.
  Reading either register returns the current ``TOP_EN`` bitmask.
  ``TOP_EN`` is not a latch; it just remembers what the host last set
  or cleared.

* The **MSI-edge AND-gate**: one per subtree, internal to ``INTR_CTRL``.
  It ANDs ``TOP[N]`` with ``TOP_EN[N]`` and drives the output
  through an edge detector. An MSI for subtree ``N`` is delivered
  on every *rising edge* of this AND output; level changes that
  drop the output to 0 (for any reason) deliver no MSI.

::

       LEAF[2N], LEAF[2N+1]         (sticky latches; W1C to clear)
              |
              v
       (OR of all 64 latched bits in subtree N)
              |
              v
        TOP[N] ----+
                   |
                   AND ---(rising edge detector)---> MSI for subtree N
                   |
       TOP_EN[N] --+
              ^
              |   host pokes:
              |     write 1 to TOP_EN_SET[N]   -> TOP_EN[N] becomes 1
              |     write 1 to TOP_EN_CLEAR[N] -> TOP_EN[N] becomes 0

With that in hand:

1. **REARM** (writing the subtree mask to ``TOP_EN_SET``) rescues a
   timing race: between the ISR's last leaf ack and the moment
   ``TOP_EN`` is brought back high, *new* engine events can arrive
   and latch fresh leaf bits. The ISR did its best to drain
   everything visible at the time of its W1C, but the W1C only
   clears the bits the ISR snapshotted; anything the engine fires
   afterwards sets new bits in ``LEAF[i]`` that the ISR never saw.

2. **INTR_RETRIGGER** (a per-engine register, not part of INTR_CTRL)
   rescues a still-asserted level source *inside an engine*. Most
   engines drive their internal "interrupt pending" signal as a
   level and convert it to an ``all2ctrl_intr_cmd`` message via an
   edge converter that fires only on the rising edge of that level.
   So one rising edge of the engine's level produces one message,
   which sets one leaf bit. After the host's W1C clears that leaf
   bit, a level that has stayed high produces no new edge, so the
   engine's edge converter never sends another message to INTR_CTRL,
   the leaf stays clear, and ``TOP[N]`` is 0. REARM's AND-gate trick
   is useless here. Writing 1 to the engine's
   ``INTR_RETRIGGER`` register drops the engine's level for one
   clock cycle; the level then returns to 1 (the engine still has
   work pending in its source register), the edge converter sees a
   fresh 0->1 transition, sends a new message, the leaf re-latches,
   ``TOP[N]`` goes back to 1, and an MSI follows on REARM (or
   immediately, if ``TOP_EN[N]`` was already 1). ``INTR_RETRIGGER``
   bridges the asymmetry between level-asserted internal engine
   logic and edge-driven ``INTR_CTRL`` leaf messages.

CPU doorbell self-test
======================

INTR_CTRL exposes a software-trigger register, ``NV_VF_INTR_LEAF_TRIGGER``.
Writing a vector number ``v`` to this register synthesizes a hardware
interrupt event on vector ``v``: the matching leaf bit latches, TOP
updates, and (assuming the subtree is armed and the leaf vector is
enabled) an MSI is delivered to the host.

nova-core uses vector 129 (``LEAF[4]`` bit 1) as a self-test "doorbell":
during early initialization, the driver registers a temporary ISR for vector
129, writes 129 to ``LEAF_TRIGGER``, and verifies that its ISR fires.
This validates the entire MSI -> INTR_CTRL -> ISR path *without* needing
the GSP firmware to be running, which makes it useful for debugging early
PCI / MSI issues, VFIO passthrough setups, and testing when GSP is not yet
available.
