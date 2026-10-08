.. SPDX-License-Identifier: GPL-2.0

===========================
Passive optical network ONU
===========================

Overview
========

``net/pon`` is the kernel side of an optical network unit, the subscriber end
of a passive optical network. One optical line terminal in the exchange serves
many ONUs over a shared fiber. Downstream is a broadcast the ONU filters.
Upstream is time division multiple access and an ONU may only transmit inside
the windows the OLT grants it.

The subsystem implements ITU-T G.9807.1 (XGS-PON): the PLOAM codec and the
identifier ranges are those of G.9807.1. The uapi also
reserves the modes of G.984 (GPON) and G.987 (XG-PON) for later drivers, as
well as the activation state o3 of G.984.3. The core implements none of those
systems. A device lists the modes it supports in its capabilities. The core
refuses any other mode. The subsystem does not cover EPON, which is IEEE
802.3ah and a different access method.

The model
=========

Three objects carry the datapath and they nest:

Device
  One PON MAC. It has an identity the OLT authenticates, a mode and an
  activation state.

T-CONT
  A transmission container: the entity the OLT grants upstream windows to,
  named by an alloc-id the OLT assigns over PLOAM. Upstream bandwidth is
  arbitrated between T-CONTs by the OLT and within one T-CONT by the ONU.

GEM port
  A flow, named by a GEM port id the OLT assigns over OMCI. Several GEM ports
  ride one T-CONT. A GEM port id is a label in the frame header, not a
  scheduling entity. G.9807.1 Table C.6.6 reserves 0 to 1020 for the default
  GEM port (equal to the ONU-ID, OMCI only) and 65535 for the idle GEM port,
  so the uapi takes only 1021 to 65534.

One more object supports them:

Classifier rule
  What upstream traffic maps onto which GEM port, matched on VLAN tag state,
  VLAN id, priority or DSCP.

Activation
==========

An ONU reaches the operational state O5 through the PLOAM state machine of
G.984.3 or G.9807.1: serial number exchange, ONU-ID assignment, ranging and
finally O5, which is the only state in which the OMCI management channel
carries traffic.

**The state machine runs in the driver, not in the core.** That is a
deliberate choice and the reason this subsystem has no software MAC layer.
On the hardware it was written against, the key hierarchy of G.9807.1 clause
C.15.3 runs inside the MAC's key generator and the OMCI integrity key never
leaves it, so no generic layer can compute the OMCI message integrity check.
A core that owned the state machine would be a core that could not finish the
job.

What the core owns instead is the vocabulary and the object model:

* the ITU-T constants and message layouts, in ``include/net/pon/ploam.h``
* the PLOAM message codec, which builds and parses standard message bodies and
  holds no state, no timer and no policy
* the objects above, which it stores
* the activation state, which the driver reports and the core validates and
  publishes
* the network devices, their carrier and the OMCI channel
* the netlink uapi

The driver owns the state machine, its timers, the key hierarchy and every
register.

Why the core validates the state
--------------------------------

``ploam-ntf`` and the activation state in the netlink reply are uapi. If each
driver published its own state, each vendor would invent its own notification
timing and an OMCI daemon would behave differently per MAC. So the driver
reports through ``pon_dev_state_report()`` and the core checks the transition
against the state machine of the standard, publishes it and updates the
carrier. An out of range value is refused. An illegal edge is published
anyway and warned about: the hardware is the truth and a driver that reports
something the standard does not permit is a bug to find, not a state to hide.

Everything the uapi carries follows the ITU-T recommendations. Where a MAC
deviates from them, its driver translates the hardware to the standard before
it reports, as a quirk of that driver. The core and the uapi never carry a
vendor state or a vendor meaning. The states are the union of G.984.3 and
G.9807.1: G.9807.1 has one Serial Number state, O2-3 (Table C.12.1), which a
driver reports as ``o2``. Only a G.984.3 device reports ``o3``.

The core logs every edge it publishes at info level, one line per edge, for
example ``pon0: PLOAM state O5 -> O6``. The log names the Serial Number state
``O2-3`` in every mode but G-PON. A repeated report logs nothing. A work
item outside the instance's context prints the line, so a slow console never
delays the PLOAM exchange. A driver logs its own lines the same way through
``pon_dev_log()``. All lines keep their order.

Objects outlive the link
========================

A GEM port is OMCI configuration and the ONU keeps it when the link goes and
a new activation starts: G.9807.1 clause C.6.1.5.8 has the ONU retain every
XGEM port id the OMCI assigned when it enters O1 and discard only the default
one that carries the OMCC. So the driver keeps its GEM ports across a loss of
the link and the core keeps the objects, exactly as an address on an ethernet
device outlives a cable pull. Nothing is handed back when the link returns,
because nothing was taken away.

What the ONU does discard are the alloc-ids (clause C.6.1.5.7). The OLT assigns
them again over PLOAM once the ONU is back in O5 and the driver binds each one
to a transmit channel and reports that to the core through
``pon_dev_event()``. The default alloc-id, equal to the ONU-ID, is not assigned
by a message. The driver binds it with the ONU-ID and it may carry user traffic
as well as the OMCC. A GEM port whose
alloc-id has no channel yet is held and carries no traffic. The carrier of the
PON interfaces is up in O5 and O6 while at least one GEM port rides an alloc-id
that has a channel and a conduit (see below) is paired with the device and up.
The core follows the conduit with a netdev notifier: the carrier falls when the
conduit starts to go down and when its driver takes it back.

Classifier rules are objects of their own. In G.988 they come from their own
managed entities, which reference a GEM port rather than belonging to it, so a
rule outlives the GEM port it names, in the core and in the driver.

The lent context
================

A PON MAC runs the activation state machine itself, but it must not run it in
hard interrupt context and it must not run it against a half finished netlink
transaction. The core therefore lends the driver its own serialized context:
one ordered workqueue per instance and one lock that the netlink handlers
take in their ``pre_doit``.

A driver queues a ``pon_work`` from any context, hard interrupt included. The
handler runs with the instance lock held, so it may sleep. One worker drains
the list one item at a time, so the ordering a driver sees is the order it
queued in.

The OMCI channel
================

OMCI is the management protocol of G.988 and it is userspace's job. The kernel
carries it over the netlink family, the way nl80211 carries management frames
to hostapd. A daemon sends ``omci-register`` for a device and from then on
receives every OMCI PDU of that device as an ``omci-ntf`` message, sent to
its socket alone. It sends a PDU with ``omci-tx``. One socket owns the OMCI
channel of a device at a time: another socket is refused with ``EBUSY`` and
only the owner may send. The registration ends when the socket closes, so a
daemon that restarts registers again and nothing is left behind.

A PDU crosses the boundary bare, from the transaction correlation id to the
end of the message contents, in both directions and in both the baseline and
the extended format. It carries no integrity field, because the MAC computes
the upstream one and checks the downstream one. A PDU is therefore at most
1976 bytes: G.988 clause 11.2.5 limits an extended message to 1980 bytes
and 4 of them are the integrity field.

The downstream check works like a receive checksum offload. The MAC that
checks the integrity field says so in its receive descriptor, per PDU and
the core takes such a PDU as verified and stripped. A PDU the MAC passed up
unchecked still ends with its integrity field, so the core accepts it up to
1980 bytes and hands it to the driver's ``omci_verify``, which checks and
strips the field. That check cannot be a generic helper of the core: the
field is an AES-CMAC with the OMCI integrity key (G.9807.1 clause C.15.7.2) and
the key belongs to the key hierarchy the driver runs. On some MACs it never
leaves the key generator.

No network device and no
EtherType is involved: the OMCC carries the PDU on its own GEM port with no
Ethernet header and the core knows that port, so there is nothing to
classify.

A received PDU waits in the instance's context until it reaches the owner. A
PDU that finds no owner, a full queue or a full socket is dropped. The OLT
retries. The exchange can be captured on an ``nlmon`` device like any other
netlink traffic.

No OMCI managed entity is modeled in the kernel. The daemon reads the OLT's
intent from the MIB and expresses it through the netlink family below.

The conduit
===========

A PON MAC on a system on chip has no DMA of its own: its frames ride the rings
of an ethernet port of the same chip. That port is the conduit, in the sense
DSA gives the word and its driver registers it with ``pon_conduit_register()``
when the port's firmware node names the MAC through a ``pon-handle``
reference. The core pairs the two by that node, in whichever order they
probe and neither waits for the other.

Per frame information crosses the boundary as explicit arguments and never in
``skb->cb`` or in a frame tag. On transmit the MAC driver names the GEM port,
the T-CONT channel, the queue and, for an OMCI PDU, the integrity key index
and the conduit's driver builds its descriptor from them. On receive the
conduit's driver reads the GEM port and the OMCI and integrity flags from its
descriptor and hands the frame to ``pon_conduit_rx()``, which delivers it to
the OMCI channel, to the GEM port's own network device when it has one, or to
the PON data interface. The MAC driver verifies an OMCI PDU the hardware
passed up unchecked through ``omci_verify``. Without that callback such a PDU
is dropped. Such a PDU still carries its integrity field, so the core accepts
it at up to 1980 bytes and the conduit's MTU never drops below that.

The conduit keeps the address of the PON data interface, because the frame
engine behind it recognizes the ONU's frames by the conduit's own address.
The core sets it when the two are paired. When the data interface's address
changes afterwards, the MAC driver calls ``pon_conduit_addr_set()`` from its
``ndo_set_mac_address``.

The conduit also takes the largest MTU among the PON network devices, because
the frames of all of them ride its rings. The core sets it when the two are
paired and when a GEM port's network device is created or changes its MTU. The
MAC driver calls ``pon_conduit_mtu_set()`` from the ``ndo_change_mtu`` of the
data interface. A change that the conduit's driver refuses is refused.

Netlink interface
=================

The generic netlink family ``pon`` is specified in
``Documentation/netlink/specs/pon.yaml`` and the uapi header is generated from
it. Every object can be listed, watched and read back:

=================  ==============================================
``dev-get``        devices, with a dump
``tcont-get``      T-CONTs, with a dump
``gem-get``        GEM ports, with a dump
``gem-map-get``    classifier rules, a dump
=================  ==============================================

Notifications share the reply format of the matching get, so a listener parses
one message shape whether it asked for an object or was told about it. There
is no sequence number: a listener that overruns its socket re-reads.
``dev-del-ntf`` stands for the removal of every object of that device. The
objects go without a notification of their own.

The dumps of the T-CONTs, the GEM ports and the classifier rules take an
optional device ID and then list the objects of that device only. A
device ID that names no device ends the dump with ``-ENODEV``. A dump that
does not fit one message resumes by count, so the kernel sets
``NLM_F_DUMP_INTR`` when an object joined or left a list in between and the
reader repeats the dump.

``omci-register``, ``omci-tx`` and ``omci-ntf`` are the OMCI channel, described
above. ``omci-ntf`` goes to the owner of the channel alone and to no
multicast group.

``ploam-ntf`` reports every activation transition.

An operation is a ``set`` when it can change an object that already exists
and a ``new`` when it can only create one. ``tcont-set`` rebinds a T-CONT,
while ``gem-new`` and ``gem-map-new`` refuse an object that exists with other
attributes.

A GEM port reaches an alloc-id only through its T-CONT. ``tcont-set`` that
changes the alloc-id moves the GEM ports of the T-CONT too. If the driver
refuses one, the core restores the old alloc-id on the T-CONT and on its GEM
ports and returns the error. ``tcont-del`` answers ``-EBUSY`` while a GEM port
names the T-CONT, so userspace deletes the GEM ports first. ``tcont-set``
answers ``-EEXIST`` for an alloc-id that another T-CONT holds: G.988 clause
9.2.2 relates alloc-ids and T-CONTs one to one, so userspace that moves an
alloc-id releases it first.

``dev-set`` refuses to start the link until userspace gave the device a serial
number. The driver carries no serial of its own, so an ONU that has not been
provisioned stays silent rather than range under a vendor default.

No key material crosses this interface in either direction. ``gem-new`` selects
the key ring of each GEM port, as G.988 clause 9.2.3 defines it. The key
ring alone decides whether a GEM port is encrypted and in which direction. The
keys themselves are derived and rotated below. The broadcast key ring is
refused, because the OLT distributes those keys over the OMCI and the kernel
takes none.

Internals
=========

The kernel-internal interfaces, pulled from the source.

.. kernel-doc:: net/pon/pon_main.c
   :doc: PON locking

.. kernel-doc:: net/pon/pon_work.c
   :doc: The lent context

.. kernel-doc:: include/net/pon/ploam.h
   :doc: The PLOAM vocabulary

.. kernel-doc:: include/net/pon/ploam_msg.h
   :doc: The PLOAM message codec

.. kernel-doc:: include/net/pon/types.h
   :internal:

.. kernel-doc:: include/net/pon/functions.h
