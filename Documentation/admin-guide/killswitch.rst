.. SPDX-License-Identifier: GPL-2.0
..
.. Copyright (C) 2026 Sasha Levin <sashal@kernel.org>

============
Killswitch
============

Killswitch lets a privileged operator make a chosen kernel function
return a fixed value without executing its body, as a temporary
mitigation for a security bug while a real fix is being prepared.

The function returns the operator-supplied value and nothing else
runs in its place. There is no allowlist, no return-type check; if
the kprobe layer accepts the symbol, killswitch engages it. Once
engaged, the change is in effect on every CPU until ``disengage`` is
written or the system reboots.

Configuration
=============

``CONFIG_KILLSWITCH``
  Enables the feature. Depends on ``SECURITYFS``, ``KPROBES`` (with
  ftrace support), and ``FUNCTION_ERROR_INJECTION``.

The interface
=============

::

    /sys/kernel/security/killswitch/
        engaged                 RO  currently-engaged functions
        control                 WO  command sink
        taint                   RO  0 or 1
        fn/<name>/              per-function directory, created on engage
            retval              RW  return value
            hits                RO  per-cpu summed call count

Three commands are accepted by ``control``::

    engage <symbol> <retval>
    disengage <symbol>
    disengage_all

Each engage and disengage emits a single ``KERN_WARNING`` line to
dmesg with the symbol, retval, hit count (on disengage), and the
operator's identity (uid/auid/sessionid/comm, or ``source=cmdline``).

Engagement is rejected when:

* the symbol is unknown, in a non-traceable section, on the kprobe
  blacklist, or otherwise refused by ``register_kprobe`` (the error
  from the kprobe layer is logged and returned to userspace);
* the symbol is already engaged (``-EBUSY``);
* the operator does not hold ``CAP_SYS_ADMIN``.

Whatever value the operator writes is what the function returns.
Writing the wrong type or wrong value lands in the caller as-is.

Boot parameter
==============

``killswitch=fn1=<val>,fn2=<val>,...``

Parsed early; engagements are applied at the end of kernel init
once the kprobe subsystem is up. Parse failures emit a warning and
skip the offending entry; they never panic.

Useful for fleet rollout: when an issue drops, ship the mitigation
in the bootloader / PXE config and roll the fleet through reboots
while the real fix is being prepared.

Tainting
========

The first successful engagement (runtime or boot-time) sets
``TAINT_KILLSWITCH`` (bit 20, char ``H``). The taint persists across
``disengage`` until reboot, so an oops on a killswitch-modified
kernel is identifiable from the banner: ``Tainted: ... H`` tells a
maintainer to consult ``engaged`` before further triage.

Module unload
=============

If a module containing an engaged target is unloaded, killswitch
auto-disengages the entry and emits a ``KERN_WARNING`` so the loss
of mitigation is visible. Reloading the module does not silently
re-arm the killswitch; the operator re-engages explicitly.

Choosing the right target
=========================

A function that *looks* skippable may be relied on by callers for a
side effect (a lock the caller releases, a refcount the caller
drops, a scatterlist the caller consumes). The rule of thumb:

  Pick the **highest-level** entry point that contains the bug.

That gives callers no chance to dereference half-initialised state
from a function whose body was skipped. Two illustrative examples
from ``crypto/af_alg.c``:

Anti-pattern: ``af_alg_count_tsgl``
-----------------------------------

``af_alg_count_tsgl()`` returns ``unsigned int`` (the number of TX
SG entries). Engaging it with retval ``0`` causes the caller in
``algif_aead.c`` to allocate a 1-entry scatterlist (its
``if (!entries) entries = 1`` guard) and then walk the *real* TX
SGL into that undersized destination via ``af_alg_pull_tsgl``,
producing out-of-bounds writes. **Killswitching here introduces a
worse bug than the one being mitigated.**

Anti-pattern: ``af_alg_pull_tsgl``
----------------------------------

``af_alg_pull_tsgl()`` returns ``void``, so any retval is accepted.
But its caller depends on the per-request SGL being filled in.
Skipping the body leaves the per-request SGL with NULL pages; the
next-stage ``memcpy_sglist`` dereferences them and the kernel
oopses.

Correct pattern: ``af_alg_sendmsg``
-----------------------------------

``af_alg_sendmsg()`` is the highest-level entry into the AF_ALG
send path. Engaging it with retval ``-EPERM`` causes every send
attempt to return -EPERM to userspace; no caller ever sees
half-initialised state, and any AF_ALG-reachable bug downstream of
``sendmsg`` is unreachable until the killswitch is disengaged.

The canonical pattern: pick a syscall-handler-shaped function whose
return value already encodes "this operation didn't happen", and
let userspace handle the error as it would any other failed
syscall.

Safety notes
============

* In-flight calls during ``write()`` to ``control`` may run either
  the original body or the override. The override is ``return X``,
  which has no preconditions to violate.
* SMP visibility comes from ``text_poke_bp()``. ``write()`` to
  ``control`` returns only after every CPU sees the new path.
* The ftrace ops unregister waits for in-flight pre-handlers, so
  freeing the engagement attribute on disengage is safe.
* Inline functions, freed ``__init`` symbols, and anything compiled
  away cannot be killswitched. ``register_kprobe`` rejects them
  with whatever error the kprobe layer chooses.

Diagnostics
===========

Per-call hits are aggregated in a per-cpu counter readable at
``/sys/kernel/security/killswitch/fn/<name>/hits``. Per-hit logging
is not provided to avoid log storms on hot paths.

A ``KILLSWITCH`` entry appears in the kernel taint vector once any
engagement succeeds (also visible as ``H`` in the oops banner).
