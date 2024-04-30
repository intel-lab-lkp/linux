.. SPDX-License-Identifier: GPL-2.0+

=================================================================
Linux Base Driver for the Intel(R) Volume Management Device (VMD)
=================================================================

Intel vmd Linux driver.

Contents
========

The Intel Volume Management Device (VMD) is a Root Complex Integrated
Endpoint that acts as a host bridge to a secondary PCIe domain. The BIOS
can enable or disable VMD. Additioanlly the BIOS can reassign one or more
Root Ports to appear within a VMD domain instead of the primary domain.
The immediate benefit is that additional PCIe domains allow more than 256
buses in a system by letting bus numbers be reused across different domains.

VMD domains do not define ACPI _SEG, so to avoid domain clashing with host
bridges defining this segment, VMD domains start at 0x10000, which is
greater than the highest possible 16-bit ACPI defined _SEG.

This driver enumerates and enables the domain using the root bus
configuration interface provided by the PCI subsystem. The driver provides
configuration space accessor functions (pci_ops), bus and memory resources,
an MSI IRQ domain with irq_chip implementation, and DMA operations
necessary to use devices through the VMD endpoint's interface.

VMD routes I/O as follows:

   1) Configuration Space: BAR 0 ("CFGBAR") of VMD provides the base
   address and size for configuration space register access to VMD-owned
   root ports. It works similarly to MMCONFIG for extended configuration
   space.  Bus numbering is independent and does not conflict with the
   primary domain.

   2) MMIO Space: BARs 2 and 4 ("MEMBAR1" and "MEMBAR2") of VMD provide the
   base address, size, and type for MMIO register access. These addresses
   are not translated by VMD hardware; they are simply reservations to be
   distributed to root ports' memory base/limit registers and subdivided
   among devices downstream.

   3) DMA: To interact appropriately with an IOMMU, the source ID DMA read
   and write requests are translated to the bus-device-function of the VMD
   endpoint. Otherwise, DMA operates normally without VMD-specific address
   translation.

   4) Interrupts: Part of VMD's BAR 4 is reserved for VMD's MSI-X Table and
   PBA. MSIs from VMD domain devices and ports are remapped to appear as
   if they were issued using one of VMD's MSI-X table entries. Each MSI
   and MSI-X address of VMD-owned devices and ports has a special format
   where the address refers to specific entries in the VMD's MSI-X table.
   As with DMA, the interrupt source ID is translated to VMD's
   bus-device-function.

   The driver provides its own MSI and MSI-X configuration functions
   specific to how MSI messages are used within the VMD domain, and
   provides an irq_chip for independent IRQ allocation to relay interrupts
   from VMD's interrupt handler to the appropriate device driver's handler.

   5) Errors: PCIe error message are intercepted by the root ports normally
   (e.g., AER), except with VMD, system errors (i.e. firmware first) are
   disabled by default. AER and hotplug interrupts are translated in the
   same way as endpoint interrupts.

   6) VMD does not support INTx interrupts or IO ports. Devices or drivers
   requiring these features should either not be placed below VMD-owned
   root ports, or VMD should be disabled by BIOS for such endpoints.

The driver can run on both a host and a guest. The BIOS provides a mechanism
to specify which VMD devices should be passed through to a guest and the
hypervisor can select that device by specifying the bus-device-function. The
device that is passed through to a guest is not seen by the driver on the
host.