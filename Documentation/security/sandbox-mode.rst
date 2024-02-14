.. SPDX-License-Identifier: GPL-2.0

============
SandBox Mode
============

Introduction
============

The primary goal of SandBox Mode (SBM) is to reduce the impact of potential
memory safety bugs in kernel code by decomposing the kernel. The SBM API
allows to run each component inside an isolated execution environment. In
particular, memory areas used as input and/or output are isolated from the
rest of the kernel and surrounded by guard pages. Without arch hooks, this
common base provides *weak isolation*.

On architectures which implement the necessary arch hooks, SandBox Mode
leverages hardware paging facilities and CPU privilege levels to enforce the
use of only these predefined memory areas. With arch support, SBM can also
recover from protection violations. This means that SBM forcibly terminates
the sandbox and returns an error code (e.g. ``-EFAULT``) to the caller, so
execution can continue. Such implementation provides *strong isolation*.

A target function in a sandbox communicates with the rest of the kernel
through a caller-defined interface, comprising read-only buffers (input),
read-write buffers (output) and the return value. The caller can explicitly
share other data with the sandbox, but doing so may reduce isolation strength.

Protection of sensitive kernel data is currently out of scope. SandBox Mode is
meant to run kernel code which would otherwise have full access to all system
resources. SBM allows to impose a scoped access control policy on which
resources are available to the sandbox. That said, protection of sensitive
data is foreseen as a future goal, and that's why the API is designed to
control not only memory writes but also memory reads.

The expected use case for SandBox Mode is parsing data from untrusted sources,
especially if the parsing cannot be reasonably done by a user mode helper.
Keep in mind that a sandbox doesn't guarantee that the output data is correct.
The result may be corrupt (e.g. as a result of an exploited bug) and where
applicable, it should be sanitized before further use.

Using SandBox Mode
==================

SandBox Mode is an optional feature, enabled with ``CONFIG_SANDBOX_MODE``.
However, the SBM API is always defined regardless of the kernel configuration.
It will call a function with the best available isolation, which is:

* *strong isolation* if both ``CONFIG_SANDBOX_MODE`` and
  ``CONFIG_ARCH_HAVE_SBM`` are set,
* *weak isolation* if ``CONFIG_SANDBOX_MODE`` is set, but
  ``CONFIG_ARCH_HAVE_SBM`` is unset,
* *no isolation* if ``CONFIG_SANDBOX_MODE`` is unset.

Code which cannot safely run with no isolation should depend on the relevant
config option(s).

The API can be used like this:

.. code-block:: c

  #include <linux/sbm.h>

  /* Function to be executed in a sandbox. */
  static SBM_DEFINE_FUNC(my_func, const struct my_input *, in,
			 struct my_output *, out)
  {
	/* Read from in, write to out. */
	return 0;
  }

  int caller(...)
  {
	/* Declare a SBM instance. */
	struct sbm sbm;

	/* Initialize SBM instance. */
	sbm_init(&sbm);

	/* Execute my_func() using the SBM instance. */
	err = sbm_call(&sbm, my_func,
		       SBM_COPY_IN(&sbm, input, in_size),
		       SBM_COPY_OUT(&sbm, output, out_size));

	/* Clean up. */
	sbm_destroy(&sbm);

The return type of a sandbox mode function is always ``int``. The return value
is zero on success and negative on error. That's because the SBM helpers
return an error code (such as ``-ENOMEM``) if the call cannot be performed.

If sbm_call() returns an error, you can use sbm_error() to decide whether the
error was returned by the target function or because sandbox mode was aborted
(or failed to run entirely).

Public API
----------

.. kernel-doc:: include/linux/sbm.h
		:identifiers: sbm sbm_init sbm_destroy sbm_exec sbm_error
			      SBM_COPY_IN SBM_COPY_OUT SBM_COPY_INOUT
			      SBM_DEFINE_CALL SBM_DEFINE_THUNK SBM_DEFINE_FUNC
			      sbm_call

Arch Hooks
----------

These hooks must be implemented to select HAVE_ARCH_SBM.

.. kernel-doc:: include/linux/sbm.h
		:identifiers: arch_sbm_init arch_sbm_destroy arch_sbm_exec
			      arch_sbm_map_readonly arch_sbm_map_writable

Current Limitations
===================

This section lists know limitations of the current SBM implementation, which
are planned to be removed in the future.

Stack
-----

There is no generic kernel API to run a function on an alternate stack, so SBM
runs on the normal kernel stack by default. The kernel already offers
self-protection against stack overflows and underflows as well as against
overwriting on-stack data outside the current frame, but violations are
usually fatal.

This limitation can be solved for specific targets. Arch hooks can set up a
separate stack and recover from stack frame overruns.

Inherent Limitations
====================

This section lists limitations which are inherent to the concept.

Explicit Code
-------------

The main idea behind SandBox Mode is decomposition of one big program (the
Linux kernel) into multiple smaller programs that can be sandboxed. AFAIK
there is no way to automate this task for an existing code base in C.

Given the performance impact of running code in a sandbox, this limitation may
be perceived as a benefit. It is expected that sandbox mode is introduced only
knowingly and only where safety is more important than performance.

Complex Data
------------

Although data structures are not serialized and deserialized between kernel
mode and sandbox mode, all directly and indirectly referenced data structures
must be explicitly mapped into the sandbox, which requires some manual effort.

Copying of input/output buffers also incurs some runtime overhead. This
overhead can be reduced by sharing data directly with the sandbox, but the
resulting isolation is weaker, so it may or may not be acceptable, depending
on the overall safety requirements.

Page Granularity
----------------

Since paging is used to enforce memory safety, page size is the smallest unit.
Objects mapped into the sandbox must be aligned to a page boundary, and buffer
overflows may not be detected if they fit into the same page.

On the other hand, even though such writes are not detected, they do not
corrupt kernel data, because only the output buffer is copied back to kernel
mode, and the (corrupted) rest of the page is ignored.

Transitions
-----------

Transitions between kernel mode and sandbox mode are synchronous. That is,
whenever entering or leaving sandbox mode, the currently running CPU executes
the instructions necessary to save/restore its kernel-mode state. The API is
generic enough to allow asynchronous transitions, e.g. to pass data to another
CPU which is already running in sandbox mode. However, to see the benefits, a
hypothetical implementation would require far-reaching changes in the kernel
scheduler. This is (currently) out of scope.
