.. SPDX-License-Identifier: GPL-2.0

==============================
CXL Protocol Error Handling
==============================

CXL devices report protocol-layer failures (CXL.cachemem RAS) as PCIe
AER Internal Errors: PCI_ERR_COR_INTERNAL for correctable events and
PCI_ERR_UNC_INTN for uncorrectable events.  The actual fault
information lives in CXL RAS capability registers, not in the PCIe AER
status registers.

The kernel routes every CXL Internal Error through a producer/consumer
pipeline shared by all CXL device types: Root Ports, Upstream/Downstream
Switch Ports, Endpoints, and Restricted CXL Devices (RCDs).


Architecture
============

Two error planes run side by side:

* The **PCIe AER plane** handles native PCIe errors (receiver
  overflows, malformed TLPs, completion timeouts, etc.).
* The **CXL protocol error plane** handles CXL Internal Errors.
  The AER core forwards them to cxl_core via a dedicated kfifo;
  cxl_core reads the CXL RAS registers, emits trace events, and
  applies recovery/panic policy.

The boundary between the two planes is enforced by is_cxl_error() in
aer_cxl_vh.c.  It checks info->is_cxl, the PCIe device type
(Endpoint, Root Port, Upstream, or Downstream), and whether the AER
status word indicates an internal error.  RC_END devices are excluded
from is_cxl_error() because they reach the kfifo via the separate
cxl_rch_handle_error() path instead.

The pipeline:

1. **Producer** (aer_cxl_vh.c, aer_cxl_rch.c) - AER threaded
   handler context.  Classifies and enqueues a
   struct cxl_proto_err_work_data into the kfifo.
2. **Queue** - the AER-CXL kfifo plus a backing work_struct.
3. **Consumer** (cxl_core/ras.c) - workqueue context.  Resolves
   the CXL port topology and dispatches to CE/UE handlers.


Topologies
==========

Virtual Hierarchy (VH)
----------------------

Standard PCIe topology: Root Port, optional switch (Upstream Port with
one or more Downstream Ports), and Endpoints.  Each component raises
Internal Errors directly via the Root Port's AER interrupt.

Producer: cxl_forward_error() in aer_cxl_vh.c.

Restricted CXL Host (RCH)
--------------------------

A Root Complex Event Collector (RCEC) aggregates errors from RCDs
attached as Root Complex Integrated Endpoints.  The AER driver
iterates RCDs beneath the RCEC via pcie_walk_rcec() and forwards
each qualifying device through cxl_forward_error() into the same
kfifo.

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
   |  aer_irq() -> aer_isr()             |
   |  -> find_source_device()             |
   |  -> handle_error_source(dev, info)   |
   +--------------------------------------+
                   |
                   v
   +--------------------------------------+
   | handle_error_source() dispatch       |
   |                                      |
   |  1. cxl_rch_handle_error()           |
   |     [always; filters internally]     |
   |                                      |
   |  2. if is_cxl_error():              |
   |       cxl_forward_error()            |
   |       [enqueue to kfifo]             |
   |                                      |
   |  3. if cxl_pending && non-CE:        |
   |       cxl_proto_err_flush()          |
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
   |  port = find_cxl_port_by_dev(        |
   |           &pdev->dev, NULL)           |
   |  dport = cxl_find_dport_by_dev(      |
   |           port, &pdev->dev)           |
   |  [dport NULL for EP/USP; set RP/DSP] |
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

cxl_do_recovery() reads the CXL RAS uncorrectable status register.
If UE bits are set, it emits the trace event and panics.  If no bits
are set (e.g. RAS mapped but error already cleared), it logs a
diagnostic and defers to AER recovery.


Severity policy
===============

**CE** - cxl_handle_cor_ras() reads the CXL RAS correctable status
register, clears set bits, and emits a cxl_aer_correctable_error
trace event.  No recovery action.

**UCE (non-fatal, and fatal on Root Port/Downstream Port)** - cxl_do_recovery() reads the CXL RAS
uncorrectable status register.  If UE bits are set, the kernel panics.
CXL.cachemem traffic cannot be safely recovered once an uncorrectable
error is signaled; continuing risks silent data corruption across
interleaved HDM regions.  This panic policy applies to the native AER
path.  On firmware-first (CPER/GHES) platforms the CPER handler emits
trace events only and does not call cxl_do_recovery().

**Fatal UCE on EP/USP** - The AER core driver does not read AER status
registers for Endpoint and Upstream Ports with fatal events because the
link is down.  Without AER status, is_cxl_error() cannot classify
the event as a CXL protocol error and it falls through to standard
AER recovery.

RCH special case
================

When the consumer sees is_cxl_restricted(pdev), it calls
cxl_handle_rdport_errors() first to process the RCH Downstream
Port's RAS registers (accessed via RCRB, not standard config space).
It then continues to process the RCD Endpoint's own RAS registers
via the common path.  Both register blocks are checked because
errors can appear in either independently.

cxl_handle_rdport_errors() acquires the port lock internally.
Callers must not hold it.


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

CXL Internal Error bits (PCI_ERR_UNC_INTN and PCI_ERR_COR_INTERNAL)
are unmasked in the AER capability only after the CXL RAS register
block is successfully mapped.  A devm teardown action restores the
mask when the port or dport is removed, ensuring clean state after
driver removal.


Source files
============

.. list-table::
   :header-rows: 1

   * - File
     - Role
   * - drivers/pci/pcie/aer.c
     - AER core; IRQ, dispatch
   * - drivers/pci/pcie/aer_cxl_vh.c
     - VH producer; kfifo
   * - drivers/pci/pcie/aer_cxl_rch.c
     - RCH dispatch; RCEC walk
   * - drivers/cxl/core/ras.c
     - Consumer; CE/UE handlers; CPER
   * - drivers/cxl/core/ras_rch.c
     - RCH dport RAS handling
   * - drivers/acpi/apei/ghes.c
     - CPER/GHES kfifo producer
