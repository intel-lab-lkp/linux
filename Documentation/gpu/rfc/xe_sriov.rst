.. SPDX-License-Identifier: MIT

========================
Xe – SR-IOV Support Plan
========================

The Single Root I/O Virtualization (SR-IOV) extension to the PCI Express (PCIe)
specification suite is supported starting from 12th generation of Intel Graphics
processors.

This document describes planned ABI of the new Xe driver (see xe.rst) that will
provide flexible configuration and management options related to the SR-IOV.
It will also highlight few most important changes to the Xe driver
implementation to deal with Intel GPU SR-IOV specific requirements.


SR-IOV Capability
=================

Due to SR-IOV complexity and required co-operation between hardware, firmware
and kernel drivers, not all Xe architecture platforms might have SR-IOV enabled
or fully functional.

To control at the driver level which platform will provide support for SR-IOV,
as we can't just rely on the PCI configuration data exposed by the hardware,
we will introduce "has_sriov" flag to the struct xe_device_desc that describes
a device capabilities that driver checks during the probe.

Initially this flag will be set to disabled even on platforms that we plan to
support. We will enable this flag only once we finish merging all required
changes to the driver and related validated firmwares are also made available.


SR-IOV Platforms
================

Initially we plan to add SR-IOV functionality to the following SDV platforms
already supported by the Xe driver:

 - TGL (up to 7 VFs)
 - ADL (up to 7 VFs)
 - MTL (up to 7 VFs)
 - ATSM (up to 31 VFs)
 - PVC (up to 63 VFs)

Newer platforms will be supported later, but we hope that enabling will be
much faster, as majority of the driver changes are either platform agnostic
or are similar between earlier platforms (hence we start with SDVs).


PF Mode
=======

Support in the driver for acting in Physical Function (PF) mode, i.e. mode
that allows configuration of VFs, depends on the CONFIG_PCI_IOV and will be
enabled by default.

However, due to potentially conflicting requirements for SR-IOV and other mega
features, we might want to have an option to disable SR-IOV PF mode support at
the driver load time.

Thus, we plan to use additional modparam named "sriov_totalvfs" which if set to
0 will force the driver to operate in the native (non-virtualized) mode.
The same modparam could be used to limit number of supported Virtual Functions
(VFs) by the driver compared to the hardware limit exposed in PCI configuration.

The name of this modparam corresponds to the existing PCI sysfs attribute, that
by default exposes hardware capability.

The default value of this param will allow to support all possible VFs as
claimed by the hardware.

This modparam will have no effect if driver is running on the VF device.


VFs Enabling
============

To enable or disable VFs we plan to rely on existing sysfs attribute exposed by
the PCI subsystem named "sriov_numvfs". We will provide all necessary tweaks to
provision VFs in our custom implementation of the "sriov_configure" hook from
the struct pci_driver.

If for some reason, including explicit request to disable SR-IOV PF mode using
modparam, we will not be able to correctly support any VFs, driver will change
number of supported VFs, exposed to the userspace by "sriov_totalvfs" attribute,
to 0, thus preventing configuration of the VFs.


VF Mode
=======

When driver is running on the VF device, then due to hardware enforcements,
access to the privileged registers is not possible. To avoid relying on these
registers, we plan to perform early detection if we are running on the VF
device using dedicated VF_CAP(0x1901f8) register and then use global macro
IS_SRIOV_VF(xe) to control the driver logic.

To speed up merging of the required changes, we might first introduce dummy
macro that is always set to false, to prepare driver to avoid some code paths
before we finalize our VF mode detection and other VFs enabling changes.


Resources
=========

Most of the hardware (or firmware) resources available on the Xe architecture,
like GGTT, LMEM, GuC context IDs, GuC doorbells, will be shared between PF and
VFs and will require some provisioning steps to assign those resources for use
by the VF.

Until VFs are provisioned with resources, the PF driver will be able to use all
resources, in the same way as it would be running in non-virtualized mode.

If some resource (of part or region of it) is assigned to specific VF, then PF
is not allowed to use that part or region of the resource, but can continue to
use whatever is left available.

Those resources are usually fully virtualized, so they will not require any
special handling when used by the VF driver, except that VF driver must know
the assigned quota.

The most notable exception is the GGTT address space, as on some platforms,
the VF driver must additionally know the real range that it can access.

Once the resources were assigned to the VF use and the VF driver has started,
then it is not allowed to change such provisioning, as that would break the
VF driver. To make changes the VF driver, which was using these resources,
must be unloaded (or the VM is terminated) and the VF device must be reset
using the FLR.


Scheduling
==========

The workloads from PF driver and VF drivers must be submitted to the hardware
always by using the GuC submission mechanism. Unless VF has exclusive access
to the GT then submissions from different VFs are time-sliced and controlled
with additional "execution_quantum" and "preemption_timeout" parameters.

In contrast to the resource provisioning, those scheduling parameters can be
changed even if VF drivers are already running and are active.


Automatic VFs Provisioning
==========================

To provide out-of-the box experience when user will be enabling VFs using
generic "sriov_numvfs" attribute without requiring complex provisioning steps,
the SR-IOV PF driver will implement automatic VFs resource provisioning.

By default, all VFs will be allocated with the fair amount of the mandatory
resources (like GGTT, GuC IDs) and with unrestricted scheduling parameters.
Such provisioning should be sufficient for most of the normal usages, when
no strict SLA is required.

The PF driver will also expose some additional sysfs files to allow adjusting
this automatic VFs provisioning, like default values for most of the
provisioning parameters that PF will then apply for each enabled VF.

    Details about those extension can be found in
    :download:`Preliminary Xe driver ABI <sysfs-driver-xe-sriov>`.


Manual VFs Provisioning
=======================

If automatic VFs provisioning, which applies same configuration to every VF,
is not sufficient or there is a need for advanced customization of some VF,
the PF driver will also provide extended sysfs interface which will allow
control every provisioning attribute to the lowest feasible level.

It is expected that these low-level attributes will be mostly used by the
advanced users or by the custom tools that will setup configurations that
meet predefined and validated SLA as required by the customers.

    Details about those extension can be found in
    :download:`Preliminary Xe driver ABI <sysfs-driver-xe-sriov>`.


VFs Monitoring
==============

In addition to the resource provisioning or changing scheduling parameters,
the PF driver might also allow configure some monitoring parameters, like
thresholds of adverse events or sample period, to track undesired behavior
of the VFs that could impact the whole system.

Once those thresholds are setup and sampling period is defined, the GuC will
notify the PF driver about which VF is excessing the threshold and then PF is
able to trigger the uevent to notify the administrator (or VMM) that could
take some action against the VF.
