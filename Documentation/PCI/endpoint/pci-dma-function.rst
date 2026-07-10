.. SPDX-License-Identifier: GPL-2.0

================
PCI DMA Function
================

:Author: Koichiro Den <den@valinux.co.jp>

The PCI DMA endpoint function exposes an endpoint-integrated DMA controller
to the PCI host as a PCI DMA controller.  A matching host-side driver
discovers the endpoint DMA metadata and registers the delegated channels with
the Linux DMAengine framework, so host DMAengine clients can submit
transfers.

An endpoint Linux system can already use an endpoint-integrated DMA
controller locally through the normal DMAengine API, for example to transfer
data between endpoint memory and host addresses reachable over PCI.  The PCI
DMA function provides a different ownership model: it delegates selected
local DMA channels to the host, so a host DMAengine client can request and
program those endpoint-side channels through the host's DMAengine API.

To make that possible, the endpoint function publishes the DMA controller
register window and descriptor memory layout to the host, reserves the
selected local DMA channels on the endpoint side, and lets the host program
those channels directly.

Constructs Used for Implementing DMA
====================================

The PCI DMA function uses the following endpoint-side resources and
configuration:

	1) DMA controller register window
	2) DMA descriptor memory for endpoint-to-RC channels
	3) DMA descriptor memory for RC-to-endpoint channels
	4) MSI or MSI-X interrupt vectors selected through configfs
	5) One endpoint BAR used to publish metadata
	6) If needed, one endpoint BAR used for dynamically mapped DMA windows

The endpoint controller reports the DMA controller register and descriptor
resources through the endpoint auxiliary resource interface.  The PCI DMA
function uses those descriptions to build the host-visible metadata and to map
resources that are not already visible to the host.

DMA Controller Register Window
------------------------------

It contains the DMA controller registers programmed by the host-side driver
to submit transfers, control channels and handle DMA interrupts.

DMA Descriptor Memory
---------------------

It contains the descriptor memory used by the DMA controller.  The PCI DMA
function exposes descriptor memory for the delegated endpoint-to-RC and
RC-to-endpoint channels.

MSI/MSI-X Interrupt Vectors
---------------------------

They are used by the delegated DMA channels to signal completion and error
conditions to the host-side driver.

Metadata BAR
------------

It is the endpoint BAR used to publish the endpoint DMA metadata and handshake
bits.  The BAR remains stable while the endpoint function programs the DMA
windows.

DMA Window BAR
--------------

It is the endpoint BAR used for DMA resources that are not already visible
through a fixed BAR.  The endpoint function may switch this BAR to subrange
mapping after the host-side driver has found the metadata BAR.

BAR Metadata
============

The endpoint function places a small metadata block at the beginning of the
selected metadata BAR.  The format is defined in
``include/linux/pci-ep-dma.h``.

The host-side driver scans the function's assigned memory BARs, looks for the
endpoint DMA metadata magic, requests DMA window programming, waits for the
READY bit, and then parses the metadata to find the DMA register window and
descriptor windows.

::

	+----------------------+ metadata BAR offset 0
	| endpoint DMA metadata|
	+----------------------+
	| optional padding     |
	+----------------------+

	+----------------------+ DMA window BAR offset 0
	| mapped DMA resources |
	+----------------------+
	| optional padding     |
	+----------------------+

The metadata can also reference resources that are already host-visible
through fixed BARs.  For example, an endpoint controller may expose the DMA
controller register window at a fixed BAR offset while descriptor memories
are mapped into the DMA window BAR by the endpoint function.

The metadata is BAR-resident instead of a self-contained PCI Vendor-Specific
Extended Capability (VSEC).  Some endpoint controllers do not provide writable
configuration-space backing storage large enough for a new VSEC payload, while
they can map endpoint memory and controller resources into a BAR.

Channel Ownership
=================

The ``wr_chans`` attribute exposes endpoint-to-RC DMA write channels.  The
``rd_chans`` attribute exposes RC-to-endpoint DMA read channels.  The function
reserves the selected endpoint-side DMAengine channels so that endpoint-side
DMAengine clients cannot allocate and use the same hardware channels while
they are delegated to the host.

The current metadata revision describes channels in dense, zero-based order.
For example, ``wr_chans = 2`` exposes write channels 0 and 1.  Skipping a
hardware channel in the middle of the exposed range is not supported.

DesignWare eDMA unroll and HDMA compatible layouts require each exposed
direction to be delegated as a whole.  For example, on a controller with two
write channels, ``wr_chans`` must be either 0 or 2.  DesignWare HDMA native
linked-list mode uses per-channel registers, so a smaller dense prefix can be
delegated.

Interrupts
==========

The PCI DMA function exposes DMA interrupts through MSI or MSI-X.  The common
endpoint function ``msi_interrupts`` and ``msix_interrupts`` configfs attributes
select the interrupt vector counts programmed into endpoint config space.  At
least one MSI or MSI-X vector must be configured before the function is bound
to an endpoint controller.

Transfer Addressing
===================

The host-side DMAengine client supplies the endpoint memory address as the
DMA slave address.  For example, the ``dw-edma-pcie`` endpoint DMA metadata
parser passes that slave address to the DMA controller as a raw endpoint-side
address instead of translating it through a host PCI BAR resource.

The host memory buffer used as the other side of the transfer is still mapped
using the normal DMA mapping API on the host.

Endpoint Controller Requirements
================================

The endpoint controller driver must expose the DMA controller register
window and per-channel descriptor memories through the endpoint auxiliary
resource API.  Endpoint controllers with other DMA register layouts also need
matching metadata and host-side DMAengine driver support.

Current DesignWare endpoint DMA support exposes only channels with descriptor
memory; HDMA native non-linked-list mode is not supported yet.

If any DMA resource is not already host-visible through a fixed BAR, the
endpoint controller must also support BAR subrange mapping and dynamic inbound
mapping, because the DMA window BAR is assembled from those resources.

Current Support
===============

The current host-side support is implemented in ``dw-edma-pcie`` for
DesignWare eDMA unroll, HDMA compatible and HDMA native linked-list layouts.
Other PCIe controller DMA implementations need corresponding host-side
DMAengine driver support.

The ``dw-edma-pcie`` PCI ID table does not contain a generic endpoint DMA PCI
ID entry.  Users need to bind the host-side driver explicitly using
``driver_override``.

The current metadata revision requires the exposed channels to be a dense
prefix of the hardware channel numbers.

Security Model
==============

The interface is intended for trusted endpoint/host deployments.  A delegated
DMA channel can access endpoint memory addresses supplied by a host DMAengine
client.
