.. SPDX-License-Identifier: GPL-2.0-only

.. include:: <isonum.txt>

===============
 AMD Versal PCI
===============

:Copyright: |copy| 2026 Advanced Micro Devices, Inc.
:Authors: - Sonal Santan <sonal.santan@amd.com>
	  - Yidong (David) Zhang <yidong.zhang@amd.com>

Overview
========

The AMD Embedded+ platform integrates AMD Ryzen Embedded processors with
AMD Versal AI Edge adaptive System-on-Chip (SoC) on a single PCB. The AMD
Ryzen Embedded processor is connected to the Versal AI Edge adaptive SoC
via PCIe enabling a tightly coupled heterogeneous compute platform. AMD
Embedded+ platform is commonly used for sensor fusion, AI inferencing,
industrial networking, control, and visualization.

AMD Versal PCI driver, versal-pci, is a host-side PCIe driver for AMD
Embedded+ platform running on AMD Ryzen Embedded processor. The versal-pci
driver is responsible for the **management-plane** operations for the AMD
Versal AI Edge adaptive SoC, including:

* Loading accelerator firmware images
* Reset and recovery
* Health monitoring

Please note that the versal-pci driver does *not* participate in workload
execution.

Hardware Description
====================

AMD Versal AI SoCs boot from a dedicated flash device and presents two
PCIe physical functions to the AMD Ryzen Embedded processor which acts as
PCIe host.

* Physical function 0 (PF0) is used for the management-plane to interact
  with versal-pci driver.
* Physical function 1 (PF1) is used for the execution-plane. In some cases
  PF1 can be attached to a VM.

Versal Management Runtime (VMR) Firmware
----------------------------------------

AMD Versal AI SoC runs Versal Management Runtime (VMR) firmware on a
microcontroller. VMR is responsible for:

* Low-level management of the AMD Versal AI SoC.
* Loading accelerator images into a dedicated partition on the AMD Versal
  AI SoC.

The VMR communicates with the versal-pci driver via:

* A command queue mapped to PCIe PF0 BAR.
* A shared memory region in the PCIe PF0 BAR.

Accelerator Image
-----------------

An AMD Embedded+ platform may store multiple *accelerator images*, each
identified by a UUID in a secure location on the AMD Ryzen host file
system. Each image enables a specific application like sensor fusion, AI
inferencing, etc. A chosen accelerator image is sent to the VMR by the
versal-pci driver. Once loaded, the accelerator image running in the
dedicated partition of AMD Versal AI SoC interfaces with the AMD Ryzen host
via PCIe PF1.

Accelerator Image Loading Workflow
----------------------------------

1. A user application requests an image load by sending the UUID of the
   desired image to the user driver attached to PF1.
2. The user driver forwards the UUID to the versal-pci driver via a mailbox
   channel which connects PF1 to PF0.
3. The versal-pci driver on PF0 receives the UUID via the mailbox channel.
4. The versal-pci driver on PF0 then requests the accelerator image change
   by sending a message to the VMR.
5. The versal-pci driver copies the accelerator image to the shared memory
   region on the PF0 PCIe BAR. It then sends a message to the VMR via the
   command queue.
6. Upon receiving the request from versal-pci, the VMR programs dedicated
   hardware in the AMD Versal AI SoC to perform image loading.
7. The image is loaded over the dedicated partition which then starts
   running.  The VMR sends success response to the versal-pci driver.
8. The versal-pci driver then responds to the user PF driver over the
   communication channel.
9. The versal-pci driver begins monitoring the health of the AMD Versal
   SoC.


The Driver Architecture
=======================

.. code-block:: bash

                          +------------+
                          |            |
                          |     VM     |
       +------------+     |            |
       | versal-pci |     |            |
   +---+----o-------+-----+-----o------+-----+
   |        |       Linux       |            |  AMD Ryzen Host
   +--------|-------------------|------------+
           PF0                 PF1
            |                   |
   +--------v--------+----------v------------+
   |                 .                       |
   |       VMR       .   Accelerator Image   | AMD Versal AI SoC
   |                 .                       |
   +-----------------+-----------------------+


References
==========

* `AMD Embedded Plus <https://www.amd.com/en/products/embedded/embedded-plus.html>`_
* `AMD Embedded Platform Architecture <https://xilinx-wiki.atlassian.net/wiki/spaces/A/pages/3011838141/AMD+Embedded+Platforms>`_
