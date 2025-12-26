.. SPDX-License-Identifier: GPL-2.0

Pointer Formatting in Rust
===========================

This document describes how to format kernel pointers safely in Rust code,
corresponding to the C kernel's printk format specifiers ``%p``, ``%pK``, and ``%px``.

For general information about pointer formatting in the kernel, please refer to
:doc:`../core-api/printk-formats`.

Overview
--------

The Rust kernel provides three wrapper types for formatting kernel pointers:

- **``HashedPtr``** → ``%p`` (hashed, default)
- **``RestrictedPtr``** → ``%pK`` (restricted, respects ``kptr_restrict``)
- **``RawPtr``** → ``%px`` (raw address, debug only)

When formatting raw pointers (``*const T`` or ``*mut T``) with ``{:p}``,
they are automatically wrapped with ``HashedPtr``, providing safe default behavior.

HashedPtr (%p)
--------------

Use ``HashedPtr`` for general kernel logging. Pointers are hashed before printing
to prevent leaking information about the kernel memory layout.

**Example**::

    use kernel::ptr::HashedPtr;

    pr_info!("Device pointer: {:p}\n", HashedPtr::from(ptr));

RestrictedPtr (%pK)
-------------------

Use ``RestrictedPtr`` when producing content of a file read by userspace from
e.g. procfs or sysfs (using e.g. ``seq_print!()``, not ``pr_info!()``). The behavior
depends on the ``kptr_restrict`` sysctl setting.

**Example**::

    use kernel::ptr::RestrictedPtr;

    seq_print!(seq_file, "Pointer: {:p}\n", RestrictedPtr::from(ptr));

For more details about ``kptr_restrict``, see :doc:`../admin-guide/sysctl/kernel`.

RawPtr (%px)
------------

**Warning**: This exposes the real kernel address and should **only** be used
for debugging purposes.

**Example**::

    use kernel::ptr::RawPtr;

    pr_info!("Debug pointer: {:p}\n", RawPtr::from(ptr));

Formatting Options
------------------

All pointer wrapper types support standard Rust formatting options including
width, alignment, and padding characters. These options are preserved through
the formatting system.

The following examples demonstrate formatting options using ``RawPtr`` for
predictable output. The same options work with ``HashedPtr`` and
``RestrictedPtr``, though the exact output may vary due to pointer hashing::

    use kernel::ptr::RawPtr;

    // Basic formatting
    pr_info!("Pointer: {:p}\n", RawPtr::from(ptr));

    // Minimum width
    pr_info!("Pointer: {:30p}\n", RawPtr::from(ptr));

    // Right align with zero padding
    pr_info!("Pointer: {:0>30p}\n", RawPtr::from(ptr));

    // Left align with zero padding
    pr_info!("Pointer: {:0<30p}\n", RawPtr::from(ptr));

    // Center align with zero padding
    pr_info!("Pointer: {:0^30p}\n", RawPtr::from(ptr));

    // Right align with space padding (default)
    pr_info!("Pointer: {:>30p}\n", RawPtr::from(ptr));

    // Center align with custom padding character
    pr_info!("Pointer: {:*^30p}\n", RawPtr::from(ptr));

See Also
--------

- :doc:`../core-api/printk-formats` - General pointer formatting documentation
- `rust/kernel/ptr.rs <srctree/rust/kernel/ptr.rs>`_ - Implementation
