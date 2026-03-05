mq_timedreceive2 system call
=============================

This document describes the mq_timedreceive2() system call. It provides
an overview of the feature, its motivation, interface specification,
design, and test specification.

Contents
--------
        1) Overview
        2) Motivation
        3) Functional Specification
        4) Design
        5) Implementation Notes
        6) Test Specification

1) Overview
-----------

POSIX message queues on Linux provide mq_receive() and mq_timedreceive()
for consuming messages from a queue. Both interfaces require the caller
to pass the message buffer, length, and priority pointer as individual
arguments to the system call. This imposes a fixed calling convention
that cannot be extended without breaking the ABI.

mq_timedreceive2() introduces a new system call entry point that accepts
message buffer parameters via a struct argument rather than as individual
syscall arguments. This frees the remaining syscall argument slots for
new functionality flags and a message index, enabling non-destructive
peek and indexed access semantics that are not possible with the
original interface.

Two variants are provided:

  mq_timedreceive2()         - primary variant, 64-bit 
  mq_timedreceive2_time32()  - 32-bit time variant for legacy and compat

2) Motivation
-------------

The original mq_timedreceive() interface exhausts all usable syscall
argument slots with mqdes, msg_ptr, msg_len, msg_prio, and abs_timeout.
Adding new behavior such as peek-without-consume or indexed message
access would require either a new syscall with a completely different
signature, or overloading existing arguments in ways that harm clarity
and correctness.

By consolidating the message buffer parameters into a struct, this
syscall recovers argument slots for:

  flags   - controls peek vs consume and other future behavior
  index   - selects a specific message position in the priority queue

This design follows the precedent of other Linux syscalls that use
struct arguments to allow forward-compatible extensibility, such as
clone3() and openat2().

2.1 Non-destructive peek
~~~~~~~~~~~~~~~~~~~~~~~~

When the appropriate flag is set, mq_timedreceive2() copies the
message into the caller's buffer without removing it from the queue.
The queue state and priority ordering are fully preserved. This allows
applications to inspect a message before deciding whether to consume
it, eliminating the need to re-queue messages and the race conditions
that re-queuing introduces in multi-consumer scenarios.

2.2 Indexed access
~~~~~~~~~~~~~~~~~~

The index argument allows the caller to address a specific position
within the priority-ordered queue rather than always receiving the
highest priority message. This is useful for queue inspection,
debugging, and monitoring tools that need visibility into queue
contents without disturbing normal consumers.

3) Functional Specification
---------------------------

NAME
        mq_timedreceive2 - receive or peek at a message from a
        POSIX message queue

SYNOPSIS
        #include <mqueue.c>

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

        Note: As of now,no glibc wrapper exists for this syscall. Callers must
        invoke it directly using syscall().

DESCRIPTION
        mq_timedreceive2() receives or peeks at a message from the
        message queue referred to by the descriptor mqdes.

        The uargs structure provides the message buffer parameters:

          msg_ptr   userspace buffer to receive the message body
          msg_len   size of msg_ptr in bytes, must be >= mq_msgsize
                    of the queue
          msg_prio  if not NULL, the priority of the received message
                    is stored here

        The flags argument controls receive behavior. The following
        flags are defined:

          MQ_PEEK
                    Copy the message into msg_ptr without removing it
                    from the queue. The queue is not modified. If this
                    flag is not set, behavior is identical to
                    mq_timedreceive() and the message is consumed.

        The index argument selects which message to operate on within
        the priority-ordered queue. index 0 refers to the highest
        priority message. When MQ_PEEK is not set, index will be ignored but must be 0.

        The abs_timeout argument specifies an absolute timeout. If
        MQ_PEEK is set,this take control to peek path and so timeout 
        will be ignored otherwise it will behave as mq_timedreceive().


RETURN VALUE
        On success, returns the number of bytes copied into msg_ptr.
        On failure, returns -1 and sets errno.

ERRORS
        EAGAIN   Queue is empty and Operation Peek (O_NONBLOCK
                 equivalent behavior).

        EBADF    mqdes is not a valid message queue descriptor open
                 for reading.

        EFAULT   uargs, msg_ptr, msg_prio, or abs_timeout points to
                 an invalid address.
        EINVAL    Error to copy msg to temp buffer in kernel space

        EMSGSIZE msg_len is less than the mq_msgsize attribute of
                 the queue.

4) Design
---------

4.1 Struct-based argument passing
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The message buffer parameters (msg_ptr, msg_len, msg_prio) are
consolidated into struct mq_timedreceive2_args rather than passed
as individual syscall arguments. Linux syscalls are limited to six
arguments. The original mq_timedreceive() consumes all six slots,
leaving no room for extension. Consolidating the buffer parameters
into a struct recovers two argument slots for flags and index while
keeping the interface clean and forward-compatible.

Future extensions can be made by adding new flag bits without
requiring a new syscall number, following the same extensibility
model as openat2() and clone3().

4.2 Compat handling
~~~~~~~~~~~~~~~~~~~~

32-bit userspace on 64-bit kernels requires a separate compat
variant because pointer sizes and alignment differ between ABIs.
mq_timedreceive2_time32() accepts struct compat_mq_timedreceive2_args
which uses compat_uptr_t and compat_size_t in place of native pointer
and size_t types, and struct old_timespec32 for the timeout.

        struct compat_mq_timedreceive2_args {
                compat_size_t  msg_len;
                compat_uptr_t  msg_prio;
                compat_uptr_t  msg_ptr;
        };

The compat entry point performs the necessary conversions before
calling the shared do_mq_timedreceive2() implementation.

4.3 Peek implementation
~~~~~~~~~~~~~~~~~~~~~~~~

When MQ_PEEK is set, the implementation locates the target
message in the priority tree but does not remove it. Two locks are
taken: the first confirms a message exists before any allocation is
attempted, avoiding atomic allocation on empty queues. The second
protects the kernel temporary buffer copy operation.

4.4 Index argument
~~~~~~~~~~~~~~~~~~~

The priority tree is walked to the node at position index within
the priority ordering. index 0 is always the highest priority
message.

5) Implementation Notes
-----------------------

The implementation lives in ipc/mqueue.c. The two syscall entry
points mq_timedreceive2 and mq_timedreceive2_time32 are thin wrappers
that validate and convert arguments before calling the shared internal
function do_mq_timedreceive2(). This mirrors the existing pattern used
by mq_timedreceive and mq_timedreceive_time32.

Syscall numbers are assigned for all supported architectures. Refer
to the respective syscall table files under arch/ for per-architecture
numbers.

6) Test Specification
---------------------

Tests for mq_timedreceive2() should cover the following:

  1) Basic receive: verify that without MQ_PEEK the message is
     consumed and queue depth decreases by one. Verify message body
     and priority are correct.

  2) Peek semantics: verify that with MQ_PEEK the message body
     and priority are returned correctly and the queue depth is
     unchanged after the call.

  3) Repeated peek: verify that calling mq_timedreceive2() with
     MQ_PEEK multiple times on the same queue returns the same
     message each time without modifying the queue.

  4) Index argument: verify that index 0 returns the highest priority
     message.  Verify that out-of-range index returns ENOENT.

  5) Empty queue : verify that on an peek with empty queue return EAGAIN

  6) Memory behavior:Verify tiny and big message copy works as expected.

  7) Invalid arguments: verify EBADF for invalid mqdes, EFAULT for
     bad pointers in uargs, EINVAL for unknown flags, EMSGSIZE for
     msg_len smaller than queue mq_msgsize.

  9) Concurrent access: verify that simultaneous peek from multiple
     threads returns consistent results and does not corrupt queue
     state. Verify that a concurrent mq_receive() and peek() do
     not race or oops.