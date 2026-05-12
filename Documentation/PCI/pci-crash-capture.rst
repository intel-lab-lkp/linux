.. SPDX-License-Identifier: GPL-2.0

========================
PCI Crash Capture Buffer
========================

Overview
========

The PCI crash capture module (``CONFIG_PCI_CRASH``) saves PCI configuration
space for all (or selected) devices at panic time.  The data is written into
a pre-allocated buffer whose physical pages are exported via VMCOREINFO,
allowing crash analysis tools to extract device state from the vmcore.

This is useful because AER (Advanced Error Reporting) registers are volatile
and cleared by device reset during kexec into the crash kernel.  Capturing
them before kexec preserves the error state that caused or contributed to the
crash.

Boot Parameters
===============

``pci_crash.capture=`` (default: ``aer``)
  When to capture PCI config space.  Comma-separated tokens:

  ``aer``
    Capture only if a root port reports uncorrectable errors in its
    AER ROOT_STATUS register.  Non-PCI panics skip capture entirely
    (a handful of MMIO reads to root ports, sub-microsecond).

  ``always``
    Capture on every panic regardless of AER state.  Useful for
    cascading failures where a PCI link-down causes an MCE or NMI
    watchdog timeout before DPC/AER fires.

``pci_crash.devices=`` (default: ``all``)
  Which devices to include in the capture buffer.  Comma-separated tokens:

  ``all``
    Every PCI device in the system.

  ``bridges``
    PCI-to-PCI bridges (class 0604) and CardBus bridges (class 0607).

  ``root_ports``
    PCIe root ports only.

  ``XXYY``
    Hex PCI class code (class byte XX, subclass byte YY).
    Up to 8 class codes may be specified.

  Bridges are always implicitly included regardless of the filter value
  because they hold AER registers needed for root cause analysis.  The
  filter is applied at device enumeration and hotplug rebuild time, not at
  crash time (zero overhead on the panic path).

Both parameters are writable at runtime via sysfs
(``/sys/module/pci_crash/parameters/``).

Architecture
============

::

  late_initcall
      │
      ├── enumerate PCI devices (filtered by devices= param)
      ├── allocate buffer via kvmalloc (may be vmalloc for >4 MiB)
      ├── build pagemap: kmalloc'd array of per-page physical addresses
      └── register bus notifier for hotplug

  hotplug (BUS_NOTIFY_ADD_DEVICE / BUS_NOTIFY_DEL_DEVICE)
      │
      └── schedule delayed rebuild (200 ms debounce)
              └── re-enumerate, re-allocate buffer + pagemap

  panic (__crash_kexec → crash_save_vmcoreinfo → pci_crash_save)
      │
      ├── quick-scan root port AER ROOT_STATUS (capture=aer)
      │     └── bail if no uncorrectable errors
      ├── read config space for each device (MMIO, no locks)
      ├── flush dcache (buffer + pagemap) to RAM
      └── VMCOREINFO exports: PCI_CRASH_PAGEMAP, PCI_CRASH_BUF_SZ

Buffer Format
=============

The buffer consists of a 32-byte header followed by variable-length
device records:

.. code-block:: c

    struct pci_crash_buffer_header {   /* 32 bytes */
        __le32 magic;           /* 0x50434943 "PCIC" */
        __le32 version;         /* 1 */
        __le32 device_count;
        __le32 config_size;     /* 0 = variable-length records */
        __le64 timestamp;       /* ktime_get_real_fast_ns() */
        __le32 flags;           /* reserved */
        __le32 reserved;
    };

    struct pci_crash_device_record {   /* 8 + cfg_size bytes */
        __le16 domain;
        __u8   bus;
        __u8   devfn;
        __le32 config_size;     /* 256 or 4096 */
        __u8   config_data[];
    };

The pagemap (exported via ``PCI_CRASH_PAGEMAP``) allows the parser to
locate buffer pages without walking page tables:

.. code-block:: c

    struct pci_crash_pagemap {
        __le32 magic;           /* 0x5043504d "PCPM" */
        __le32 num_pages;
        __le64 buf_size;
        __le64 addrs[];         /* physical address per page */
    };

Safety Considerations
=====================

- ``pci_read_config_dword()`` is direct ECAM MMIO at crash time (no locks).
- ``ktime_get_real_fast_ns()`` is NMI-safe (lockless timekeeper snapshot).
- ``WRITE_ONCE``/``READ_ONCE`` + memory barriers between rebuild (process
  context) and ``pci_crash_save()`` (crash context, single CPU, interrupts
  disabled).
- Buffer capped at 24 MiB to prevent excessive allocation on systems with
  thousands of VFs.
- ``slab_is_available()`` guard in param setters prevents use-before-init
  when parameters are set via kernel command line.
