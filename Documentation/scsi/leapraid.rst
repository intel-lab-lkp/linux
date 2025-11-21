.. SPDX-License-Identifier: GPL-2.0

=========================
LeapRaid Driver for Linux
=========================

Introduction
============

LeapRaid is a storage RAID controller driver developed by LeapIO Tech. The
controller targets enterprise storage, cloud infrastructure, high performance
computing (HPC), and AI workloads.

It provides high-performance storage virtualization over PCI Express Gen4
and supports both SAS and SATA HDDs and SSDs. It offers both Host Bus Adapter
and RAID modes to meet diverse deployment requirements.

Supported devices
=================

- LeapHBA-8200A
- LeapHBA-8200C
- LeapRAID-8260

Features
========
- PCIe Gen4 x8 host interface
- Support for SAS and SATA devices
- RAID levels: 0, 1, 10, 5, 50, 6, 60
- Advanced error handling and end-to-end data integrity

LeapRaid specific host attributes
=================================

::

   /sys/class/scsi_host/host*/fw_queue_depth
   /sys/class/scsi_host/host*/host_sas_address

The host "fw_queue_depth" read-only attribute shows the firmware queue
depth of the host.

The host "host_sas_address" read-only attribute shows the SAS address
of the host.

LeapRaid specific disk attributes
=================================

::

   /sys/class/scsi_disk/host:bus:target:lun/device/sas_address
   /sys/class/scsi_disk/host:bus:target:lun/device/sas_device_handle
   /sys/class/scsi_disk/host:bus:target:lun/device/sas_ncq

The disk "sas_address" read-only attribute shows the SAS address of the
disk.

The read-only attribute "sas_device_handle" represents the disk's device
handle, which is a unique identifier maintained by the firmware.

This attribute "sas_ncq" controls the Native Command Queuing (NCQ) feature
for SATA devices. A value of 0 indicates that NCQ is currently disabled or
not supported. Writing 1 attempts to enable NCQ on the device. If the
operation succeeds, the value remains 1, indicating that NCQ has been
successfully enabled.

LeapRaid module parameters
==========================

The following module parameters can be configured at driver load time to
control driver behavior and tuning options.

1. open_pcie_trace
------------------

This parameter controls whether PCIe transaction tracing is enabled in the
driver. When set to 1, PCIe trace collection is enabled by default, allowing
detailed tracing of PCIe operations for debugging and performance analysis.
Setting it to 0 disables the trace functionality to reduce overhead in
production environments.

2. enable_mpio
--------------

This parameter enables or disables multipath support for target devices.
When set to 1, multipath functionality is enabled (default), allowing
multiple paths to be established. Setting it to 0 disables multipath
handling.

3. msix_disable
---------------

This parameter specifies whether MSI-X interrupts should be disabled. By
default, 0 keeps MSI-X enabled, providing high-performance interrupt
handling. Setting this parameter to 1 disables MSI-X, causing the driver
to fall back to Legacy INTx.

4. max_msix_vectors
-------------------

This parameter sets the upper limit on the number of MSI-X interrupt
vectors that the driver will request during initialization. The default
value of -1 allows the driver to use all available vectors as provided
by the device. Setting a positive integer restricts the number of vectors.

5. interrupt_mode
-----------------

This parameter defines which interrupt delivery mechanism is used by the
driver. A value of 0 selects MSI-X mode (default), providing the highest
performance and scalability. A value of 1 switches to MSI mode, while 2
forces the driver to use legacy INTx interrupts.

6. poll_queues
--------------

This parameter specifies the number of I/O queues to be used when operating
in io_uring poll mode. The default value is 0.

7. smart_poll
-------------

This parameter controls the SMART polling mechanism for SATA drives. When
enabled (1), the driver periodically checks the health of connected SATA
devices, allowing early detection of potential drive failures. The default
value 0 disables SMART polling

File Location
=============
The driver source is located at:

``drivers/scsi/leapraid/``

.. note::

   This document is intended for kernel developers and system
   integrators who need to build, test, and deploy the LeapRaid driver.
