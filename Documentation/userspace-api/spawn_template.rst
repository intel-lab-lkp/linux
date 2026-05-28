.. SPDX-License-Identifier: GPL-2.0

===============
Spawn templates
===============

``spawn_template`` is a userspace-controlled interface for workloads that
repeatedly start the same executable with different arguments, environment, and
file-descriptor setup.

Userspace creates a template fd for an executable with
``spawn_template_create()``.  Later calls to ``spawn_template_spawn()`` create a
new child from that template and return both a pid and a pidfd.  The child still
executes through the normal ``execve`` path.  The template only lets the kernel
reuse metadata that is safe to reuse after revalidation.

This is intended for launchers, shells, and agent runtimes that already know
which tools are hot.  The kernel does not decide policy for names such as
``rg``, ``git``, or ``sed``.  Userspace should keep its existing spawn path as a
fallback for unsupported files, invalidated templates, and policy decisions.

This RFC version supports ELF executable templates only.  Scripts, binfmt_misc
targets, and other non-ELF formats are expected to use the fallback path.

Template lifetime
=================

``spawn_template_create()`` takes ``struct spawn_template_create_args`` and
returns a template fd.  The fd is an ordinary file descriptor backed by an
anonymous inode.  Closing the fd releases the template.

Userspace can identify the executable either by an existing executable fd or by
an absolute path.  Exactly one of ``execfd`` and ``filename`` must be supplied.
Passing ``SPAWN_TEMPLATE_CREATE_CLOEXEC`` sets ``O_CLOEXEC`` on the returned
template fd.

Relative paths are rejected for path-created templates.  The kernel stores the
filename and re-opens it at spawn time to check that the path still names the
same executable.  A relative filename would be resolved against the caller's
current working directory at spawn time, not the directory that was current
when the template was created.  For example, a template created for ``bin/tool``
while the caller is in ``/repo-a`` could later be spawned after the caller has
changed to ``/repo-b``.  Revalidating ``bin/tool`` would then look under
``/repo-b`` and give different semantics from the executable that was
originally templated.  Userspace that wants directory-relative lookup should
open the executable itself and create the template from ``execfd``.

Creating a template for an unsupported executable format fails.  For this RFC
that means non-ELF executables fail template creation rather than becoming a
partially cached template.

Create-time fd actions are not supported.  ``actions`` and ``actions_len`` in
``struct spawn_template_create_args`` are reserved and must be zero.  File
descriptor numbers are per-process state, so reusable fd actions would be
ambiguous once the creating process changes its fd table.

Spawning
========

``spawn_template_spawn()`` takes a template fd and
``struct spawn_template_spawn_args``.  ``argv`` and ``envp`` point to the normal
userspace argument and environment vectors for the new image.  ``pidfd`` points
to an ``int`` in userspace where the kernel stores the new pidfd.  The syscall
return value is the new pid on success.

A successful ``spawn_template_spawn()`` return means the child has been created
and the pidfd has been installed.  After that point, per-spawn action failures
or exec failures are reported by the child exit status, not by changing the
syscall return value.  The syscall itself returns a negative errno only for
errors detected before child creation, such as bad arguments, a bad template
fd, stale executable identity, or clone failure.

Per-spawn actions run in the child before exec.  They are intended for the same
kind of setup that ``posix_spawn_file_actions_t`` commonly performs:

``SPAWN_TEMPLATE_ACTION_CLOSE``
  Close one fd.

``SPAWN_TEMPLATE_ACTION_DUP2``
  Duplicate one fd to another fd, optionally with ``O_CLOEXEC``.

``SPAWN_TEMPLATE_ACTION_FCHDIR``
  Change the child's current working directory to an open directory fd.

``SPAWN_TEMPLATE_ACTION_OPEN``
  Open a path using ``struct open_how`` and install it at ``newfd``.

``SPAWN_TEMPLATE_ACTION_CLOSE_RANGE``
  Apply ``close_range()`` to a child fd range.

``SPAWN_TEMPLATE_ACTION_SIGMASK``
  Set the child signal mask.

``SPAWN_TEMPLATE_ACTION_SIGDEFAULT``
  Reset selected signal dispositions to ``SIG_DFL``.

By default, the child closes all inherited file descriptors above standard
error after the requested actions have run.  Passing
``SPAWN_TEMPLATE_SPAWN_INHERIT_FDS`` keeps the traditional inheritance model.
Launchers for untrusted or secret-bearing workloads should prefer the default.

Security model
==============

``spawn_template_spawn()`` is not a shortcut around ``execve`` security.  Each
spawn still reaches the normal binary handler and credential commit path, so
permission checks, LSM hooks, secure-exec handling, and ``no_new_privs`` remain
part of execution.

The template fd does not grant ambient authority to unrelated tasks.  The
current implementation requires the caller to have the same credential object
that created the template.  Passing the fd with ``SCM_RIGHTS`` is therefore not
enough to delegate spawn authority after credentials have changed.

The kernel pins the executable inode against writes while the template exists.
An in-place writer therefore fails while a template fd is alive.  A package
manager can still replace a tool with a rename; a path-created template then
sees that the absolute path resolves to a different executable and spawn fails
before creating a child.  Userspace can close the old template fd and create a
new one after such an update.

Each spawn revalidates cached identity metadata before using template metadata.
The key includes device, inode, size, mode, owner, ctime, and mtime.
Path-created templates re-open the path before child creation and reject reuse
if the path now names a different executable.

Cached metadata
===============

For ELF executables, the template caches only the main executable ELF header,
program headers, and executable identity key.  The cached program headers are
used to avoid repeated metadata reads for hot executables after the executable
identity has been revalidated.

The cache does not include the shared-library dependency graph.  Shared
libraries are found by the userspace dynamic linker after exec and depend on
userspace policy such as ``LD_LIBRARY_PATH``, ``RPATH``, ``RUNPATH``,
``/etc/ld.so.cache``, mount namespaces, and secure-exec state.  The kernel
therefore does not try to duplicate dynamic-linker policy in a spawn template.

Errors and fallback
===================

If template creation reports an unsupported format, or if spawn reports a stale
template before child creation, the caller should use its existing spawn
implementation.  A launcher may also drop the template fd and create a new
template after a failure.  Once spawn has returned a pid, the caller should
observe child success or failure by waiting on the pid or pidfd.

The interface is designed so ordinary tools do not need to be modified.
Runtimes that already centralize process launch can opt in one executable at a
time and preserve their existing fallback behavior.
