.. SPDX-License-Identifier: GPL-2.0

===============================
FUSE Uring design documentation
==============================

This documentation covers basic details how the fuse
kernel/userspace communication through uring is configured
and works. For generic details about FUSE see fuse.rst.

This document also covers the current interface, which is
still in development and might change.

Limitations
===========
As of now not all requests types are supported through uring, userspace
is required to also handle requests through /dev/fuse after
uring setup is complete.  Specifically notifications (initiated from
the daemon side) and interrupts.

Fuse io-uring configuration
========================

Fuse kernel requests are queued through the classical /dev/fuse
read/write interface - until uring setup is complete.

In order to set up fuse-over-io-uring fuse-server (user-space)
needs to submit SQEs (opcode = IORING_OP_URING_CMD) to the
/dev/fuse connection file descriptor. Initial submit is with
the sub command FUSE_URING_REQ_FETCH, which will just register
entries to be available in the kernel.

Once at least one entry per queue is submitted, kernel starts
to enqueue to ring queues.
Note, every CPU core has its own fuse-io-uring queue.
Userspace handles the CQE/fuse-request and submits the result as
subcommand FUSE_URING_REQ_COMMIT_AND_FETCH - kernel completes
the requests and also marks the entry available again. If there are
pending requests waiting the request will be immediately submitted
to the daemon again.

Initial SQE
-----------

 |                                    |  FUSE filesystem daemon
 |                                    |
 |                                    |  >io_uring_submit()
 |                                    |   IORING_OP_URING_CMD /
 |                                    |   FUSE_URING_REQ_FETCH
 |                                    |  [wait cqe]
 |                                    |   >io_uring_wait_cqe() or
 |                                    |   >io_uring_submit_and_wait()
 |                                    |
 |  >fuse_uring_cmd()                 |
 |   >fuse_uring_fetch()              |
 |    >fuse_uring_ent_release()       |


Sending requests with CQEs
--------------------------

 |                                         |  FUSE filesystem daemon
 |                                         |  [waiting for CQEs]
 |  "rm /mnt/fuse/file"                    |
 |                                         |
 |  >sys_unlink()                          |
 |    >fuse_unlink()                       |
 |      [allocate request]                 |
 |      >__fuse_request_send()             |
 |        ...                              |
 |       >fuse_uring_queue_fuse_req        |
 |        [queue request on fg or          |
 |          bg queue]                      |
 |         >fuse_uring_assign_ring_entry() |
 |         >fuse_uring_send_to_ring()      |
 |          >fuse_uring_copy_to_ring()     |
 |          >io_uring_cmd_done()           |
 |          >request_wait_answer()         |
 |           [sleep on req->waitq]         |
 |                                         |  [receives and handles CQE]
 |                                         |  [submit result and fetch next]
 |                                         |  >io_uring_submit()
 |                                         |   IORING_OP_URING_CMD/
 |                                         |   FUSE_URING_REQ_COMMIT_AND_FETCH
 |  >fuse_uring_cmd()                      |
 |   >fuse_uring_commit_and_release()      |
 |    >fuse_uring_copy_from_ring()         |
 |     [ copy the result to the fuse req]  |
 |     >fuse_uring_req_end_and_get_next()  |
 |      >fuse_request_end()                |
 |       [wake up req->waitq]              |
 |      >fuse_uring_ent_release_and_fetch()|
 |       [wait or handle next req]         |
 |                                         |
 |                                         |
 |       [req->waitq woken up]             |
 |    <fuse_unlink()                       |
 |  <sys_unlink()                          |



