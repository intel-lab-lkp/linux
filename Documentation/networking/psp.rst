.. SPDX-License-Identifier: GPL-2.0-only

=====================
PSP Security Protocol
=====================

Protocol
========

PSP Security Protocol (PSP) was defined at Google and published in:

https://raw.githubusercontent.com/google/psp/main/doc/PSP_Arch_Spec.pdf

This section briefly covers protocol aspects crucial for understanding
the kernel API. Refer to the protocol specification for further details.

Note that the kernel implementation and documentation uses the term
"device key" in place of "master key", it is both less confusing
to an average developer and is less likely to run afoul any naming
guidelines.

Derived Rx keys
---------------

PSP borrows some terms and mechanisms from IPsec. PSP was designed
with HW offloads in mind. The key feature of PSP is that Rx keys for every
connection do not have to be stored by the receiver but can be derived
from device key and information present in packet headers.
This makes it possible to implement receivers which require a constant
amount of memory regardless of the number of connections (``O(1)`` scaling).

Tx keys have to be stored like with any other protocol, but Tx is much
less latency sensitive than Rx, and delays in fetching keys from slow
memory is less likely to cause packet drops. Preferably, the Tx keys
should be provided with the packet (e.g. as part of the descriptors).

Key rotation
------------

The device key known only to the receiver is fundamental to the design.
Per specification this state cannot be directly accessible (it must be
impossible to read it out of the hardware of the receiver NIC).
Moreover, it has to be "rotated" periodically (usually daily). Rotation
means that new device key gets generated (by a random number generator
of the device), and used for all new connections. To avoid disrupting
old connections the old device key remains in the NIC. A phase bit
carried in the packet headers indicates which generation of device key
the packet has been encrypted with.

User facing API
===============

PSP is designed primarily for hardware offloads. There is currently
no software fallback for systems which do not have PSP capable NICs.
There is also no standard (or otherwise defined) way of establishing
a PSP-secured connection or exchanging the symmetric keys.

The expectation is that higher layer protocols will take care of
protocol and key negotiation. For example one may use TLS key exchange,
announce the PSP capability, and switch to PSP if both endpoints
are PSP-capable.

All configuration of PSP is performed via the PSP netlink family.

Device discovery
----------------

The PSP netlink family defines operations to retrieve information
about the PSP devices available on the system, configure them and
access PSP related statistics.

Securing a connection
---------------------

PSP encryption is currently only supported for TCP connections.
Rx and Tx keys are allocated separately. First the ``rx-assoc``
Netlink command needs to be issued, specifying a target TCP socket.
Kernel will allocate a new PSP Rx key from the NIC and associate it
with given socket. At this stage socket will accept both PSP-secured
and plain text TCP packets.

Tx keys are installed using the ``tx-assoc`` Netlink command.
Once the Tx keys are installed, all data read from the socket will
be PSP-secured. In other words act of installing Tx keys has a secondary
effect on the Rx direction.

There is an intermediate period after ``tx-assoc`` successfully
returns and before the TCP socket encounters it's first PSP
authenticated packet, where the TCP stack will allow certain nondata
packets, i.e. ACKs, FINs, and RSTs, to enter TCP receive processing
even if not PSP authenticated. During the ``tx-assoc`` call, the TCP
socket's ``rcv_nxt`` field is recorded. At this point, ACKs and RSTs
will be accepted with any sequence number, while FINs will only be
accepted at the latched value of ``rcv_nxt``. Once the TCP stack
encounters the first TCP packet containing PSP authenticated data, the
other end of the connection must have executed the ``tx-assoc``
command, so any TCP packet, including those without data, will be
dropped before receive processing if it is not successfully
authenticated. This is summarized in the table below. The
aforementioned state of rejecting all non-PSP packets is labeled "PSP
Full".

+----------------+------------+------------+-------------+-------------+
| Event          | Normal TCP | Rx PSP     | Tx PSP      | PSP Full    |
+================+============+============+=============+=============+
| Rx plain       | accept     | accept     | drop        | drop        |
| (data)         |            |            |             |             |
+----------------+------------+------------+-------------+-------------+
| Rx plain       | accept     | accept     | accept      | drop        |
| (ACK|FIN|RST)  |            |            |             |             |
+----------------+------------+------------+-------------+-------------+
| Rx PSP (good)  | drop       | accept     | accept      | accept      |
+----------------+------------+------------+-------------+-------------+
| Rx PSP (bad    | drop       | drop       | drop        | drop        |
| crypt, !=SPI)  |            |            |             |             |
+----------------+------------+------------+-------------+-------------+
| Tx             | plain text | plain text | encrypted   | encrypted   |
|                |            |            | (excl. rtx) | (excl. rtx) |
+----------------+------------+------------+-------------+-------------+

To ensure that any data read from the socket after the ``tx-assoc``
call returns success has been authenticated, the kernel will scan the
receive and ofo queues of the socket at ``tx-assoc`` time. If any
enqueued packet was received in clear text, the Tx association will
fail, and the application should retry installing the Tx key after
draining the socket (this should not be necessary if both endpoints
are well behaved).

Because TCP sequence numbers are not integrity protected prior to
upgrading to PSP, it is possible that a MITM could offset sequence
numbers in a way that deletes a prefix of the PSP protected part of
the TCP stream. If userspace cares to mitigate this type of attack, a
special "start of PSP" message should be exchanged after ``tx-assoc``.

Queue steering
--------------

The PSP header may carry an optional 64 bit "virtualization cookie" (VC).
The protocol assigns it no meaning in transport mode, so Linux uses it to
let the two ends of a connection tell each other which Rx queue they want
traffic delivered to. The cookie carries two queue IDs::

   63           48 47           32 31           16 15            0
  +---------------+---------------+---------------+---------------+
  |    reserved   |    req qid    |    reserved   |    dst qid    |
  +---------------+---------------+---------------+---------------+

``req`` is the Rx queue the sender is asking the peer to send to, and
``dst`` is the queue this packet is to be delivered to, which holds the
``req`` the sender last saw from the peer. Each side's request is what
becomes the other side's destination. ``0xffff`` means "no queue" and
reserved bits must be zero.

Each ID gets a 32 bit word to itself, of which only the low half is used.
Queue counts fit in 16 bits today; should that stop being true, an ID can
grow into the reserved half of its word without the fields moving.

The two directions are enabled independently, with ``vc-steer-ena``:

 * ``rx`` asks peers to send to the queue paired with the flow's Tx queue,
   so traffic this host *receives* gets steered. This needs
   ``vc-steer-cap``: the driver is required to arrange the appropriate Rx
   steering, and whatever rules it uses to do so are implicit, not visible
   to the user. They have lower priority than any explicitly configured
   flow steering, but do take precedence over the RSS table.
 * ``tx`` grants the requests peers make, so traffic this host *sends*
   gets steered at the far end. The queue is the peer's to choose and the
   rules are the peer's to install, so this needs no device support at
   all, and can be turned on even where ``vc-steer-cap`` is absent.

Enabling either direction grows the PSP header by the size of the cookie,
and the MSS shrinks accordingly. The setting is therefore sampled when an
association is created; changing it later applies to new associations
only, and existing connections keep the header size they were set up
with.

The queue the local end asks for is refreshed from the Tx queue the stack
picks for the flow, assuming that Rx and Tx queues are paired by index.

Trust model
~~~~~~~~~~~

VC steering as implemented is not robust against queue DDoS attacks, that
is a coordinated overload of a single Rx queue, because any peer can name
any queue. The expectation is that PSP is not used to talk to untrusted
peers while VC steering is enabled. Note that use of the cookie requires
PSP, so the *machine* as a whole may still talk to untrusted peers, as
long as it hands out no PSP keys to them.

The intended way of handling a mix of trusted and untrusted peers is to
extend the queue ID and stop using it as a direct index, making it a per
queue cookie instead. An untrusted peer should then not be able to guess
a tag which was never communicated to it, and the secrets can be rotated
periodically and gradually, one queue at a time. It is important that
changes to the implementation do not prevent this future extension.

Rotation notifications
----------------------

The rotations of device key happen asynchronously and are usually
performed by management daemons, not under application control.
The PSP netlink family will generate a notification whenever keys
are rotated. The applications are expected to re-establish connections
before keys are rotated again.

Kernel implementation
=====================

Driver notes
------------

Drivers are expected to start with no PSP enabled (``psp-versions-ena``
in ``dev-get`` set to ``0``) whenever possible. The user space should
not depend on this behavior, as future extension may necessitate creation
of devices with PSP already enabled, nonetheless drivers should not enable
PSP by default. Enabling PSP should be the responsibility of the system
component which also takes care of key rotation.

Note that ``psp-versions-ena`` is expected to be used only for enabling
receive processing. The device is not expected to reject transmit requests
after ``psp-versions-ena`` has been disabled. User may also disable
``psp-versions-ena`` while there are active associations, which will
break all PSP Rx processing.

Drivers are expected to ensure that a device key is usable and secure
upon init, without explicit key rotation by the user space. It must be
possible to allocate working keys, and that no duplicate keys must be
generated. If the device allows the host to request the key for an
arbitrary SPI - driver should discard both device keys (rotate the
device key twice), to avoid potentially using a SPI+key which previous
OS instance already had access to.

Drivers must use ``psp_skb_get_assoc_rcu()`` to check if PSP Tx offload
was requested for given skb. On Rx drivers should allocate and populate
the ``SKB_EXT_PSP`` skb extension, and set the skb->decrypted bit to 1.

Every driver has to carry the cookie, not just those which advertise
``vc-steer-cap`` - granting a peer's request needs no help from the
device, so the ``tx`` direction of ``vc-steer-ena`` may be turned on
anywhere. Drivers must ask ``psp_assoc_vc_tx_get()`` for the cookie to
place in the Tx header, and report the queue IDs a received cookie held
in ``psp_skb_ext.vc_req`` and ``vc_dst`` (``psp_dev_rcv()`` does this for
drivers which let the core strip the headers). Reporting is what allows
the core to grant the peer's request. The steering itself is only
expected of drivers which advertise ``vc-steer-cap``.

When VC steering is enabled GRO implementations are allowed to ignore
changes in the cookie for transport mode PSP.

Kernel implementation notes
---------------------------

PSP implementation follows the TLS offload more closely than the IPsec
offload, with per-socket state, and the use of skb->decrypted to prevent
clear text leaks.

PSP device is separate from netdev, to make it possible to "delegate"
PSP offload capabilities to software devices (e.g. ``veth``).
