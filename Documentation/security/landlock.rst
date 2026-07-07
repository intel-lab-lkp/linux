.. SPDX-License-Identifier: GPL-2.0
.. Copyright © 2017-2020 Mickaël Salaün <mic@digikod.net>
.. Copyright © 2019-2020 ANSSI
.. Copyright © 2026 Cloudflare, Inc.

=========================
Landlock: Security design
=========================

:Author: Mickaël Salaün
:Date: July 2026

Landlock's goal is to create scoped access-control (i.e. sandboxing).  To
harden a whole system, this feature should be available to any process,
including unprivileged ones.  Because such a process may be compromised or
backdoored (i.e. untrusted), Landlock's features must be safe to use from the
kernel and other processes point of view.  Landlock's interface must therefore
expose a minimal attack surface.

Landlock is designed to be usable by unprivileged processes while following the
system security policy enforced by other access control mechanisms (e.g. DAC,
LSM).  A Landlock rule shall not interfere with other access-controls enforced
on the system, only add more restrictions.

Any user can enforce Landlock rulesets on their processes.  They are merged and
evaluated against inherited rulesets in a way that ensures that only more
constraints can be added.

User space documentation can be found here:
Documentation/userspace-api/landlock.rst.

Threat model
============

Landlock lets any process, even an unprivileged one, restrict itself.  Its
threat model therefore treats a sandboxed process as potentially malicious. The
adversary is a sandboxed process that tries to perform an action its own policy
should deny.  A Landlock security bug is when it succeeds.  This complements
Documentation/process/threat-model.rst.

What Landlock protects
----------------------

A Landlock *domain* is a ruleset (a set of rules), or a stack of them, enforced
on a task (a thread), attached to its credentials and inherited across
:manpage:`fork(2)` and :manpage:`execve(2)`.  A sandboxed *subject* (a task
restricted by a domain) is confined in the *actions* it may perform, such as
accessing files, binding or connecting to network ports, or sending signals.  A
ruleset declares the *handled* actions it restricts, in one of two shapes:
access rights and *scopes* that the running kernel supports.

An access right allows specific accesses to a target named by a rule.  Currently
a target is a file descriptor, which designates one kernel object: the inode it
references, or for a directory the file hierarchy beneath it.  It can instead be
a network port, a value matching any socket using it rather than a specific
object.  Any access a handled right covers but does not explicitly allow is
denied.

A scope instead restricts crossing out of the domain hierarchy, denying outgoing
interaction with processes or IPC peers that are neither in the subject's domain
nor in a domain nested under it (e.g. sending signals or connecting to abstract
UNIX sockets).

Beyond these two declared shapes, a domain also carries restrictions Landlock
imposes implicitly, which a policy does not select.  It may :manpage:`ptrace(2)`
only a target confined by its own domain or by a domain nested under it
(necessarily more restricted); tracing a less restricted, unrelated, or
unsandboxed target is denied regardless of the policy.  A domain restricting the
filesystem also denies changes to the filesystem topology and, by default,
reparenting a file to a different directory, so inode-based rules cannot be
bypassed by relocating a file (see `Limitations are not security bugs`_).
Actions that are neither handled nor implicitly restricted are left to the
system's other access controls; likewise, semantics that live only in user
space, such as a service's own handling of the requests it receives, are beyond
a kernel mechanism's reach.  Landlock's coverage (the set of restrictable
actions) grows over time, extended without changing the meaning of existing
rights.

Landlock also supports observability: each domain has an identifier that is
unique and not reused for the system's lifetime and increases by a random step,
which hides the exact next value but not the underlying monotonic progression,
so the identifier is an observability aid, not a confidentiality boundary.
Denied actions can be logged through audit, but this per-domain configuration is
not a security boundary.  Landlock access records describe denials, never
allowed accesses, and a nested layer's denial is attributed to that layer, not
bypassing an outer one (see :ref:`admin-guide/LSM/landlock:Audit`).

Composition and trust boundaries
--------------------------------

Building a ruleset combines rights as a *union*: its rules come from a single
trusted author, so each rule grants the accesses it describes.

Enforcing a ruleset on a thread (a domain transition) combines constraints as an
*intersection*: the stacked layers do not trust each other, so a thread's own
transition can only ever remove access.  A thread cannot un-sandbox itself or
regain an access a previous layer (or an inherited parent sandbox) denied. A
thread may enforce a ruleset on all threads of its process at once, replacing
the siblings' Landlock configuration.  This is not a relaxation across a
security boundary, since threads share an address space and are not a security
boundary (see `What is not a Landlock security bug`_).  Landlock's restrictions
therefore stay attached to the thread for its lifetime, aside from such
whole-process synchronization.

Each access right has a precise and fixed semantic
--------------------------------------------------

An access right or scope controls exactly the operations its semantic defines,
no more and no less.  A semantic is defined by an operation's effect, not by the
syscall or code path used to reach it, including an indirect, deferred, or
kernel-mediated effect the subject arranges.  Every path that produces a covered
effect is in scope, so missing one is under-enforcement.  For example, the TCP
rights control TCP ``bind`` and ``connect`` only, on any path (including an
implicit connect performed while sending data), but they do not apply to MPTCP
or SCTP, even when those use TCP internally.  A scope likewise covers the
cross-domain interaction however it is produced: ``LANDLOCK_SCOPE_SIGNAL``
covers a signal the subject arranges the kernel to deliver (e.g. ``SIGIO`` via
:manpage:`fcntl(2)` ``F_SETOWN``), not only a direct :manpage:`kill(2)`.

Each right is checked at its own enforcement point against the current domain,
and Landlock does not retroactively revoke access already tied to an explicitly
referenced kernel object (e.g. a file descriptor or io_uring instance).  A file
descriptor obtained before enforcement is thus not covered afterward.  This is
not under-enforcement but the intended capability-model behavior: a program can
open its dependencies before restricting itself, or use only what a broker
passes it while being unable to obtain those resources directly.

This semantic is a permanent interface contract: it does not change, and the set
of rights a policy targets does not grow on its own (new rights are opt-in; see
:ref:`userspace-api/landlock:Compatibility`).  A kernel update only makes
*enforcement* converge toward the semantic, in either direction:

* Enforcing *less* than the semantic is under-enforcement: a security bug. The
  fix may make a deployed policy stricter, but only by finally enforcing what it
  already requested.
* Enforcing *more* than the semantic is over-enforcement: a compatibility bug,
  not a security bug.  The fix relaxes the restriction; because that is visible
  to user space, it is advertised through the errata mechanism (see
  :ref:`userspace-api/landlock:Compatibility`).  Most programs need not check
  errata; when they do, an erratum should only gate enabling a restriction,
  never dropping one.
* An operation whose effect lies outside every right's semantic is simply not
  covered; controlling it requires a new access right, not redefining an
  existing one.

What is a Landlock security bug
-------------------------------

A Landlock security bug is a deviation from this contract.  A sandboxed subject
performs an action that its own active policy should deny (under-enforcement),
including regaining an access across a domain transition. To classify a
behavior, check in order:

#. Does the domain *handle* the relevant access right or scope, or does an
   implicit restriction above apply, and does that semantic cover the operation?
   If not, the action is not restricted, possibly a limitation described below,
   not a bug.
#. Could the subject perform an action that a missing allow-list entry, a scope,
   or an implicit restriction should deny?  That is a security bug.
#. Did an access become allowed for the transitioning thread only after a
   further domain transition, which must only remove access?  That is a security
   bug.

Conversely, Landlock denying a *legitimate* action is not under-enforcement. An
intentional implicit restriction is expected, such as the ptrace hierarchy, or a
filesystem-topology or reparenting restriction while a filesystem right is
handled; these are non-selective limitations, described below.  Denying more
than a handled right's semantic requires is over-enforcement, a compatibility
bug, not a security bug.

This concerns Landlock's access-control guarantees.  A vulnerability in
Landlock's own implementation that an unprivileged process could exploit to
escalate privileges or compromise the kernel, separate from this access-control
classification, is a security bug under the general kernel threat model, like
any other kernel code.

Impact and blast radius
-----------------------

Security bugs (under-enforcement) vary in impact along two dimensions:

* *Blast radius within a sandbox*: a flaw in a domain-wide mechanism such as
  credential handling can drop all of a domain's restrictions (broad), while a
  missing check for one access right only affects sandboxes using that right and
  leaves their other restrictions intact (narrow).
* *Affected processes*: a Landlock policy is self-imposed per domain, not
  system-wide, so a bug weakens only the sandboxes that requested the affected
  restriction; other sandboxes and unsandboxed processes are unaffected.

Because Landlock only adds restrictions on top of existing access controls, an
*enforcement* bug can at most undo Landlock's own restrictions on the affected
sandbox: the leaked access is still subject to standard DAC and the other LSMs.

What is not a Landlock security bug
-----------------------------------

The following are outside Landlock's threat model and are handled as ordinary
issues, not security bugs (see also Documentation/process/threat-model.rst):

* **Actions relying on privileges Landlock does not control**: a sandboxed
  process stays bound by its domain whatever capabilities it holds, so bypassing
  a handled right is a bug even for a privileged process.  Out of scope is what
  a retained privilege (e.g. ``CAP_SYS_ADMIN``) enables outside Landlock's
  coverage, including undermining the sandbox's own construction; dropping such
  privileges is the sandbox's job (see `Sandboxing is layered`_).

* **Interactions within the same domain**: the cross-domain restrictions
  (:manpage:`ptrace(2)` and the ``LANDLOCK_SCOPE_*`` scopes) apply only when
  leaving the domain hierarchy, not among tasks of the same domain or a domain
  nested under it; a domain's filesystem and network access rights still apply
  to all of its tasks.

* **Direct interactions between threads of the same process**: Linux manages
  credentials, and therefore the Landlock domain, per thread, so threads of one
  process may even be in different domains.  Sharing an address space, they are
  not a security boundary, and for practical reasons some restrictions are not
  enforced between them: notably ``LANDLOCK_SCOPE_SIGNAL`` always allows signals
  between threads of the same process, even in different domains (like the
  :manpage:`ptrace(2)` same-process exception), because user space synchronizes
  per-thread credentials by signaling within the process, and some runtimes do
  not expose thread control.  This does not extend to objects tied to a
  creator's domain, such as abstract UNIX sockets.  Consequently a per-thread
  domain does not protect the process; a whole-process guarantee requires
  confining all its threads with the same domain (see `Sandboxing is layered`_).

* **Resources obtained from outside the sandbox**: receiving or inheriting a
  file descriptor (or similar) is governed by the capability model, not
  Landlock.  If an unsandboxed process willingly passes a sensitive resource,
  that is a security-architecture issue (a possible confused deputy), not a
  bypass; the resource carries the access rights set when it was created (e.g.
  an FD's read/write mode), which may exceed the receiver's policy.  By
  contrast, a resource the sandboxed process obtains itself stays bound by its
  own domain: a subsystem it sets up, such as io_uring, captures the subject's
  credentials, and with them the domain, so the work it later performs stays
  restricted.

* **Actions a user space service performs on the caller's behalf**: Landlock
  mediates the kernel-level access to a service, such as reaching its socket,
  not the service's own authorization of the requests it receives.  Exposing a
  service to a sandbox is a policy choice; a more-privileged service that acts
  on a sandboxed client's request without checking what that client should be
  allowed is a confused deputy, an issue in the service, not a Landlock bypass.

* **Denial of service by an already-privileged user**: such a user can exhaust
  resources regardless of Landlock.  Landlock must, however, not give an
  *unprivileged* process a new way to do so: its long-lived allocations are
  accounted to the requesting task's memory cgroup (and thus limitable) and its
  computation impacts only the processes requesting it (see
  :ref:`userspace-api/landlock:Current limitations` and `Guiding principles for
  safe access controls`_).

* **Information disclosure about the policy or filesystem layout**: a denial
  error code (e.g. ``EACCES`` versus ``ENOENT`` or ``EXDEV``) or timing can
  reveal whether a path exists or what a policy allows.  Consistent with the
  general kernel threat model, such probing side channels are not Landlock
  security bugs; Landlock only minimizes avoidable ones (e.g. the random-step
  domain identifiers above).

* **Syscall-argument filtering**: that is seccomp-bpf's role, not Landlock's
  (see `Guiding principles for safe access controls`_).

Limitations are not security bugs
---------------------------------

Landlock is best-effort: it enforces what the running kernel and the program's
configuration allow rather than refusing to sandbox at all.  An operation that
cannot be restricted is a limitation, not a bug, in three cases:

#. **Not requested**: the program did not handle the relevant right (a policy
   choice).
#. **Not supported by the running kernel**: the right belongs to a newer ABI
   than the running kernel; programs query the ABI version and enable the
   largest available subset.
#. **Not supported by any kernel yet**: no access right covers the operation.
   For example, Landlock restricts access to a file's data but not yet changes
   to its metadata (chmod, chown, utime, setxattr).

Likewise, some objects cannot be tied to rules and are not explicitly
restrictable, such as pipes or sockets reachable only through
``/proc/<pid>/fd/*``.  See :ref:`userspace-api/landlock:Current limitations`.

Some restrictions are also non-selective rather than absent: a domain handling
any filesystem right denies filesystem-topology changes (:manpage:`mount(2)`,
:manpage:`pivot_root(2)` and the like), since Landlock cannot yet scope them to
particular mounts, and denies reparenting a file to a different directory by
default.  The ``LANDLOCK_ACCESS_FS_REFER`` right is denied even when not
handled, and allowing a reparenting is subject to further constraints (see
:ref:`userspace-api/landlock:Kernel interface`).  A policy cannot opt out of
these while keeping its filesystem rights.  :manpage:`chroot(2)` is not denied:
it only changes the calling process's root directory without relocating any
inode, so the inode-based rules still apply (it can even narrow the visible
tree).

Best-effort matters because a program and its kernel are built and released
independently, often by different parties, so the running kernel is often
unknown at build time.  A program tested against the newest features it targets
should still protect users as much as possible on an older kernel. This is safe
because a right's semantic is identical across kernels (above), so a policy
degrades gracefully.

Landlock started with a limited set of access rights and gains more over time.
Each new right is designed, tested, and documented, and once released its
meaning becomes a permanent interface (above) that can never change. Classifying
an operation as a limitation does not discourage lifting it: extending
Landlock's coverage is welcome, and ongoing or planned work is listed in the
`Landlock issue tracker <https://github.com/landlock-lsm/linux/issues>`_.

Sandboxing is layered
---------------------

Landlock is the access-control layer of a sandbox, not the whole sandbox.  A
robust sandbox also needs steps that are the program's responsibility: switching
to an unprivileged user, dropping capabilities, setting ``PR_SET_NO_NEW_PRIVS``,
and confining all threads of the process with the same domain.  A
single-threaded process gets the latter for free; a multithreaded one can
enforce a ruleset atomically on all its threads, or must otherwise synchronize
them before any untrusted work.  Landlock is typically applied last, to tighten
access and make the domain identifiable and auditable.

Stronger isolation can come from combining Landlock with other mechanisms in a
defense-in-depth approach, notably seccomp-bpf (see
Documentation/userspace-api/seccomp_filter.rst) for what Landlock does not yet
cover.  A long-term goal of Landlock is to control access to any kind of kernel
resource in a way suited to sandboxing.

Guiding principles for safe access controls
===========================================

* A Landlock rule shall be focused on access control on kernel objects instead
  of syscall filtering (i.e. syscall arguments), which is the purpose of
  seccomp-bpf.
* To avoid multiple kinds of side-channel attacks (e.g. leak of security
  policies, CPU-based attacks), Landlock rules shall not be able to
  programmatically communicate with user space.
* Kernel access check shall not slow down access request from unsandboxed
  processes.
* Computation related to Landlock operations (e.g. enforcing a ruleset) shall
  only impact the processes requesting them.
* Resources (e.g. file descriptors) directly obtained from the kernel by a
  sandboxed process shall retain their scoped accesses (at the time of resource
  acquisition) whatever process uses them.
  Cf. `File descriptor access rights`_.
* Access denials shall be logged according to system and Landlock domain
  configurations.  Log entries must contain information about the cause of the
  denial and the owner of the related security policy.  Such log generation
  should have a negligible performance and memory impact on allowed requests.

Design choices
==============

Inode access rights
-------------------

All access rights are tied to an inode and what can be accessed through it.
Reading the content of a directory does not imply to be allowed to read the
content of a listed inode.  Indeed, a file name is local to its parent
directory, and an inode can be referenced by multiple file names thanks to
(hard) links.  Being able to unlink a file only has a direct impact on the
directory, not the unlinked inode.  This is the reason why
``LANDLOCK_ACCESS_FS_REMOVE_FILE`` or ``LANDLOCK_ACCESS_FS_REFER`` are not
allowed to be tied to files but only to directories.

File descriptor access rights
-----------------------------

Access rights are checked and tied to file descriptors at open time.  The
underlying principle is that equivalent sequences of operations should lead to
the same results, when they are executed under the same Landlock domain.

Taking the ``LANDLOCK_ACCESS_FS_TRUNCATE`` right as an example, it may be
allowed to open a file for writing without being allowed to
:manpage:`ftruncate` the resulting file descriptor if the related file
hierarchy doesn't grant that access right.  The following sequences of
operations have the same semantic and should then have the same result:

* ``truncate(path);``
* ``int fd = open(path, O_WRONLY); ftruncate(fd); close(fd);``

Similarly to file access modes (e.g. ``O_RDWR``), Landlock access rights
attached to file descriptors are retained even if they are passed between
processes (e.g. through a Unix domain socket).  Such access rights will then be
enforced even if the receiving process is not sandboxed by Landlock.  Indeed,
this is required to keep access controls consistent over the whole system, and
this avoids unattended bypasses through file descriptor passing (i.e. confused
deputy attack).

.. _scoped-flags-interaction:

Interaction between scoped flags and other access rights
--------------------------------------------------------

The ``scoped`` flags in &struct landlock_ruleset_attr restrict the
use of *outgoing* IPC from the created Landlock domain, while they
permit reaching out to IPC endpoints *within* the created Landlock
domain.

In the future, scoped flags *may* interact with other access rights,
e.g. so that abstract UNIX sockets can be allow-listed by name, or so
that signals can be allow-listed by signal number or target process.

When introducing ``LANDLOCK_ACCESS_FS_RESOLVE_UNIX``, we defined it to
implicitly have the same scoping semantics as a
``LANDLOCK_SCOPE_PATHNAME_UNIX_SOCKET`` flag would have: connecting to
UNIX sockets within the same domain (where
``LANDLOCK_ACCESS_FS_RESOLVE_UNIX`` is used) is unconditionally
allowed.

The reasoning is:

* Like other IPC mechanisms, connecting to named UNIX sockets in the
  same domain should be expected and harmless.  (If needed, users can
  further refine their Landlock policies with nested domains or by
  restricting ``LANDLOCK_ACCESS_FS_MAKE_SOCK``.)
* We reserve the option to still introduce
  ``LANDLOCK_SCOPE_PATHNAME_UNIX_SOCKET`` in the future.  (This would
  be useful if we wanted to have a Landlock rule to permit IPC access
  to other Landlock domains.)
* But we can postpone the point in time when users have to deal with
  two interacting flags visible in the userspace API.  (In particular,
  it is possible that it won't be needed in practice, in which case we
  can avoid the second flag altogether.)
* If we *do* introduce ``LANDLOCK_SCOPE_PATHNAME_UNIX_SOCKET`` in the
  future, setting this scoped flag in a ruleset does *not reduce* the
  restrictions, because access within the same scope is already
  allowed based on ``LANDLOCK_ACCESS_FS_RESOLVE_UNIX``.

Tests
=====

Userspace tests for backward compatibility, ptrace restrictions and filesystem
support can be found here: `tools/testing/selftests/landlock/`_.

Kernel structures
=================

Object
------

.. kernel-doc:: security/landlock/object.h
    :identifiers:

Filesystem
----------

.. kernel-doc:: security/landlock/fs.h
    :identifiers:

Process credential
------------------

.. kernel-doc:: security/landlock/cred.h
    :identifiers:

Ruleset and domain
------------------

A domain is a read-only ruleset tied to a set of subjects (i.e. tasks'
credentials).  Each time a ruleset is enforced on a task, the current domain is
duplicated and the ruleset is imported as a new layer of rules in the new
domain.  Indeed, once in a domain, each rule is tied to a layer level.  To
grant access to an object, at least one rule of each layer must allow the
requested action on the object.  A task can then only transit to a new domain
that is the intersection of the constraints from the current domain and those
of a ruleset provided by the task.

The definition of a subject is implicit for a task sandboxing itself, which
makes the reasoning much easier and helps avoid pitfalls.

.. kernel-doc:: security/landlock/ruleset.h
    :identifiers:

.. kernel-doc:: security/landlock/domain.h
    :identifiers:

Additional documentation
========================

* Documentation/userspace-api/landlock.rst
* Documentation/admin-guide/LSM/landlock.rst
* https://landlock.io

.. Links
.. _tools/testing/selftests/landlock/:
   https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/tools/testing/selftests/landlock/
