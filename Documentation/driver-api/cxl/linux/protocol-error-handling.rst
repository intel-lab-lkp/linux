.. SPDX-License-Identifier: GPL-2.0

==============================
CXL Protocol Error Handling
==============================

CXL devices report protocol-layer failures (CXL.cachemem RAS) as PCIe AER
Internal Errors: PCI_ERR_COR_INTERNAL for correctable events and
PCI_ERR_UNC_INTN for uncorrectable events. The actual fault information
lives in CXL RAS capability registers, not in the PCIe AER status registers.

The kernel routes every CXL Internal Error through a producer/consumer
pipeline shared by all CXL device types: Root Ports, Upstream/Downstream
Switch Ports, Endpoints, and Restricted CXL Devices (RCDs).

Errors are delivered by one of two mechanisms. On native-AER platforms the
kernel takes the AER interrupt and reads the CXL RAS registers itself. On
firmware-first (CPER/GHES) platforms, platform firmware handles the error
and hands the kernel a CPER record; that path is trace-only. Both converge
on the same cxl_core RAS handlers.


Architecture
============

Two error planes run side by side:

* The **PCIe AER plane** handles native PCIe errors (receiver overflows,
  malformed TLPs, completion timeouts, etc.). This includes CXL.io, which
  is functionally PCIe and reports through native AER status registers.
* The **CXL protocol error plane** handles CXL.cachemem (CXL.cache and
  CXL.mem) protocol errors. These have no native AER status; they are
  signaled as AER Internal Errors, with the fault detail held in the CXL
  RAS capability registers. The AER core forwards them to cxl_core via a
  dedicated kfifo; cxl_core reads the CXL RAS registers, emits trace
  events, and applies recovery/panic policy.

The boundary between the two planes is enforced by is_cxl_error() in
aer_cxl_vh.c. It checks info->is_cxl, the PCIe device type (Endpoint,
Root Port, Upstream, or Downstream), and whether the AER status word
indicates an internal error. RC_END devices are excluded from
is_cxl_error() because they reach the kfifo via the separate
cxl_rch_handle_error() path instead.

The pipeline:

1. **Producer** (aer_cxl_vh.c, aer_cxl_rch.c) - AER threaded handler
   context. Classifies and enqueues a struct cxl_proto_err_work_data
   into the kfifo.
2. **Queue** - the AER-CXL kfifo plus a backing work_struct.
3. **Consumer** (cxl_core/ras.c) - workqueue context. Resolves the CXL
   port topology and dispatches to CE/UE handlers.


AER handlers vs RAS handlers
============================

Two distinct handler layers cooperate; keeping them separate is central to
the design:

* **AER handlers** run in PCIe AER context (aer.c, aer_cxl_vh.c,
  aer_cxl_rch.c). They own the PCIe side: they observe the Internal Error,
  classify it with is_cxl_error(), and act only as the *producer* - they
  enqueue a work item into the AER-CXL kfifo. AER handlers never touch the
  CXL RAS capability registers and never make a recovery/panic decision.

* **RAS handlers** run in cxl_core (cxl_core/ras.c, cxl_core/ras_rch.c).
  They are the *consumers*: they read the CXL RAS capability registers,
  emit the CXL trace events, and apply the CE/UCE severity policy (clear
  correctable status, or panic on an uncorrectable error). A RAS handler
  is where the actual CXL fault information is decoded, because that
  information lives in the RAS registers, not in the PCIe AER status word.

The AER handler and the RAS handler are decoupled by the kfifo: the AER
handler cannot block on RAS register access (which may sleep) and the RAS
handler runs in workqueue context where it can safely take port locks. The
same RAS handlers are reached through three different entry paths, and only
the entry path differs - the RAS decode/policy is identical:

* the AER-CXL kfifo consumer (native AER, VH and RCH),
* the pci_error_handlers .error_detected callback (fatal EP/RCD UCE, where
  no AER status is available), and
* the CPER-CXL kfifo consumer (firmware-first CPER/GHES, trace-only).


Topologies
==========

Virtual Hierarchy (VH)
----------------------

Standard PCIe topology: Root Port, optional switch (Upstream Port with one
or more Downstream Ports), and Endpoints. Each component raises Internal
Errors directly via the Root Port's AER interrupt.

Producer: cxl_forward_error() in aer_cxl_vh.c.

Restricted CXL Host (RCH)
--------------------------

A Root Complex Event Collector (RCEC) aggregates errors from RCDs attached
as Root Complex Integrated Endpoints. The AER driver iterates RCDs beneath
the RCEC via pcie_walk_rcec() and forwards each qualifying device through
cxl_forward_error() into the same kfifo.

Producer: cxl_forward_error() in aer_cxl_vh.c, called from
cxl_rch_handle_error_iter() via pcie_walk_rcec().


Error flow
==========

.. code-block:: text

   CXL device raises AER Internal Error
   (PCI_ERR_COR_INTERNAL or PCI_ERR_UNC_INTN)
                   |
                   v
   +--------------------------------------+
   | AER core (aer.c)                     |
   |  aer_irq() -> aer_isr()              |
   |  -> find_source_device()             |
   |  -> handle_error_source(dev, info)   |
   +--------------------------------------+
                   |
                   v
   +--------------------------------------+
   | handle_error_source() dispatch       |
   |                                      |
   |  1. cxl_rch_handle_error()           |
   |     [always; filters internally.     |
   |      RC_END enters the kfifo here    |
   |      via pcie_walk_rcec(), NOT via   |
   |      is_cxl_error() below]           |
   |                                      |
   |  2. if is_cxl_error():               |
   |       cxl_forward_error()            |
   |       [enqueue to kfifo; EP/RP/USP/  |
   |        DSP only, RC_END excluded]    |
   |                                      |
   |  3. if cxl_pending && non-CE:        |
   |       cxl_proto_err_wait_for_empty() |
   |       [sync drain before recovery]   |
   |                                      |
   |  4. pci_aer_handle_error() [always]  |
   +--------------------------------------+
                   |
          (kfifo -> workqueue)
                   |
                   v
   +--------------------------------------+
   | __cxl_proto_err_work_fn() consumer   |
   |                                      |
   |  if is_cxl_restricted(pdev):         |
   |    cxl_handle_rdport_errors()        |
   |    [RCH dport RAS first]             |
   |                                      |
   |  cxl_handle_proto_error()            |
   +--------------------------------------+
            |                |
            v                v
   +-----------------+  +--------------------+
   | CE              |  | UCE                |
   | cxl_handle_     |  | cxl_do_recovery()  |
   |   cor_ras()     |  |  read RAS status   |
   | trace + clear   |  |  trace + panic     |
   +-----------------+  +--------------------+

cxl_do_recovery() first checks whether the CXL RAS register block is
mapped. If it is not (to_ras_base() returns NULL), the kernel panics
immediately without reading any register or emitting a trace event,
because a signaled UCE cannot be confirmed or cleared. Otherwise it
reads the CXL RAS uncorrectable status register. If UE bits are set, it
emits the trace event and panics. If no bits are set (e.g. RAS mapped but
error already cleared), it logs a debug diagnostic and defers to AER
recovery.


Fatal UCE flow for Endpoints and RCDs
=====================================

For a fatal (AER_FATAL) uncorrectable error, aer_get_device_error_info()
reads the AER uncorrectable status register only for Root Ports, RC Event
Collectors, and Downstream Ports; it skips the read for Endpoints and
Upstream Ports because their link is presumed down. With info->status left
zero, is_cxl_error() cannot classify the event as a CXL protocol error, so
it never enters the AER-CXL kfifo. This is a severity/device-type property,
not an RCH-specific one: it affects every Endpoint (VH Endpoint and RCD
alike) and every Upstream Port.

Endpoints instead reach the RAS handler through the pci_error_handlers
.error_detected callback (cxl_pci_error_detected()), which is registered by
the CXL memdev driver and fires for both VH Endpoints and RCDs. The only
RCD-specific step is the leading cxl_handle_rdport_errors() call, which
processes the RCH Downstream Port's RAS registers first; the Endpoint RAS
read and panic policy that follow are identical for VH and RCH:

.. code-block:: text

   Fatal UCE on Endpoint (VH Endpoint or RCD; link down, no AER status)
                   |
                   v
   +--------------------------------------+
   | PCIe core error recovery             |
   |  pcie_do_recovery()                  |
   |  -> report_error_detected()          |
   |  -> cxl_pci_error_detected()         |
   |     [pci_error_handlers callback in  |
   |      cxl_core/ras.c; the RAS handler,|
   |      NOT the AER kfifo path]         |
   +--------------------------------------+
                   |
                   v
   +--------------------------------------+
   | cxl_pci_error_detected()             |
   |                                      |
   |  if is_cxl_restricted(pdev):         |
   |    cxl_handle_rdport_errors()        |
   |    [RCD-only: RCH Dport RAS first]   |
   |                                      |
   |  cxl_handle_ras(port, NULL,          |
   |                 to_ras_base(...))    |
   |    [unconditional EP RAS read;       |
   |     dead link readl()==0xFFFFFFFF    |
   |     sets all UE bits -> panic]       |
   |                                      |
   |  if ue: panic("CXL cachemem error")  |
   |                                      |
   |  else switch (channel state):        |
   |    io_normal      -> CAN_RECOVER     |
   |    io_frozen      -> release driver, |
   |                      NEED_RESET      |
   |    perm_failure   -> DISCONNECT      |
   +--------------------------------------+

This path handles both severities: a non-fatal EP UCE arrives as
pci_channel_io_normal and a fatal EP UCE as pci_channel_io_frozen. Either
way the CXL RAS read runs first, so a real CXL.mem UCE always panics; only
when no CXL UE bit is set (or RAS is unmapped) does the channel state drive
ordinary AER recovery. Endpoint unbind therefore does not depend on the
AER-status-to-RAS coupling that the kfifo path relies on.

Upstream Ports bound to portdrv have no such .error_detected callback and
fall back to standard AER recovery - this is a known limitation.


CPER / firmware-first flow
==========================

On firmware-first platforms, CXL protocol errors are delivered by platform
firmware as an ACPI CPER record (CPER_SEC_CXL_PROT_ERR) instead of a native
AER interrupt. These records already contain a snapshot of the CXL RAS
capability registers, so the RAS handler does not read hardware; it only
emits trace events. Firmware-first is therefore trace-only and never
panics or drives recovery - the platform owns the recovery decision.

CPER protocol-error records are delivered by the GHES/APEI firmware-first
path:

* **GHES/APEI** (ghes.c) - the common firmware-first path. Its producer,
  cxl_cper_post_prot_err(), enqueues a struct cxl_cper_prot_err_work_data
  into a dedicated CPER-CXL kfifo (cxl_cper_prot_err_fifo, depth 8) and
  schedules the cxl_core consumer work item.

.. code-block:: text

   Platform firmware CPER record (CPER_SEC_CXL_PROT_ERR)
            |
            v
   +----------------------+
   | GHES/APEI (ghes.c)   |
   |  ghes_do_proc()      |
   |  cxl_cper_post_      |
   |    prot_err()        |
   |  kfifo_put(CPER-CXL) |
   |  schedule_work()     |
   +----------------------+
            |
            v
   +----------------------+
   | CPER-CXL kfifo       |
   | + work_struct        |
   +----------------------+
            |
            v
   +----------------------+
   | cxl_cper_prot_err_   |
   |   work_fn() consumer |
   | (cxl_core/ras.c)     |
   |  drain kfifo ->      |
   +----------------------+
            |
            v
   +--------------------------------+
   | cxl_cper_handle_prot_err()     |
   |  pci_get_domain_bus_and_slot() |
   |  find_cxl_port_by_dev()        |
   |  cxl_find_dport_by_dev()       |
   |                                |
   |  if CE: trace correctable      |
   |  else:  trace uncorrectable    |
   |  [trace-only; no panic,        |
   |   no cxl_do_recovery()]        |
   +--------------------------------+

The consumer work item is registered with GHES via
cxl_cper_register_prot_err_work() when cxl_core loads and torn down with
cxl_cper_unregister_prot_err_work(), which cancels any pending work and
resets the kfifo so stale records are not replayed on the next module load.


Severity policy
===============

**CE** - cxl_handle_cor_ras() reads the CXL RAS correctable status register,
clears set bits, and emits a cxl_aer_correctable_error trace event. No
recovery action.

**UCE (non-fatal, and fatal on Root Port/Downstream Port)** -
cxl_do_recovery() reads the CXL RAS uncorrectable status register. If UE
bits are set, the kernel panics. If the CXL RAS register block is not
mapped (to_ras_base() returns NULL), cxl_do_recovery() panics before any
register read and emits no trace event, since the UCE cannot be confirmed.
CXL.cachemem traffic cannot be safely recovered once an uncorrectable error
is signaled; continuing risks silent data corruption. This panic policy
applies to the native AER path. On firmware-first (CPER/GHES) platforms the
CPER handler emits trace events only and does not call cxl_do_recovery().

**Fatal UCE on EP/USP** - A fatal event brings the link down, so the AER
core reads no AER status and is_cxl_error() cannot enqueue the event to the
kfifo. Endpoints and RCDs are instead handled through the
pci_error_handlers .error_detected callback (cxl_pci_error_detected()),
which reads the CXL RAS registers unconditionally and panics on any UE bit.
Upstream Ports bound to portdrv fall back to standard AER recovery - a known
limitation. See "Fatal UCE flow for Endpoints and RCDs" above for the full
path and channel-state handling.


RCH special case
================

When the consumer sees is_cxl_restricted(pdev), it calls
cxl_handle_rdport_errors() first to process the RCH Downstream Port's RAS
registers (accessed via RCRB, not standard config space). It then
continues to process the RCD Endpoint's own RAS registers via the common
path. Both register blocks are checked because errors can appear in either
independently.

cxl_handle_rdport_errors() acquires the port lock internally. Callers must
not hold it.


Trace events
============

Two trace events cover all device types and both the native AER and
CPER/GHES firmware-first paths:

* cxl_aer_correctable_error
* cxl_aer_uncorrectable_error

Fields:

* ``memdev`` - memdev name for Endpoints; empty for non-Endpoints.
* ``port`` - CXL port device name.
* ``dport`` - Downstream Port device name; empty when not applicable.
* ``host`` - parent host bridge or uport device name.
* ``serial`` - PCI Device Serial Number from pdev->dsn (cached at
  enumeration; no config-space read in the error path).


Interrupt masking
=================

CXL Internal Error bits (PCI_ERR_UNC_INTN and PCI_ERR_COR_INTERNAL) are
unmasked in the AER capability only after the CXL RAS register block is
successfully mapped. A devm teardown action restores the mask when the
port or dport is removed, ensuring clean state after driver removal.


Source files
============

.. list-table::
   :header-rows: 1

   * - File
     - Role
   * - drivers/pci/pcie/aer.c
     - AER core; IRQ, dispatch
   * - drivers/pci/pcie/aer_cxl_vh.c
     - VH AER producer; AER-CXL kfifo
   * - drivers/pci/pcie/aer_cxl_rch.c
     - RCH AER dispatch; RCEC walk
   * - drivers/cxl/core/ras.c
     - RAS handlers; AER-CXL and CPER-CXL kfifo consumers;
       .error_detected callback (cxl_pci_error_detected)
   * - drivers/cxl/core/ras_rch.c
     - RCH Downstream Port RAS handling
   * - drivers/acpi/apei/ghes.c
     - CPER/GHES producer; CPER-CXL kfifo
