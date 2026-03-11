.. SPDX-License-Identifier: GPL-2.0

Linux Security Modules in Rust
================================

This document describes how to write a Linux Security Module (LSM) using
the Rust abstractions provided in ``rust/kernel/lsm.rs``.


Overview
--------

The LSM framework allows security policies to be implemented as kernel
modules that register hooks into security-sensitive operations.  Traditionally
these are written in C; the Rust abstraction layer provides a safe,
trait-based interface that enforces correct hook registration at compile time.

A Rust LSM:

1. Implements the :ref:`Hooks trait <rust_lsm_hooks_trait>` for the subset
   of hooks it cares about.
2. Registers itself at boot time using the :ref:`define_lsm! macro
   <rust_lsm_define_macro>`.

No C code is required by the LSM author.  The abstraction layer generates
the necessary C-callable adapter functions and places the ``lsm_info``
descriptor in the ``.lsm_info.init`` ELF section, where
``security_init()`` discovers it during boot.


.. _rust_lsm_hooks_trait:

The ``Hooks`` trait
-------------------

``kernel::lsm::Hooks`` is the central trait.  Each method corresponds to one
LSM hook.  All methods have a default no-op implementation, so an implementor
only needs to override the hooks it cares about::

    pub trait Hooks: Sync + Send + 'static {
        fn file_open(_file: &File) -> Result { Ok(()) }
        fn task_alloc(_task: &Task, _clone_flags: u64) -> Result { Ok(()) }
        fn task_free(_task: &Task) {}
    }

The ``Sync + Send + 'static`` bounds are required because hook functions may
be called concurrently from any CPU.

Return values
~~~~~~~~~~~~~

Hooks that can deny an operation return ``kernel::error::Result``:

- ``Ok(())`` — allow the operation.
- ``Err(e)`` — deny the operation; ``e.to_errno()`` is returned to the caller.

Common denial error codes:

- ``EACCES`` — permission denied (policy decision).
- ``EPERM`` — operation not permitted (capability check).

The ``task_free`` hook cannot deny anything and has no return value.

Sleeping
~~~~~~~~

``task_free`` is called in a non-sleeping context.  Implementations **must
not** sleep or allocate memory with ``GFP_KERNEL`` inside ``task_free``.
All other hooks may sleep unless the call site is documented otherwise.


Available hooks (v1)
~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :widths: 25 75
   :header-rows: 1

   * - Hook
     - When called
   * - ``file_open``
     - A file is being opened.  Return ``Err`` to deny the open.
   * - ``task_alloc``
     - A new task (thread or process) is being created via ``clone(2)``
       or ``fork(2)``.  ``clone_flags`` contains the ``CLONE_*`` flags.
       Return ``Err`` to deny creation.
   * - ``task_free``
     - A task is being freed.  Must not sleep.  Use this to clean up any
       per-task state associated with the LSM.


.. _rust_lsm_define_macro:

The ``define_lsm!`` macro
--------------------------

``kernel::define_lsm!`` registers a type implementing ``Hooks`` with the
kernel LSM framework::

    kernel::define_lsm!(MyLsmType, "my_lsm\0", bindings::LSM_ID_UNDEF as u64);

Arguments:

.. list-table::
   :widths: 20 80
   :header-rows: 1

   * - Argument
     - Description
   * - ``$T``
     - The type implementing ``Hooks``.  Must be ``Sync + Send + 'static``.
   * - ``$name``
     - A ``&str`` literal with a NUL terminator (``\0``).  This name is used
       in ``/sys/kernel/security/lsm`` and in the ``lsm=`` kernel command-line
       parameter.
   * - ``$id``
     - The LSM identity constant from ``include/uapi/linux/lsm.h``.  Use
       ``bindings::LSM_ID_UNDEF as u64`` during development; request a
       permanent value as part of the upstream patch series.

The macro generates:

- A ``static __LSM_ID: LsmId`` holding the LSM identity.
- A ``static mut __LSM_HOOKS`` array of ``MaybeUninit<security_hook_list>``
  entries, one per registered hook.
- An ``unsafe extern "C" fn __lsm_init()`` that fills each hook slot using
  the C ``LSM_HOOK_INIT`` shim (necessary because ``security_hook_list`` has
  ``__randomize_layout``) and calls ``security_add_hooks()``.
- A ``static __LSM_INFO: LsmInfo`` in the ``.lsm_info.init`` ELF section,
  which ``security_init()`` iterates at boot to discover and call the init
  function.


Enabling in Kconfig
-------------------

To include a Rust LSM in the build, add a ``Kconfig`` entry that selects
``RUST`` and depends on ``SECURITY``::

    config SECURITY_MY_LSM
        bool "My Rust LSM"
        depends on SECURITY
        select RUST
        help
          A minimal Rust LSM that ...

Wire the object into the build in ``security/Makefile``::

    obj-$(CONFIG_SECURITY_MY_LSM) += my_lsm/

And in ``security/my_lsm/Makefile``::

    obj-$(CONFIG_SECURITY_MY_LSM) += my_lsm.o

The source file must **not** declare ``#![no_std]`` — the build system injects
it automatically for all kernel Rust objects.

Define ``__LOG_PREFIX`` at the top of the source file so that ``pr_info!`` and
friends work correctly::

    const __LOG_PREFIX: &[u8] = b"my_lsm\0";


Minimal example
---------------

The following is a complete, minimal Rust LSM that logs every file open and
allows all operations::

    // SPDX-License-Identifier: GPL-2.0

    //! Minimal Rust LSM example.

    const __LOG_PREFIX: &[u8] = b"my_lsm\0";

    use kernel::bindings;
    use kernel::lsm;
    use kernel::prelude::*;

    struct MyLsm;

    impl lsm::Hooks for MyLsm {
        fn file_open(file: &kernel::fs::File) -> Result {
            pr_info!("file_open: flags={:#x}\n", file.flags());
            Ok(())
        }
    }

    kernel::define_lsm!(MyLsm, "my_lsm\0", bindings::LSM_ID_UNDEF as u64);

The reference implementation is at ``security/rust_lsm/rust_lsm.rs``
(``CONFIG_SECURITY_RUST_LSM``).


Activating at boot
------------------

A Rust LSM compiled into the kernel is activated by listing its name in the
``lsm=`` kernel command-line parameter or by adding it to the ``CONFIG_LSM``
Kconfig string.

The LSM name must also be counted in ``include/linux/lsm_count.h`` so that
``MAX_LSM_COUNT`` is large enough to accommodate it alongside other compiled-in
LSMs.  Add a ``CONFIG_SECURITY_MY_LSM``-guarded macro alongside the existing
entries::

    #ifdef CONFIG_SECURITY_MY_LSM
    #define MY_LSM_ENABLED 1,
    #else
    #define MY_LSM_ENABLED
    #endif

    #define MAX_LSM_COUNT \
        COUNT_LSMS( \
            ...existing entries... \
            MY_LSM_ENABLED)

To verify the LSM is active after boot::

    cat /sys/kernel/security/lsm


Limitations (v1)
-----------------

- Only three hooks are available: ``file_open``, ``task_alloc``, and
  ``task_free``.  Additional hooks will be added in follow-up patches as safe
  Rust wrappers for their argument types are contributed upstream.

- At most one Rust LSM can be registered per kernel build with the current
  macro design.  Supporting multiple Rust LSMs requires unique static symbol
  generation, planned for v2 using a proc-macro.

- No security blob support.  Per-task or per-inode data associated with a
  Rust LSM requires the ``SecurityBlob<T>`` abstraction planned for v2.


C headers of interest
---------------------

- ``include/linux/lsm_hooks.h`` — ``lsm_info``, ``security_hook_list``,
  ``LSM_HOOK_INIT``, ``LSM_ORDER_*``.
- ``include/linux/lsm_hook_defs.h`` — canonical hook signatures.
- ``include/uapi/linux/lsm.h`` — ``LSM_ID_*`` constants.
- ``include/linux/lsm_count.h`` — ``MAX_LSM_COUNT`` computation.
- ``security/security.c`` — ``security_init()``, ``security_add_hooks()``.
