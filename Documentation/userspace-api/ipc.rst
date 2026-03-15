mq_timedreceive2 system call
=============================

This document describes the mq_timedreceive2() system call. It provides
an overview of the feature, interface specification, design, and
test specification.

Contents
--------

        1) Overview
        2) Functional Specification
        3) Design
        4) Implementation Notes
        5) Test Specification

1) Overview
-----------

POSIX message queues on Linux provide mq_timedreceive() for consuming
messages from a queue.This interface requires the caller to pass the
message buffer, length and priority pointer as individual arguments to
the system call. This imposes a fixed calling convention that cannot be
extended without breaking the ABI.

mq_timedreceive2() introduces a new system call entry point that accepts
message buffer parameters via a struct argument rather than as individual
syscall arguments. This frees the remaining syscall argument slots for
new functionality flags and a message index, enabling non-destructive
peek and indexed access semantics that are not possible with the
original interface.

One 64-bit variant is provided with compat handling:
    mq_timedreceive2()

2) Functional Specification
---------------------------

NAME
        mq_timedreceive2 - receive or peek at a message from a
        POSIX message queue

SYNOPSIS

.. code-block:: c

        #include <mqueue.h>

        struct mq_timedreceive2_args {
                size_t         msg_len;
                unsigned int  *msg_prio;
                char          *msg_ptr;
        };

        ssize_t mq_timedreceive2(mqd_t mqdes,
                                 struct mq_timedreceive2_args *uargs,
                                 unsigned int flags,
                                 unsigned long index,
                                 const struct timespec *abs_timeout);

Note: No glibc wrapper exists for this syscall. Callers must invoke it
directly using syscall(2).

DESCRIPTION
        mq_timedreceive2() receives or peeks at a message from the
        message queue referred to by the descriptor mqdes.

        The uargs structure provides the message buffer parameters:

        ``msg_ptr``
                Userspace buffer to receive the message body.

        ``msg_len``
                Size of msg_ptr in bytes. Must be greater than or equal
                to the mq_msgsize attribute of the queue.

        ``msg_prio``
                If not NULL, the priority of the received message is
                stored here.

        The flags argument controls receive behavior. The following
        flag is defined:

        ``MQ_PEEK``
                Copy the message into msg_ptr without removing it from
                the queue. The queue is not modified. If this flag is
                not set, behavior is identical to mq_timedreceive() and
                the message is consumed.

        The index argument selects which message to operate on within
        the priority-ordered queue. index 0 refers to the highest
        priority message. When MQ_PEEK is not set, index is ignored
        but must be non-empty.

        The abs_timeout argument specifies an absolute timeout. When
        MQ_PEEK is set, abs_timeout is ignored since peek is a
        non-blocking snapshot operation. When MQ_PEEK is not set,
        abs_timeout behaves identically to mq_timedreceive().

RETURN VALUE
        On success, returns the number of bytes copied into msg_ptr.
        On failure, returns -1 and sets errno.

ERRORS
        ``EAGAIN``
                Queue is empty and MQ_PEEK is set. Peek is always
                non-blocking and returns immediately on empty queue.

        ``EBADF``
                mqdes is not a valid message queue descriptor open
                for reading.

        ``EFAULT``
                uargs, msg_ptr, msg_prio, or abs_timeout points to
                an invalid address.

        ``EINVAL``
                flags contains an unknown value, or index is nonzero
                and MQ_PEEK is not set.

        ``EMSGSIZE``
                msg_len is less than the mq_msgsize attribute of
                the queue.

        ``ETIMEDOUT``
                Pop path only. The call timed out before a message
                became available. Never returned on peek path.

3) Design
---------

3.1 Struct-based argument passing
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The message buffer parameters (msg_ptr, msg_len, msg_prio) are
consolidated into struct mq_timedreceive2_args rather than passed
as individual syscall arguments. Due to limited six arguments,
The original mq_timedreceive() consumes all six slots,
leaving no room for extension. Consolidating the buffer parameters
into a struct recovers two argument slots for flags and index while
keeping the interface clean and forward-compatible.

Future extensions can be made by adding new flag bits without
requiring a new syscall number.

3.2 Compat handling
~~~~~~~~~~~~~~~~~~~~

.. code-block:: c

        struct compat_mq_timedreceive2_args {
                compat_size_t  msg_len;
                compat_uptr_t  msg_prio;
                compat_uptr_t  msg_ptr;
        };

The compat entry point performs the necessary conversions before
calling the shared do_mq_timedreceive2() implementation.

3.3 Peek implementation
~~~~~~~~~~~~~~~~~~~~~~~~

When MQ_PEEK is set, the implementation locates the target message
in the priority tree but does not remove it. Two locks are taken:
the first confirms a message exists before any allocation is
attempted, avoiding allocation on empty queues. The second protects
the kernel temporary buffer copy operation. The message is copied to
userspace and both locks are released with the queue unmodified.

3.4 Index argument
~~~~~~~~~~~~~~~~~~~

The priority tree is walked to the node at position index within
the priority ordering. index 0 is always the highest priority
message, consistent with what an unconditional mq_timedreceive()
would return.

4) Implementation Notes
-----------------------

The implementation lives in ipc/mqueue.c. The syscall entry
point mq_timedreceive2 are thin wrappers that validate and convert
arguments before calling the shared internal function do_mq_timedreceive2().

Syscall numbers are assigned for all of most common architectures. Refer
to the respective syscall table files under arch/ for per-architecture
numbers.

5) Test Specification
---------------------

Tests for mq_timedreceive2() should cover the following:

1) Basic receive: verify that without MQ_PEEK the message is consumed
   and queue depth decreases by one. Verify message body and priority
   are correct.

2) Peek semantics: verify that with MQ_PEEK the message body and
   priority are returned correctly and the queue depth is unchanged
   after the call.

3) Repeated peek: verify that calling mq_timedreceive2() with MQ_PEEK
   multiple times on the same queue returns the same message each time
   without modifying the queue.

4) Index argument: verify that index 0 returns the highest priority
   message. Verify that out-of-range index returns ENOENT.

5) Empty queue: verify that peek on an empty queue returns EAGAIN
   immediately without blocking.

6) Memory behavior: verify that both small and large message copies
   work correctly without corruption.

7) Invalid arguments: verify EBADF for invalid mqdes, EFAULT for bad
   pointers in uargs, EINVAL for unknown flags, EMSGSIZE for msg_len
   smaller than queue mq_msgsize.

8) Concurrent access: verify that simultaneous peek from multiple
   threads returns consistent results and does not corrupt queue
   state. Verify that a concurrent mq_receive() and mq_peek() do
   not race.
