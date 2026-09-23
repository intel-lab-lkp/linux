.. SPDX-License-Identifier: GPL-2.0

=================================
The sparc32 compare-and-swap trap
=================================

Pre-v9 sparc has no compare-and-swap instruction.  A lock held inside one
process cannot make a word atomic against another process mapping the same
page, and a lock kept inside the word itself costs value bits, so the kernel
performs the operation instead.  parisc provides the same service for the same
reason.

Interface
=========

``ta 0x11`` - software trap 0x11, trap type 0x91, ``SP_TRAP_CAS`` in
``asm/traps.h`` - asks the kernel to compare and swap one 32-bit word:

=======  =========================================
``%o0``  word aligned user address
``%o1``  expected value
``%o2``  new value
=======  =========================================

On return the carry bit is clear and ``%o0`` holds the value that was found at
the address.  The caller compares it against what it expected to learn whether
the swap happened; there is no separate success flag.

Registers
---------

``%o0`` is read and written.  ``%o1`` and ``%o2`` are read and are not
modified.  Every other windowed and global register is preserved across the
trap.  The integer condition codes are written - that is how success and
failure are reported - and must be treated as clobbered.

An inline-assembly wrapper therefore wants ``%o0`` as an in/out operand,
``%o1`` and ``%o2`` as inputs, and both ``"cc"`` and ``"memory"`` in the
clobber list, the latter so the compiler does not move accesses across the
operation.

On failure the carry bit is set and ``%o0`` holds an error number:

==========  =========================================
``EINVAL``  the address is not word aligned
``EFAULT``  the memory access could not be completed
==========  =========================================

A successful return does not establish that the mapping is writable.  On the
software path the kernel writes only when the comparison succeeds, so a
mismatching value on a read-only mapping can be returned without the kernel
ever discovering that the mapping could not be written.  The two
implementations are not required to agree about an invalid operand: pass an
aligned word in memory that is both readable and writable.

The operation is atomic against another process issuing the same trap, against
the futex operations in ``asm/futex_32.h``, and - on a cpu that implements
``casa`` - against a binary using that instruction directly.

The service guarantees an indivisible compare-and-swap on the addressed word
under the participation rules below.  It does not provide any additional
acquire, release or full-barrier guarantee for other memory locations.
Userspace must supply whatever ordering the operation it is implementing
requires.

One kernel path does not participate.  ``futex_robust_unlock()`` clears the
futex word through ``unsafe_atomic_store_release_user()``, for which sparc32
uses the generic definition - ultimately a plain user store.  On a no-casa SMP
system that store does not acquire the lock this trap uses, so it can land
between the trap's load and its conditional store, and the compare-and-swap
then overwrites the unlock while still reporting success.

On no-casa SMP systems the non-PI robust-unlock operations
``FUTEX_UNLOCK_WAKE_LIST32`` and ``FUTEX_UNLOCK_BITSET_LIST32``, including
their private variants, therefore do not serialise with the software-CAS lock
domain.  Their store can race with a trap-based compare-and-swap, and equally
with the lock-backed operations in ``asm/futex_32.h``, which take the lock but
still load and store separately.

The integration point for this store remains an open question for review.

What userspace has to do as well
================================

On a cpu without ``casa`` the kernel does the swap under a lock of its own, and
**an ordinary store does not take that lock**::

	trap                        another thread's plain store

	lock
	read word -> A
	                            store B -> word
	write C -> word
	unlock

``B`` is lost, and no ordering of an atomic compare-and-swap and an atomic
store makes that a legal outcome.

So userspace using this service to build atomic operations must build *every*
atomic write from it, not just compare-and-exchange: an atomic store and an
atomic exchange each become a compare-and-swap retry loop.  An implementation
that routes read-modify-writes through the trap but leaves atomic stores as
plain stores is not atomic, and the failure is silent.

This does not apply to a cpu that has ``casa``, where the kernel uses the
instruction and userspace can too.

Replacing the ``__atomic_*`` helpers is not sufficient
-----------------------------------------------------

Providing replacement ``__atomic_store_4`` and ``__atomic_exchange_4`` symbols
does not by itself route every atomic write through the trap.  gcc can expand
an atomic store to a plain store, a 32-bit atomic exchange to ``swap``, and an
atomic test-and-set to ``ldstub`` on v8.  ``swap`` and ``ldstub`` are
themselves atomic, but they do not take the kernel's lock and so do not exclude
against its separate read and write.  A call the compiler never emits cannot be
intercepted.

Two different integration scopes follow from that, and they are not
interchangeable.  Changing libc's internal atomic macros covers libc's own
uses.  It does not change application code compiled with atomic builtins; that
needs compiler support - the possibility Andreas Larsson raised in 2019 - or a
build that avoids the inline expansions.

One aligned word is not general byte and halfword atomics
--------------------------------------------------------

The service operates on one naturally aligned 32-bit word.  A byte or halfword
atomic synthesised by read-modify-writing its containing word is not safe
merely because every atomic on the target uses the trap::

	word:  [ atomic byte A | ordinary byte B | ... ]

	cpu 0                             cpu 1

	read the whole word under the lock
	                                  plain store to B
	write the whole word back,
	  including the old B

The ordinary writer of the neighbouring object ``B`` has no reason to
participate, and its store is lost.  Byte and halfword atomics built this way
need either width-aware accesses sharing this lock domain, or storage rules
that give the atomic object its containing word to itself.

Availability
============

``AT_HWCAP`` carries ``HWCAP_SPARC_CASTRAP`` (0x10000000) when the kernel
implements the trap.  Userspace must consult it rather than probing, because
there is no safe way to probe:

- on a kernel without this support the trap is fatal;
- on sparc64 a 32-bit process reads its hwcap word from ``asm/elf_64.h``, where
  the bit is reserved and always clear, and ``ta 0x11`` there is not an illegal
  instruction but the old 64-bit system call trap.

The bit describes the kernel service, not the hardware.  It is set whether or
not the cpu has ``casa``, because a binary built for plain v8 cannot use that
instruction even on a cpu that has it.  A binary that does have ``casa``
available - one built for LEON3 - should use it inline and ignore the bit
entirely; the kernel uses the instruction too where it exists, so the two agree
on the same word.
