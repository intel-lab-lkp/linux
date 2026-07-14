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

``pci_crash.capture=`` (default: ``always``)
  When to capture PCI config space.  Comma-separated tokens:

  ``aer``
    Capture only if a root port reports an uncorrectable error in its
    AER ROOT_STATUS register.  Non-PCI panics skip capture entirely
    (a handful of MMIO reads to root ports, sub-microsecond).

  ``always``
    Capture on every panic regardless of AER state.  Useful for
    cascading failures where a PCI link-down causes an MCE or NMI
    watchdog timeout before DPC/AER fires, so the crash reason is
    unrelated but the AER registers still hold the originating error.

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
      ├── register PCI bus notifier (before the first rebuild)
      ├── enumerate PCI devices (filtered by devices= param)
      ├── allocate buffer via kvmalloc (may be vmalloc for >4 MiB)
      ├── build pagemap: kmalloc'd array of per-page physical addresses
      └── publish snapshot via rcu_assign_pointer()

  hotplug (BUS_NOTIFY_ADD_DEVICE / BUS_NOTIFY_DEL_DEVICE)
      │
      └── schedule delayed rebuild (200 ms debounce)
              └── re-enumerate, re-allocate buffer + pagemap,
                  publish new snapshot, retire old via call_rcu()

  panic (__crash_kexec → crash_save_vmcoreinfo → pci_crash_save)
      │
      ├── rcu_read_lock(); sample the published snapshot
      ├── quick-scan root port AER ROOT_STATUS (capture=aer)
      │     └── bail if no uncorrectable errors
      ├── for each device: skip if unreachable, else read config space
      │     via pci_bus_read_config_dword_trylock()
      ├── flush dcache (buffer + pagemap) to RAM
      └── VMCOREINFO exports: PCI_CRASH_PAGEMAP, PCI_CRASH_BUF_SZ,
          PCI_CRASH_VERSION

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
        __u8   config_data[];   /* 0xffffffff for unreachable dwords */
    };

The pagemap (exported via ``PCI_CRASH_PAGEMAP``) allows the parser to
locate buffer pages without walking page tables:

.. code-block:: c

    struct pci_crash_pagemap {
        __le32 magic;           /* 0x5043504d "PCPM" */
        __le32 num_pages;
        __le64 buf_size;
        __le32 buf_offset;      /* offset of buffer start within first page */
        __le64 addrs[];         /* physical address per page */
    };

All multi-byte fields are little-endian.  The struct sizes and the
``addrs[]`` offset are asserted with ``BUILD_BUG_ON()`` so the on-wire
layout cannot drift away from the userspace parser silently.

VMCOREINFO keys
===============

``pci_crash_save()`` exports the following keys into VMCOREINFO (consumed by
makedumpfile / the crash-utility and any bespoke vmcore parser).  They are
emitted whenever a valid snapshot exists at panic time; the buffer may be
unfilled when the AER quick-scan found no errors and skipped the capture
(``capture=aer``).  Parsers must check the buffer header magic (``PCIC``)
to confirm config space was actually captured:

``PCI_CRASH_PAGEMAP=<hex>``
  Physical address of the ``struct pci_crash_pagemap``.  The pagemap is
  always kmalloc'd (direct-mapped), so this physical address is stable and
  the parser can read it directly from the vmcore.  From the pagemap the
  parser reconstructs the (possibly vmalloc'd, physically discontiguous)
  buffer page by page.

``PCI_CRASH_VERSION=<dec>``
  On-wire format version (``PCI_CRASH_VERSION``).  Parsers must reject a
  version they do not understand rather than misinterpret the layout.

``PCI_CRASH_BUF_SZ=<dec>``
  Total buffer size in bytes, matching ``pci_crash_pagemap::buf_size``.

Safety Considerations
=====================

``pci_crash_save()`` runs from ``crash_save_vmcoreinfo()`` inside
``__crash_kexec()``, before ``machine_kexec()``.  It executes in crash
context, so every access on that path is constrained accordingly:

- **Config reads use** ``pci_bus_read_config_dword_trylock()``, which takes
  ``pci_lock`` with a *trylock* and skips the device on contention.  Depending
  on ``crash_kexec_post_notifiers`` the other CPUs may still be running or may
  already be halted (possibly while holding ``pci_lock``), and the panicking
  CPU may itself have been interrupted mid config access while holding it.
  ``pci_lock`` is a raw, non-reentrant spinlock, so a blocking acquire could
  deadlock the dump in either case; the trylock skips the device instead.  This
  avoids the lock deadlock only; it does not make the read itself fault-safe,
  which is why unreachable devices are skipped first (below).

- **Unreachable devices are skipped before any access to them.**  A config
  read to a device whose PCIe link is physically down can, on some
  architectures (notably arm64), raise a synchronous external abort.  Before
  reading an endpoint, the module establishes reachability *without touching
  the endpoint*:

  - software state -- ``pci_dev_is_disconnected()``, ``pci_channel_offline()``
    and ``PCI_D3cold`` are flag reads (no MMIO); they catch devices a
    subsystem has already marked gone or powered off; and

  - the immediate upstream PCIe port's Link Status (Data Link Layer Link
    Active).  The upstream port is on-die and always responds, so reading its
    Link Status cannot fault on the endpoint's dead link; if the link is down,
    the endpoint is skipped and its record is filled with ``0xffffffff``.

- ``ktime_get_real_fast_ns()`` is NMI-safe (lockless timekeeper snapshot).

- **Live capture state is a single RCU-published snapshot.**  The rebuild
  worker (process context) swaps it via ``rcu_assign_pointer()`` and frees the
  old snapshot via ``call_rcu()``; ``pci_crash_save()`` reads it under
  ``rcu_read_lock()``.  RCU keeps the snapshot alive for the *fill*, but the
  exported buffer/pagemap addresses are consumed after ``pci_crash_save()``
  returns (the VMCOREINFO export, and ``machine_kexec()`` snapshotting RAM),
  i.e. after ``rcu_read_unlock()`` -- and on the default panic path peer CPUs
  are still live and may retire snapshots.  ``pci_crash_save()`` therefore
  *pins* the snapshot it captured (``pci_crash_captured_snap``); the RCU free
  callback leaks a pinned snapshot instead of freeing it, which is harmless
  because the system is rebooting into the crash kernel.

- Buffer capped at 24 MiB to bound allocation on systems with thousands of
  VFs; per-device reads are clamped to 4096 bytes and the fill loop
  bounds-checks every record against the buffer end.

- ``pci_crash_ready`` defers param parsing and rebuild until ``late_initcall``
  completes; kernel command-line values are stored and take effect once the
  PCI subsystem is up.

Architecture support and residual risk
---------------------------------------

The upstream-port Link-Status pre-check eliminates the common and
deterministic hang: an endpoint whose link is already down at panic is never
read.  Two narrow residual cases remain on architectures where a config read
to a dead device raises a fatal abort (e.g. arm64 ``do_sea()``, which has no
kernel-mode recovery for an external abort):

- a link that drops in the small window *between* the upstream-port check and
  the endpoint read (a true hardware race); and

- a multi-level fabric collapse in which an upstream port is itself behind a
  dead link (only the immediate parent is checked).

On x86 a read to an absent device returns all-ones harmlessly, so these cases
are arm64-specific.  Capturing such a device may therefore, in those narrow
races, abort the dump on arm64.  Making the read itself recoverable would
require new architecture support in the abort handler and is intentionally not
part of this module; it can be added later as a separate, properly-typed
arch facility without changing the on-wire format.
