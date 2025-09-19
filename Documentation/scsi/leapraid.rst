.. SPDX-License-Identifier: GPL-2.0

=========================
LeapRaid Driver for Linux
=========================

Introduction
============

LeapRaid is a storage RAID controller driver developed by LeapIO Tech.
The controller targets enterprise storage, cloud infrastructure, high
performance computing (HPC), and AI workloads.

It provides high-performance storage virtualization over PCI Express
Gen4 and supports both SAS and SATA HDDs and SSDs. It offers both Host
Bus Adapter (HBA) and RAID modes to meet diverse deployment requirements.

Features
========
- PCIe Gen4 x8 host interface
- Support for SAS and SATA devices
- RAID levels: 0, 1, 10, 5, 50, 6, 60
- Advanced error handling and end-to-end data integrity
- NVMe/SATA/SAS tri-mode connectivity (future roadmap)

File Location
=============
The driver source is located at:

``drivers/scsi/leapraid/``

.. note::

   This document is intended for kernel developers and system
   integrators who need to build, test, and deploy the LeapRaid driver.