.. SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)

=================================
ULP direct data placement offload
=================================

Overview
========

The Linux kernel ULP direct data placement (DDP) offload infrastructure
provides tagged request-response protocols, such as NVMe-TCP, the ability to
place response data directly in pre-registered buffers according to header
tags. DDP is particularly useful for data-intensive pipelined protocols whose
responses may be reordered.

For example, in NVMe-TCP numerous read requests are sent together and each
request is tagged using the PDU header CID field. Receiving servers process
requests as fast as possible and sometimes responses for smaller requests
bypasses responses to larger requests, e.g., 4KB reads bypass 1GB reads.
Thereafter, clients correlate responses to requests using PDU header CID tags.
The processing of each response requires copying data from SKBs to read
request destination buffers; The offload avoids this copy. The offload is
oblivious to destination buffers which can reside either in userspace
(O_DIRECT) or in kernel pagecache.

Request TCP byte-stream:

.. parsed-literal::

 +---------------+-------+---------------+-------+---------------+-------+
 | PDU hdr CID=1 | Req 1 | PDU hdr CID=2 | Req 2 | PDU hdr CID=3 | Req 3 |
 +---------------+-------+---------------+-------+---------------+-------+

Response TCP byte-stream:

.. parsed-literal::

 +---------------+--------+---------------+--------+---------------+--------+
 | PDU hdr CID=2 | Resp 2 | PDU hdr CID=3 | Resp 3 | PDU hdr CID=1 | Resp 1 |
 +---------------+--------+---------------+--------+---------------+--------+

The driver builds SKB page fragments that point to destination buffers.
Consequently, SKBs represent the original data on the wire, which enables
*transparent* inter-operation with the network stack. To avoid copies between
SKBs and destination buffers, the layer-5 protocol (L5P) will check
``if (src == dst)`` for SKB page fragments, success indicates that data is
already placed there by NIC hardware and copy should be skipped.

In addition, L5P might have DDGST which ensures data integrity over
the network.  If not offloaded, ULP DDP might not be efficient as L5P
will need to go over the data and calculate it by itself, cancelling
out the benefits of the DDP copy skip.  ULP DDP has support for Rx/Tx
DDGST offload. On the received side the NIC will verify DDGST for
received PDUs and update SKB->ulp_ddp and SKB->ulp_crc bits.  If all the SKBs
making up a L5P PDU have crc on, L5P will skip on calculating and
verifying the DDGST for the corresponding PDU. On the Tx side, the NIC
will be responsible for calculating and filling the DDGST fields in
the sent PDUs.

Offloading does require NIC hardware to track L5P protocol framing, similarly
to RX TLS offload (see Documentation/networking/tls-offload.rst).  NIC hardware
will parse PDU headers, extract fields such as operation type, length, tag
identifier, etc. and only offload segments that correspond to tags registered
with the NIC, see the :ref:`buf_reg` section.

Device configuration
====================

During driver initialization the driver sets the ULP DDP operations
for the :c:type:`struct net_device <net_device>` via
`netdev->netdev_ops->ulp_ddp_ops`.

The :c:member:`get_caps` operation returns the ULP DDP capabilities
enabled and/or supported by the device to the caller. The current list
of capabilities is represented as a bitset:

.. code-block:: c

  enum ulp_ddp_cap {
	ULP_DDP_CAP_NVME_TCP,
	ULP_DDP_CAP_NVME_TCP_DDGST,
  };

The enablement of capabilities can be controlled via the
:c:member:`set_caps` operation. This operation is exposed to userspace
via netlink. See Documentation/netlink/specs/ulp_ddp.yaml for more
details.

Later, after the L5P completes its handshake, the L5P queries the
driver for its runtime limitations via the :c:member:`limits` operation:

.. code-block:: c

 int (*limits)(struct net_device *netdev,
	       struct ulp_ddp_limits *lim);


All L5P share a common set of limits and parameters (:c:type:`struct ulp_ddp_limits <ulp_ddp_limits>`):

.. code-block:: c

 /**
  * struct ulp_ddp_limits - Generic ulp ddp limits: tcp ddp
  * protocol limits.
  * Add new instances of ulp_ddp_limits in the union below (nvme-tcp, etc.).
  *
  * @type:		type of this limits struct
  * @max_ddp_sgl_len:	maximum sgl size supported (zero means no limit)
  * @io_threshold:	minimum payload size required to offload
  * @tls:		support for ULP over TLS
  * @nvmeotcp:		NVMe-TCP specific limits
  */
 struct ulp_ddp_limits {
	enum ulp_ddp_type	type;
	int			max_ddp_sgl_len;
	int			io_threshold;
	bool			tls:1;
	union {
		/* ... protocol-specific limits ... */
		struct nvme_tcp_ddp_limits nvmeotcp;
	};
 };

But each L5P can also add protocol-specific limits e.g.:

.. code-block:: c

 /**
  * struct nvme_tcp_ddp_limits - nvme tcp driver limitations
  *
  * @full_ccid_range:	true if the driver supports the full CID range
  */
 struct nvme_tcp_ddp_limits {
	bool			full_ccid_range;
 };

Once the L5P has made sure the device is supported the offload
operations are installed on the socket.

If offload installation fails, then the connection is handled by software as if
offload was not attempted.

To request offload for a socket `sk`, the L5P calls :c:member:`sk_add`:

.. code-block:: c

 int (*sk_add)(struct net_device *netdev,
	       struct sock *sk,
	       struct ulp_ddp_config *config);

The function return 0 for success. In case of failure, L5P software should
fallback to normal non-offloaded operations.  The `config` parameter indicates
the L5P type and any metadata relevant for that protocol. For example, in
NVMe-TCP the following config is used:

.. code-block:: c

 /**
  * struct nvme_tcp_ddp_config - nvme tcp ddp configuration for an IO queue
  *
  * @pfv:        pdu version (e.g., NVME_TCP_PFV_1_0)
  * @cpda:       controller pdu data alignment (dwords, 0's based)
  * @dgst:       digest types enabled.
  *              The netdev will offload crc if L5P data digest is supported.
  * @queue_size: number of nvme-tcp IO queue elements
  * @queue_id:   queue identifier
  */
 struct nvme_tcp_ddp_config {
	u16			pfv;
	u8			cpda;
	u8			dgst;
	int			queue_size;
	int			queue_id;
 };

When offload is not needed anymore, e.g. when the socket is being released, the L5P
calls :c:member:`sk_del` to release device contexts:

.. code-block:: c

 void (*sk_del)(struct net_device *netdev,
	        struct sock *sk);

Normal operation
================

At the very least, the device maintains the following state for each connection:

 * 5-tuple
 * expected TCP sequence number
 * mapping between tags and corresponding buffers
 * current offset within PDU, PDU length, current PDU tag

NICs should not assume any correlation between PDUs and TCP packets.
If TCP packets arrive in-order, offload will place PDU payloads
directly inside corresponding registered buffers. NIC offload should
not delay packets. If offload is not possible, than the packet is
passed as-is to software. To perform offload on incoming packets
without buffering packets in the NIC, the NIC stores some inter-packet
state, such as partial PDU headers.

RX data-path
------------

After the device validates TCP checksums, it can perform DDP offload.  The
packet is steered to the DDP offload context according to the 5-tuple.
Thereafter, the expected TCP sequence number is checked against the packet
TCP sequence number. If there is a match, offload is performed: the PDU payload
is DMA written to the corresponding destination buffer according to the PDU header
tag.  The data should be DMAed only once, and the NIC receive ring will only
store the remaining TCP and PDU headers.

We remark that a single TCP packet may have numerous PDUs embedded inside. NICs
can choose to offload one or more of these PDUs according to various
trade-offs. Possibly, offloading such small PDUs is of little value, and it is
better to leave it to software.

Upon receiving a DDP offloaded packet, the driver reconstructs the original SKB
using page frags, while pointing to the destination buffers whenever possible.
This method enables seamless integration with the network stack, which can
inspect and modify packet fields transparently to the offload.

.. _buf_reg:

Destination buffer registration
-------------------------------

To register the mapping between tags and destination buffers for a socket
`sk`, the L5P calls :c:member:`setup` of :c:type:`struct ulp_ddp_dev_ops
<ulp_ddp_dev_ops>`:

.. code-block:: c

 int (*setup)(struct net_device *netdev,
	      struct sock *sk,
	      struct ulp_ddp_io *io);


The `io` provides the buffer via scatter-gather list (`sg_table`) and
corresponding tag (`command_id`):

.. code-block:: c

 /**
  * struct ulp_ddp_io - tcp ddp configuration for an IO request.
  *
  * @command_id:  identifier on the wire associated with these buffers
  * @nents:       number of entries in the sg_table
  * @sg_table:    describing the buffers for this IO request
  * @first_sgl:   first SGL in sg_table
  */
 struct ulp_ddp_io {
	u32			command_id;
	int			nents;
	struct sg_table		sg_table;
	struct scatterlist	first_sgl[SG_CHUNK_SIZE];
 };

After the buffers have been consumed by the L5P, to release the NIC mapping of
buffers the L5P calls :c:member:`teardown` of :c:type:`struct
ulp_ddp_dev_ops <ulp_ddp_dev_ops>`:

.. code-block:: c

 void (*teardown)(struct net_device *netdev,
		  struct sock *sk,
		  struct ulp_ddp_io *io,
		  void *ddp_ctx);

`teardown` receives the same `io` context and an additional opaque
`ddp_ctx` that is used for asynchronous teardown, see the :ref:`async_release`
section.

.. _async_release:

Asynchronous teardown
---------------------

To teardown the association between tags and buffers and allow tag reuse NIC HW
is called by the NIC driver during `teardown`. This operation may be
performed either synchronously or asynchronously. In asynchronous teardown,
`teardown` returns immediately without unmapping NIC HW buffers. Later,
when the unmapping completes by NIC HW, the NIC driver will call up to L5P
using :c:member:`ddp_teardown_done` of :c:type:`struct ulp_ddp_ulp_ops <ulp_ddp_ulp_ops>`:

.. code-block:: c

 void (*ddp_teardown_done)(void *ddp_ctx);

The `ddp_ctx` parameter passed in `ddp_teardown_done` is the same on provided
in `teardown` and it is used to carry some context about the buffers
and tags that are released.

Resync handling
===============

RX
--
In presence of packet drops or network packet reordering, the device may lose
synchronization between the TCP stream and the L5P framing, and require a
resync with the kernel's TCP stack. When the device is out of sync, no offload
takes place, and packets are passed as-is to software. Resync is very similar
to TLS offload (see documentation at Documentation/networking/tls-offload.rst)

If only packets with L5P data are lost or reordered, then resynchronization may
be avoided by NIC HW that keeps tracking PDU headers. If, however, PDU headers
are reordered, then resynchronization is necessary.

To resynchronize hardware during traffic, we use a handshake between hardware
and software. The NIC HW searches for a sequence of bytes that identifies L5P
headers (i.e., magic pattern).  For example, in NVMe-TCP, the PDU operation
type can be used for this purpose.  Using the PDU header length field, the NIC
HW will continue to find and match magic patterns in subsequent PDU headers. If
the pattern is missing in an expected position, then searching for the pattern
starts anew.

The NIC will not resume offload when the magic pattern is first identified.
Instead, it will request L5P software to confirm that indeed this is a PDU
header. To request confirmation the NIC driver calls up to L5P using
:c:member:`resync_request` of :c:type:`struct ulp_ddp_ulp_ops <ulp_ddp_ulp_ops>`:

.. code-block:: c

  bool (*resync_request)(struct sock *sk, u32 seq, u32 flags);

The `seq` parameter contains the TCP sequence of the last byte in the PDU header.
The `flags` parameter contains a flag (`ULP_DDP_RESYNC_PENDING`) indicating whether
a request is pending or not.
L5P software will respond to this request after observing the packet containing
TCP sequence `seq` in-order. If the PDU header is indeed there, then L5P
software calls the NIC driver using the :c:member:`resync` function of
the :c:type:`struct ulp_ddp_dev_ops <ulp_ddp_ops>` inside the :c:type:`struct
net_device <net_device>` while passing the same `seq` to confirm it is a PDU
header.

.. code-block:: c

 void (*resync)(struct net_device *netdev,
		struct sock *sk, u32 seq);

Statistics
==========

Per L5P protocol, the NIC driver must report statistics for the above
netdevice operations and packets processed by offload.
These statistics are per-device and can be retrieved from userspace
via netlink (see Documentation/netlink/specs/ulp_ddp.yaml).

For example, NVMe-TCP offload reports:

 * ``rx_nvme_tcp_sk_add`` - number of NVMe-TCP Rx offload contexts created.
 * ``rx_nvme_tcp_sk_add_fail`` - number of NVMe-TCP Rx offload context creation
   failures.
 * ``rx_nvme_tcp_sk_del`` - number of NVMe-TCP Rx offload contexts destroyed.
 * ``rx_nvme_tcp_setup`` - number of DDP buffers mapped.
 * ``rx_nvme_tcp_setup_fail`` - number of DDP buffers mapping that failed.
 * ``rx_nvme_tcp_teardown`` - number of DDP buffers unmapped.
 * ``rx_nvme_tcp_drop`` - number of packets dropped in the driver due to fatal
   errors.
 * ``rx_nvme_tcp_resync`` - number of packets with resync requests.
 * ``rx_nvme_tcp_packets`` - number of packets that used offload.
 * ``rx_nvme_tcp_bytes`` - number of bytes placed in DDP buffers.

NIC requirements
================

NIC hardware should meet the following requirements to provide this offload:

 * Offload must never buffer TCP packets.
 * Offload must never modify TCP packet headers.
 * Offload must never reorder TCP packets within a flow.
 * Offload must never drop TCP packets.
 * Offload must not depend on any TCP fields beyond the
   5-tuple and TCP sequence number.
