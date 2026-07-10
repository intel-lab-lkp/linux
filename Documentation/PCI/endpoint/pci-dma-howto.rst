.. SPDX-License-Identifier: GPL-2.0

==========================================
PCI DMA Endpoint Function (EPF) User Guide
==========================================

:Author: Koichiro Den <den@valinux.co.jp>

This guide shows how to configure the ``pci-epf-dma`` endpoint function driver.
It uses ``dw-edma-pcie`` as the currently available host-side driver.  For the
hardware model and layout see Documentation/PCI/endpoint/pci-dma-function.rst.

Endpoint Device
===============

Endpoint Controller Devices
---------------------------

To find the list of endpoint controller devices in the system::

	# ls /sys/class/pci_epc/
	e65d0000.pcie-ep

If ``PCI_ENDPOINT_CONFIGFS`` is enabled::

	# ls /sys/kernel/config/pci_ep/controllers
	e65d0000.pcie-ep

Endpoint Function Drivers
-------------------------

To find the list of endpoint function drivers in the system::

	# ls /sys/bus/pci-epf/drivers
	pci_epf_dma  pci_epf_test

If ``PCI_ENDPOINT_CONFIGFS`` is enabled::

	# ls /sys/kernel/config/pci_ep/functions
	pci_epf_dma  pci_epf_test

Creating pci-epf-dma Device
---------------------------

Create a ``pci-epf-dma`` device with configfs::

	# mount -t configfs none /sys/kernel/config
	# cd /sys/kernel/config/pci_ep/
	# mkdir functions/pci_epf_dma/dma0

The "mkdir dma0" above creates the ``pci-epf-dma`` function device that will
be probed by the ``pci_epf_dma`` driver.

The PCI endpoint framework populates the directory with the common
configurable fields::

	# ls functions/pci_epf_dma/dma0
	baseclass_code   msi_interrupts   progif_code    subsys_id
	cache_line_size  msix_interrupts  revid          subsys_vendor_id
	deviceid         pci_epf_dma.0    secondary      vendorid
	interrupt_pin    primary          subclass_code

The PCI DMA function driver also creates a function-specific sub-directory.
The numeric suffix depends on the endpoint function instance number::

	# ls functions/pci_epf_dma/dma0/pci_epf_dma.0/
	dma_window_bar  metadata_bar  rd_chans  wr_chans

Configuring pci-epf-dma Device
------------------------------

The host-side ``dw-edma-pcie`` PCI ID table does not contain a generic
endpoint DMA PCI ID entry.  Choose a PCI vendor/device ID for the endpoint
device::

	# echo <vendor-id> > functions/pci_epf_dma/dma0/vendorid
	# echo <device-id> > functions/pci_epf_dma/dma0/deviceid
	# echo 1 > functions/pci_epf_dma/dma0/msi_interrupts

The PCI class defaults to ``PCI_BASE_CLASS_SYSTEM`` and
``PCI_CLASS_SYSTEM_DMA``.

The function-specific attributes are:

============== ============================================================
Attribute      Description
============== ============================================================
metadata_bar   BAR used to publish the endpoint DMA metadata and handshake
               bits.  It is kept as a stable BAR while the DMA windows are
               programmed.  If this is left unset, the first usable BAR that
               does not already contain a fixed DMA resource is used.
dma_window_bar BAR used for DMA resources that are not already host-visible,
               such as the DMA register window or descriptor windows.  This
               BAR may be switched to subrange mapping after the host driver
               has found the metadata.  If this is left unset and a DMA
               window is needed, the first usable BAR different from
               ``metadata_bar`` and not already occupied by a fixed DMA
               resource is used.
wr_chans       Number of endpoint-to-RC DMA write channels to expose.
rd_chans       Number of RC-to-endpoint DMA read channels to expose.
============== ============================================================

A sample configuration for a DesignWare eDMA/HDMA compatible controller with
two write channels and two read channels is given below::

	# echo 0 > functions/pci_epf_dma/dma0/pci_epf_dma.0/metadata_bar
	# echo 2 > functions/pci_epf_dma/dma0/pci_epf_dma.0/dma_window_bar
	# echo 2 > functions/pci_epf_dma/dma0/pci_epf_dma.0/wr_chans
	# echo 2 > functions/pci_epf_dma/dma0/pci_epf_dma.0/rd_chans

``wr_chans`` and ``rd_chans`` default to 0.  At least one channel direction
must be configured.  The selected channels are exposed in dense, zero-based
order; for example, ``wr_chans = 2`` exposes write channels 0 and 1.
DesignWare eDMA unroll and HDMA compatible layouts require each exposed
direction to be delegated as a whole, so set a direction to either 0 or the
number of hardware channels in that direction.  DesignWare HDMA native
linked-list mode allows a smaller dense prefix.  If ``dma_window_bar`` is
configured, it must be different from ``metadata_bar``.

The common ``msi_interrupts`` and ``msix_interrupts`` attributes select the
number of MSI and MSI-X vectors exposed to the host.  At least one MSI or
MSI-X vector must be configured.

The function-specific attributes can only be changed before the endpoint
function is bound to an endpoint controller.

Binding pci-epf-dma Device to EP Controller
-------------------------------------------

The DMA function device should be attached to a PCI endpoint controller
connected to the host::

	# ln -s controllers/e65d0000.pcie-ep \
		functions/pci_epf_dma/dma0/primary/

Once the above step is completed, the PCI endpoint controller is ready to
establish a link with the host.

Start the Link
--------------

Start the endpoint controller by writing 1 to ``start``::

	# echo 1 > controllers/e65d0000.pcie-ep/start

Root Complex Device
===================

lspci Output
------------

Note that the device listed here corresponds to the values populated in the
endpoint configuration above::

	# lspci -nk
	01:00.1 0801: <vendor-id>:<device-id>

If the host was already running while the endpoint function was configured,
rescan the PCI bus after the endpoint side has completed the configfs setup
and started the endpoint controller, if the platform supports it.

Bind the endpoint DMA function to ``dw-edma-pcie`` explicitly with
``driver_override``::

	# modprobe dw_edma_pcie
	# echo dw-edma-pcie > /sys/bus/pci/devices/0000:01:00.1/driver_override
	# echo 0000:01:00.1 > /sys/bus/pci/drivers_probe

The device should then be bound to ``dw-edma-pcie``::

	# lspci -nk -s 01:00.1
	01:00.1 0801: <vendor-id>:<device-id>
		Kernel driver in use: dw-edma-pcie

Using pci-epf-dma Device
------------------------

The host side software uses the standard Linux DMAengine API.  A DMAengine
client driver running on the host must request one of the channels provided by
``dw-edma-pcie`` and submit a transfer.

For an endpoint-to-RC write transfer, the DMAengine client uses a host DMA
buffer as the destination and an endpoint-side address as the slave source
address.  For an RC-to-endpoint read transfer, the DMAengine client uses a
host DMA buffer as the source and an endpoint-side address as the slave
destination address.

Troubleshooting
===============

``pci-epf-dma`` requires endpoint controller support for DMA auxiliary
resources and MSI or MSI-X.  If any DMA resource must be mapped dynamically,
the endpoint controller must also support BAR subrange mapping and dynamic
inbound mapping.  Binding the function to an endpoint controller fails if the
required capabilities are not available, or if both ``msi_interrupts`` and
``msix_interrupts`` are zero.

If ``dw-edma-pcie`` fails to probe on the host, check that the endpoint was
bound to the host driver, that the endpoint BARs were assigned by PCI
enumeration, and that the endpoint DMA metadata READY bit was set after any
DMA window BAR submaps were programmed.
