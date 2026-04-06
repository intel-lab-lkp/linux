.. SPDX-License-Identifier: GPL-2.0
.. Copyright © 2026 Cloudflare

=====================
Landlock Trace Events
=====================

:Date: April 2026

Landlock emits trace events for sandbox lifecycle operations and access
denials.  These events can be consumed by ftrace (for human-readable
trace output and filtering) and by eBPF programs (for programmatic
introspection via BTF).

See Documentation/security/landlock.rst for Landlock kernel internals and
Documentation/admin-guide/LSM/landlock.rst for system administration.

.. warning::

   Landlock trace events, like audit records, expose sensitive
   information about all sandboxed processes on the system.  See
   :ref:`landlock_observability_security` for security considerations
   and privilege requirements.

See Documentation/userspace-api/landlock.rst for the userspace API.

Event overview
==============

Landlock trace events are organized in four categories:

**Syscall events** are emitted during Landlock system calls:

- ``landlock_create_ruleset``: a new ruleset is created
- ``landlock_add_rule_fs``: a filesystem rule is added to a ruleset
- ``landlock_add_rule_net``: a network port rule is added to a ruleset
- ``landlock_restrict_self``: a new domain is created from a ruleset

**Denial events** are emitted when an access is denied:

- ``landlock_deny_access_fs``: filesystem access denied
- ``landlock_deny_access_net``: network access denied
- ``landlock_deny_ptrace``: ptrace access denied
- ``landlock_deny_scope_signal``: signal delivery denied
- ``landlock_deny_scope_abstract_unix_socket``: abstract unix socket
  access denied

**Rule evaluation events** are emitted during rule matching:

- ``landlock_check_rule_fs``: a filesystem rule is evaluated
- ``landlock_check_rule_net``: a network port rule is evaluated

**Lifecycle events**:

- ``landlock_free_domain``: a domain is freed
- ``landlock_free_ruleset``: a ruleset is freed

Enabling events
===============

Enable all Landlock events::

    echo 1 > /sys/kernel/tracing/events/landlock/enable

Enable a specific event::

    echo 1 > /sys/kernel/tracing/events/landlock/landlock_deny_access_fs/enable

Read the trace output::

    cat /sys/kernel/tracing/trace_pipe

Differences from audit records
==============================

Tracepoints and audit records both log Landlock denials, but differ
in some field formats:

- **Paths**: Tracepoints use ``d_absolute_path()`` (namespace-independent
  absolute paths).  Audit uses ``d_path()`` (relative to the process's
  chroot).  Tracepoint paths are deterministic regardless of the tracer's
  mount namespace.

- **Device names**: Tracepoints use numeric ``dev=<major>:<minor>``.
  Audit uses string ``dev="<s_id>"``.  Numeric format is more precise
  for machine parsing.

- **Denied access field**: The ``deny_access_fs`` and ``deny_access_net``
  tracepoints use the ``blockers=`` field name (same as audit).
  Audit uses human-readable access right names (e.g.,
  ``blockers=fs.read_file``), while tracepoints use a hex bitmask
  (e.g., ``blockers=0x4``).  Scope and ptrace tracepoints omit
  ``blockers`` because the event name identifies the denial type.

- **Scope target names**: Tracepoints use role-specific field names
  (``tracee_pid``, ``target_pid``, ``peer_pid``) that reflect the
  semantic of each event.  Audit uses generic names (``opid``, ``ocomm``)
  because the audit log format is not event-type-specific.

- **Process name**: Scope tracepoints include ``comm=`` in the printk
  output for stateless consumers.  eBPF consumers can read ``comm``
  directly from the task_struct via BTF.  The ``comm`` value is treated
  as untrusted input (escaped via ``__print_untrusted_str``).

Ruleset versioning
==================

Syscall events include a ruleset version (``ruleset=<hex_id>.<version>``)
that tracks the number of rules added to the ruleset.  The version is
incremented on each ``landlock_add_rule()`` call and frozen at
``landlock_restrict_self()`` time.  This enables trace consumers to
correlate a domain with the exact set of rules it was created from.

eBPF access
===========

eBPF programs attached via ``BPF_RAW_TRACEPOINT`` can access the
tracepoint arguments directly through BTF.  The arguments include both
standard kernel objects and Landlock-internal objects:

- Standard kernel objects (``struct task_struct``, ``struct sock``,
  ``struct path``, ``struct dentry``) can be used with existing BPF
  helpers.
- Landlock-internal objects (``struct landlock_domain``,
  ``struct landlock_ruleset``, ``struct landlock_rule``,
  ``struct landlock_hierarchy``) can be read via ``BPF_CORE_READ``.
  Internal struct layouts may change between kernel versions; use CO-RE
  for field relocation.

All pointer arguments in the tracepoint prototypes are guaranteed
non-NULL.

Audit filtering equivalence
============================

Denial events include ``same_exec``, ``log_same_exec``, and
``log_new_exec`` fields.  These allow both stateless (ftrace filter)
and stateful (eBPF) consumers to replicate the audit subsystem's
filtering logic::

    # Show only denials that audit would also log:
    echo 'same_exec==1 && log_same_exec==1 || same_exec==0 && log_new_exec==1' > \
        /sys/kernel/tracing/events/landlock/landlock_deny_access_fs/filter

Event reference
===============

.. kernel-doc:: include/trace/events/landlock.h
    :doc: Landlock trace events

.. kernel-doc:: include/trace/events/landlock.h
    :internal:

Additional documentation
========================

* Documentation/userspace-api/landlock.rst
* Documentation/admin-guide/LSM/landlock.rst
* Documentation/security/landlock.rst
* https://landlock.io
