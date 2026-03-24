.. SPDX-License-Identifier: GPL-2.0

=======================================
FUSE-over-io-uring design documentation
=======================================

This documentation covers basic details how the fuse
kernel/userspace communication through io-uring is configured
and works. For generic details about FUSE see fuse.rst.

This document also covers the current interface, which is
still in development and might change.

Limitations
===========
As of now not all requests types are supported through io-uring, userspace
is required to also handle requests through /dev/fuse after io-uring setup
is complete. Specifically notifications (initiated from the daemon side)
and interrupts.

Fuse io-uring configuration
===========================

Fuse kernel requests are queued through the classical /dev/fuse
read/write interface - until io-uring setup is complete.

In order to set up fuse-over-io-uring fuse-server (user-space)
needs to submit SQEs (opcode = IORING_OP_URING_CMD) to the /dev/fuse
connection file descriptor. Initial submit is with the sub command
FUSE_URING_REQ_REGISTER, which will just register entries to be
available in the kernel.

Once at least one entry per queue is submitted, kernel starts
to enqueue to ring queues.
Note, every CPU core has its own fuse-io-uring queue.
Userspace handles the CQE/fuse-request and submits the result as
subcommand FUSE_URING_REQ_COMMIT_AND_FETCH - kernel completes
the requests and also marks the entry available again. If there are
pending requests waiting the request will be immediately submitted
to the daemon again.

Initial SQE
-----------::

 |                                    |  FUSE filesystem daemon
 |                                    |
 |                                    |  >io_uring_submit()
 |                                    |   IORING_OP_URING_CMD /
 |                                    |   FUSE_URING_CMD_REGISTER
 |                                    |  [wait cqe]
 |                                    |   >io_uring_wait_cqe() or
 |                                    |   >io_uring_submit_and_wait()
 |                                    |
 |  >fuse_uring_cmd()                 |
 |   >fuse_uring_register()           |


Sending requests with CQEs
--------------------------::

 |                                           |  FUSE filesystem daemon
 |                                           |  [waiting for CQEs]
 |  "rm /mnt/fuse/file"                      |
 |                                           |
 |  >sys_unlink()                            |
 |    >fuse_unlink()                         |
 |      [allocate request]                   |
 |      >fuse_send_one()                     |
 |        ...                                |
 |       >fuse_uring_queue_fuse_req          |
 |        [queue request on fg queue]        |
 |         >fuse_uring_add_req_to_ring_ent() |
 |         ...                               |
 |          >fuse_uring_copy_to_ring()       |
 |          >io_uring_cmd_done()             |
 |       >request_wait_answer()              |
 |         [sleep on req->waitq]             |
 |                                           |  [receives and handles CQE]
 |                                           |  [submit result and fetch next]
 |                                           |  >io_uring_submit()
 |                                           |   IORING_OP_URING_CMD/
 |                                           |   FUSE_URING_CMD_COMMIT_AND_FETCH
 |  >fuse_uring_cmd()                        |
 |   >fuse_uring_commit_fetch()              |
 |    >fuse_uring_commit()                   |
 |     >fuse_uring_copy_from_ring()          |
 |      [ copy the result to the fuse req]   |
 |     >fuse_uring_req_end()                 |
 |      >fuse_request_end()                  |
 |       [wake up req->waitq]                |
 |    >fuse_uring_next_fuse_req              |
 |       [wait or handle next req]           |
 |                                           |
 |       [req->waitq woken up]               |
 |    <fuse_unlink()                         |
 |  <sys_unlink()                            |

Kernel-managed buffer rings
===========================

Kernel-managed buffer rings have two main advantages:

* eliminates the overhead of pinning/unpinning user pages and translating
  virtual addresses for every server-kernel interaction
* reduces buffer memory allocation requirements

In order to use buffer rings, the server must preregister the following:

* a kernel-managed buffer ring
* a registered memory region large enough to hold the headers for the ents

Please refer to libfuse's lib/fuse_uring.c for an example of how to set this
up.

At a high-level, this is how fuse uses buffer rings:

* The server registers a kernel-managed buffer ring. In the kernel this
  allocates the pages needed for the buffers and vmaps them. The server
  obtains the virtual address for the buffers through an mmap call on the ring
  fd.
* When the server creates a queue, it passes in the headers offset, which
  specifies the offset into the registered memory region where the headers
  reside.
* When there is a request from a client, fuse will select a buffer from the
  ring if there is any payload that needs to be copied, copy over the payload
  to the selected buffer, and copy over the headers to the registered memory
  region. Each ent has its own id (passed through sqe->buf_index at setup
  time) that communicates where in the memory region the headers for that ent
  resides.
* The server obtains a cqe representing the request. The cqe flag will have
  IORING_CQE_F_BUFFER set if a selected buffer was used for the payload. The
  buffer id is stashed in cqe->flags (through IORING_CQE_BUFFER_SHIFT). The
  server can directly access the payload by using that buffer id to calculate
  the offset into the virtual address obtained for the buffers.
* The server processes the request and then sends a
  FUSE_URING_CMD_COMMIT_AND_FETCH sqe with the reply.
* When the kernel handles the sqe, it will process the reply and if there is a
  next request, it will reuse the same selected buffer for the request. If
  there is no next request, it will recycle the buffer back to the ring.

Zero-copy
=========

Fuse io-uring zero-copy allows the server to directly read from / write to the
client's pages and bypass any intermediary buffer copies. This is only allowed
on privileged servers. Access to these pages (eg reads/writes by the server)
must go through the io-uring interface.

In order to use zero-copy, the server must use kernel-managed buffer rings. It
will also need to preregister a sparse buffer for every ent in the queue. This
is where the client's pages will reside.

When the client issues a read/write, fuse stores the client's underlying pages
in the sparse buffer entry corresponding to the ent in the queue. The server
can then issue reads/writes on these pages through io_uring rw operations.
The pages are unregistered once the server replies to the request.
Non-zero-copyable payload (if needed) is placed in a buffer from the
kernel-managed buffer ring.
