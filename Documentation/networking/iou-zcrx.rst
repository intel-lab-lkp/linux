.. SPDX-License-Identifier: GPL-2.0

=====================
io_uring zero copy Rx
=====================

Introduction
============

io_uring zero copy Rx (ZC Rx) is a feature that removes kernel-to-user copy on
the network receive path, allowing packet data to be received directly into
userspace memory. This feature is different to TCP_ZEROCOPY_RECEIVE in that
there are no strict alignment requirements and no need to mmap()/munmap().
Compared to kernel bypass solutions such as e.g. DPDK, the packet headers are
processed by the kernel TCP stack as normal.

NIC HW Requirements
===================

Several NIC HW features are required for io_uring ZC Rx to work. For now the
kernel API does not configure the NIC and it must be done by the user.

Header/data split
-----------------

Required to split packets at the L4 boundary into a header and a payload.
Headers are received into kernel memory as normal and processed by the TCP
stack as normal. Payloads are received into userspace memory directly.

Flow steering
-------------

Specific HW Rx queues are configured for this feature, but modern NICs
typically distribute flows across all HW Rx queues. Flow steering is required
to ensure that only desired flows are directed towards HW queues that are
configured for io_uring ZC Rx.

RSS
---

In addition to flow steering above, RSS is required to steer all other non-zero
copy flows away from queues that are configured for io_uring ZC Rx.

Usage
=====

Setup NIC
---------

Must be done out of band for now.

Ensure there are at least two queues::

  ethtool -L eth0 combined 2

Enable header/data split::

  ethtool -G eth0 tcp-data-split on

Carve out half of the HW Rx queues for zero copy using RSS::

  ethtool -X eth0 equal 1

Set up flow steering, bearing in mind that queues are 0-indexed::

  ethtool -N eth0 flow-type tcp6 ... action 1

Setup io_uring
--------------

This section describes the low level io_uring kernel API. Please refer to
liburing documentation for how to use the higher level API.

Create an io_uring instance with the following required setup flags::

  IORING_SETUP_SINGLE_ISSUER
  IORING_SETUP_DEFER_TASKRUN
  IORING_SETUP_CQE32 or IORING_SETUP_CQE_MIXED

Create memory area
------------------

Allocate userspace memory area for receiving zero copy data::

  void *area_ptr = mmap(NULL, area_size,
                        PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE,
                        0, 0);

Create refill ring
------------------

Allocate memory for a shared ringbuf used for returning consumed buffers::

  void *ring_ptr = mmap(NULL, ring_size,
                        PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE,
                        0, 0);

This refill ring consists of some space for the header, followed by an array of
``struct io_uring_zcrx_rqe``::

  size_t rq_entries = 4096;
  size_t ring_size = rq_entries * sizeof(struct io_uring_zcrx_rqe) + PAGE_SIZE;
  /* align to page size */
  ring_size = (ring_size + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1);

Register ZC Rx
--------------

Fill in registration structs::

  struct io_uring_zcrx_area_reg area_reg = {
    .addr = (__u64)(unsigned long)area_ptr,
    .len = area_size,
    .flags = 0,
  };

  struct io_uring_region_desc region_reg = {
    .user_addr = (__u64)(unsigned long)ring_ptr,
    .size = ring_size,
    .flags = IORING_MEM_REGION_TYPE_USER,
  };

  struct io_uring_zcrx_ifq_reg reg = {
    .if_idx = if_nametoindex("eth0"),
    /* this is the HW queue with desired flow steered into it */
    .if_rxq = 1,
    .rq_entries = rq_entries,
    .area_ptr = (__u64)(unsigned long)&area_reg,
    .region_ptr = (__u64)(unsigned long)&region_reg,
  };

Register with kernel::

  io_uring_register_ifq(ring, &reg);

Map refill ring
---------------

The kernel fills in fields for the refill ring in the registration ``struct
io_uring_zcrx_ifq_reg``. Map it into userspace::

  struct io_uring_zcrx_rq refill_ring;

  refill_ring.khead = (unsigned *)((char *)ring_ptr + reg.offsets.head);
  refill_ring.khead = (unsigned *)((char *)ring_ptr + reg.offsets.tail);
  refill_ring.rqes =
    (struct io_uring_zcrx_rqe *)((char *)ring_ptr + reg.offsets.rqes);
  refill_ring.rq_tail = 0;
  refill_ring.ring_ptr = ring_ptr;

Receiving data
--------------

Prepare a zero copy recv request::

  struct io_uring_sqe *sqe;

  sqe = io_uring_get_sqe(ring);
  io_uring_prep_rw(IORING_OP_RECV_ZC, sqe, fd, NULL, 0, 0);
  sqe->ioprio |= IORING_RECV_MULTISHOT;

Now, submit and wait::

  io_uring_submit_and_wait(ring, 1);

Finally, process completions::

  struct io_uring_cqe *cqe;
  unsigned int count = 0;
  unsigned int head;

  io_uring_for_each_cqe(ring, head, cqe) {
    struct io_uring_zcrx_cqe *rcqe = (struct io_uring_zcrx_cqe *)(cqe + 1);

    unsigned long mask = (1ULL << IORING_ZCRX_AREA_SHIFT) - 1;
    unsigned char *data = area_ptr + (rcqe->off & mask);
    /* do something with the data */

    count++;
  }
  io_uring_cq_advance(ring, count);

Recycling buffers
-----------------

Return buffers back to the kernel to be used again::

  struct io_uring_zcrx_rqe *rqe;
  unsigned mask = refill_ring.ring_entries - 1;
  rqe = &refill_ring.rqes[refill_ring.rq_tail & mask];

  unsigned long area_offset = rcqe->off & ~IORING_ZCRX_AREA_MASK;
  rqe->off = area_offset | area_reg.rq_area_token;
  rqe->len = cqe->res;
  IO_URING_WRITE_ONCE(*refill_ring.ktail, ++refill_ring.rq_tail);

Notifications
-------------

When zero-copy receive encounters conditions that affect performance or
functionality, the kernel can notify userspace via dedicated CQE notifications.
The application must register a notification descriptor during
``IORING_REGISTER_ZCRX_IFQ`` to receive them.

Supported features can be detected by checking for ``ZCRX_FEATURE_NOTIFICATION``
in the features bitmask returned by ``IO_URING_QUERY_ZCRX``.

**Notification types**

``ZCRX_NOTIF_NO_BUFFERS``
  Fired when the page pool fails to allocate because the zcrx buffer area is
  exhausted.

``ZCRX_NOTIF_COPY``
  Fired when a received fragment could not be delivered zero-copy and was
  instead copied into a buffer.

**Registering notifications**

Allocate and fill a ``struct zcrx_notification_desc``::

  struct zcrx_notification_desc notif = {
    .user_data = MY_NOTIF_USER_DATA,
    .type_mask = ZCRX_NOTIF_NO_BUFFERS | ZCRX_NOTIF_COPY,
  };

  reg.notif_desc = (__u64)(unsigned long)&notif;

``user_data`` is the value that will appear in the notification CQE's
``user_data`` field. ``type_mask`` selects which notification types the
application wants to receive.

When a registered event occurs, the kernel posts a CQE with the specified
``user_data`` and ``cqe->res`` set to a bitmask of the triggered notification
types.

**Rate limiting**

Each notification type fires once until the application explicitly re-arms it.
To re-arm, issue ``IORING_REGISTER_ZCRX_CTRL`` with
``ZCRX_CTRL_ARM_NOTIFICATION``::

  struct zcrx_ctrl ctrl = {
    .zcrx_id = zcrx_id,
    .op = ZCRX_CTRL_ARM_NOTIFICATION,
    .zc_arm_notif = {
      .type_mask = ZCRX_NOTIF_NO_BUFFERS | ZCRX_NOTIF_COPY,
    },
  };

  io_uring_register(ring_fd, IORING_REGISTER_ZCRX_CTRL, &ctrl, 0);

Only notification types that have previously fired can be re-armed.

Notification statistics
-----------------------

In addition to CQE-based notifications, the kernel can maintain a shared-memory
statistics structure that is updated on every relevant event. All stats are
updated regardless of which notification flags were registered.

The statistics structure layout and alignment requirements can be queried via
``IO_URING_QUERY_ZCRX_NOTIF``. The application must query the structure size
and alignment requirements so that it allocates enough memory for the region
to fit both the refill ring and the stats structure.

To enable statistics, place the stats structure after the refill ring entries
within the same mapped region, and set the ``ZCRX_NOTIF_DESC_FLAG_STATS`` flag
in the notification descriptor::

  /* Compute offset for the stats struct (after refill ring entries) */
  size_t stats_offset = ring_size;
  ring_size += ALIGN_UP(sizeof(struct io_uring_zcrx_notif_stats), PAGE_SIZE);

  /* Map the region with the extra space */
  ring_ptr = mmap(NULL, ring_size, PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);

  struct zcrx_notification_desc notif = {
    .user_data = MY_NOTIF_USER_DATA,
    .type_mask = ZCRX_NOTIF_COPY,
    .flags = ZCRX_NOTIF_DESC_FLAG_STATS,
    .stats_offset = stats_offset,
  };

The ``stats_offset`` must satisfy the alignment reported by
``notif_stats_off_alignment`` and must point to a location within the mapped
region that does not overlap with the refill ring header or entries.

Application can read stat counters them at any time::

  volatile struct io_uring_zcrx_notif_stats *stats =
    (void *)((char *)ring_ptr + stats_offset);

  printf("copy fallbacks: %llu (%llu bytes)\n",
         IO_URING_READ_ONCE(stats->copy_count),
	 IO_URING_READ_ONCE(stats->copy_bytes));

``copy_count`` is incremented each time a fragment is copied instead of being
delivered via zero-copy. ``copy_bytes`` accumulates the total number of bytes
copied.

Area chunking
-------------

zcrx splits the memory area into fixed-length physically contiguous chunks.
This limits the maximum buffer size returned in a single io_uring CQE. Users
can provide a hint to the kernel to use larger chunks by setting the
``rx_buf_len`` field of ``struct io_uring_zcrx_ifq_reg`` to the desired length
during registration. If this field is set to zero, the kernel defaults to
the system page size.

To use larger sizes, the memory area must be backed by physically contiguous
ranges whose sizes are multiples of ``rx_buf_len``. It also requires kernel
and hardware support. If registration fails, users are generally expected to
fall back to defaults by setting ``rx_buf_len`` to zero.

Larger chunks don't give any additional guarantees about buffer sizes returned
in CQEs, and they can vary depending on many factors like traffic pattern,
hardware offload, etc. It doesn't require any application changes beyond zcrx
registration.

Testing
=======

See ``tools/testing/selftests/drivers/net/hw/iou-zcrx.c``
