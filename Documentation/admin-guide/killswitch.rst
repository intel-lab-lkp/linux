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

Correct pattern: ``esp_input`` (CVE-2026-43284)
-----------------------------------------------

The IPsec ESP receive-path bug fixed by ``xfrm: esp: avoid in-place
decrypt on shared skb frags`` is reachable through ``esp_input()``
in ``net/ipv4/esp4.c`` (and ``esp6_input()`` for IPv6). Engage these
with retval ``-EINVAL``:

::

    echo "engage esp_input -22"  > /sys/kernel/security/killswitch/control
    echo "engage esp6_input -22" > /sys/kernel/security/killswitch/control

Inbound ESP packets are then dropped before decapsulation, neutering
any bug downstream of the ESP receive path. IPsec tunnels stop
working; other networking is unaffected.

Do not engage
=============

Do not killswitch:

* process or memory primitives the rest of the kernel needs to
  function: ``fork``, ``do_exit``, ``__alloc_pages``, ``kmalloc``,
  ``schedule``, anything in ``mm/`` reached by every allocation.
* hot paths in the scheduler, timekeeping, RCU, or interrupt entry.
* functions invoked from the killswitch path itself (``securityfs``,
  ``lockdown``, ``audit``, ``kprobe`` registration) -- the system
  may livelock or refuse to disengage.
* functions whose return value is read structurally (size, count,
  pointer-to-allocated-thing) rather than as success/failure.
  See the AF_ALG anti-patterns above for what goes wrong.

When in doubt, measure first.

Pre-soak before engaging
========================

If the target's call rate is unknown, attach a counter for a few
seconds first. With perf::

    perf probe --add 'esp_input'
    perf stat -a -e probe:esp_input -- sleep 5

Or with bpftrace::

    bpftrace -e 'kprobe:esp_input { @hits = count(); } interval:s:5 { exit(); }'

A target with ten thousand hits per second is not a candidate -- the
kernel will not survive five seconds with that path returning a
fixed error.

Relation to other facilities
============================

* ``CONFIG_FUNCTION_ERROR_INJECTION`` provides the same architecture
  trampoline (``override_function_with_return``), which killswitch
  reuses. fail_function is debug-oriented: targets must be
  pre-annotated with ``ALLOW_ERROR_INJECTION()`` in source, the
  override is probabilistic, and the interface is on debugfs (blocked
  under ``lockdown=integrity``). Killswitch is the production cousin:
  no whitelist, deterministic, securityfs-visible under integrity
  lockdown, with audit and taint.
* livepatch can do everything killswitch can and more, at the cost
  of building, signing, and shipping a kernel module per mitigation.
  Killswitch is for the window before that module exists.
* BPF override (``bpf_override_return``) needs a BPF program and
  ``CONFIG_BPF_KPROBE_OVERRIDE``; appropriate when the policy is
  conditional, overkill for "always return -EPERM".

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
