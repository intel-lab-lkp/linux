.. SPDX-License-Identifier: GPL-2.0
.. Copyright (c) 2019-2026 Daniel P. Smith <dpsmith@apertussolutions.com>

======================
Secure Launch Overview
======================

:Author: Daniel P. Smith
:Date: February 2026

Overview
========

The TrenchBoot project initiated development of the Secure Launch kernel
feature to establish a generalized framework for dynamic root-of-trust
measurement (DRTM). The effort addresses two core objectives: (1) exposing the
platform-specific launch protocols (e.g., Intel TXT, AMD SKINIT, Arm DRTM, and
potentially OpenPOWER) directly to the Linux kernel, and (2) enabling the
kernel to act as the dynamic launch initiator. This design provides the
necessary foundation for the Linux kernel to support a broad range of dynamic
launch use cases without platform-specific user-space intermediaries.

.. note::
    A quick note on terminology. The larger open source project itself is
    called TrenchBoot, which is hosted on GitHub (links below). The kernel
    feature enabling the use of the x86 technology is referred to as "Secure
    Launch" within the kernel code.

Goals
=====

The primary use case initially targeted by the TrenchBoot project is the
ability for the Linux kernel to be launched via a dynamic launch during the
early boot sequence. In this scenario, the dynamic launch is initiated by a
bootloader that has been extended with Secure Launch support. The first
bootloader targeted was GRUB2.

An essential part of establishing measurement-based launch integrity is
ensuring that all components intended for execution (kernel image, initrd,
etc.) and all configuration data that will affect kernel execution (command
line, boot parameters, etc.) are measured prior to execution. These
measurements are then stored securely using the Trusted Platform Module (TPM).

Both Intel TXT and AMD dynamic launch implementations use the TPM for this
purpose. The architecture is designed so that a dynamic launch unlocks a
specific set of Platform Configuration Registers (PCRs) reserved for DRTM
(Dynamic Root of Trust for Measurement) measurements. These registers are known
as the **DRTM PCRs** (PCRs 17-22).

Further details on the hardware mechanisms are documented in Intel's Trusted
Execution Technology specification for the ``GETSEC`` instruction and AMD's
documentation for the ``SKINIT`` instruction. See the `Resources` section for
references.

.. note::
    Currently, only Intel TXT is supported in this first release of the Secure
    Launch feature. AMD/Hygon SKINIT and Arm support will be added in a
    subsequent release.

To enable the Linux kernel to be launched via the Intel ``GETSEC`` instruction,
a Secure Launch entry point is built into the mainline kernel. This entry point
is responsible for handling the specific processor state that the dynamic
launch process leaves the Bootstrap Processor (BSP) in.

The Secure Launch entry point performs the earliest possible measurements of
all components and data that the kernel will consume (kernel image, initrd,
command line, boot parameters, etc.). Both this entry point code and subsequent
kernel initialization code must also correctly handle the specific state that
the dynamic launch leaves the Application Processors (APs) in.

Design Decisions
================

Several design decisions were made during the development of the Secure Launch
feature. The primary guiding principle was to minimize the modifications to the
existing boot path of the kernel as little as possible.

The following illustrate how the implementation followed these principle:

 - All the entry point code necessary to properly configure the system post
   launch is found in sl_stub.S in the kernel image. This code
   validates the state of the system, restores necessary system operating
   configurations and properly handles post launch CPU states.
 - Support is introduced in the SMP boot code to properly wake the APs. This
   is required due to the unique state the dynamic launch leaves the APs in
   (i.e. they cannot be woken with the standard INIT-SIPI sequence).
 - Final setup for the Secure Launch kernel is done in a separate Secure Launch
   module that is loaded via a late initcall. This code is responsible for
   setting up the securityfs interface to allow access to the TPM event log and
   public TXT registers.
 - On the reboot and kexec paths, calls are made to a function to finalize the
   state of the Secure Launch kernel.

Basic Boot Flow
===============

Outlined here is a summary of the boot flow for Secure Launch. A more detailed
review of the Secure Launch process can be found in the Secure Launch
Specification (a link is in the `Resources`_ section).

Pre-launch: *Phase where the environment is prepared and configured to initiate
the secure launch by the boot chain.*

 - The SLRT is initialized, and dl_stub is placed in memory.
 - Load the kernel, initrd and ACM [4]_ into memory.
 - Set up the TXT heap and page tables describing the MLE [1]_ per the
   specification.
 - If non-UEFI platform, SLRT is registered in boot params and dl_stub is
   called from the legacy setup kernel.
 - If UEFI platform, SLRT registered with UEFI and kernel efi-stub support calls
   dl_stub after executing EBS.
 - The dl_stub will prepare the CPU and the TPM for the launch.
 - The secure launch is then initiated with the GETSET[SENTER] instruction.

Post-launch: *Phase where control is passed from the ACM to the MLE and the secure
kernel begins execution.*

 - Entry from the dynamic launch jumps to the SL stub.
 - SL stub fixes up the world on the BSP.
 - For TXT,
    - SL stub wakes the APs, fixes up their worlds.
    - APs are left in an optimized (MONITOR/MWAIT) wait state.
 - SL main does validation of buffers and memory locations. It sets
   the boot parameter loadflag value SLAUNCH_FLAG to inform the main
   kernel that a Secure Launch was done.
 - SL main locates the TPM event log and writes the measurements of
   configuration and module information into it.
 - The SMP bring up code is modified to wake the waiting APs via the monitor
   address.
 - SL platform module is registered as a late initcall module.
 - SL platform module initializes the securityfs interface to allow
   access to the TPM event log and TXT public registers.
 - Kernel boot finishes booting normally.
 - SEXIT support to leave SMX mode is present on the kexec path and
   the various reboot paths (poweroff, reset, halt).

PCR Usage
=========

The TCG DRTM architecture defines three PCRs for dynamic root of trust
measurement:

- PCR.Details (PCR 17)
- PCR.Authorities (PCR 18)
- PCR.DLME_Authority (PCR 19)

Further details on the semantics of the Details and Authorities PCRs are
available in the TCG DRTM Architecture specification.

The Linux kernel's Secure Launch implementation adheres to the TrenchBoot
Secure Launch Specification. It utilizes a measurement policy stored in the
`Secure Launch Resource Table` (``SLRT``) to determine both what to measure and
into which PCR each measurement should be extended.

This policy makes it possible for the kernel to store its own DRTM measurements
(such as an external initrd image) into **PCR.DLME_Detail** (PCR 20). When
combined with storing user authority information into **PCR.DLME_Authority**
(PCR 19), it enables sealing and attestation across different combinations of
platform and user details/authorities.

An example of this approach was presented in the FOSDEM 2021 talk titled
"Secure Upgrades with DRTM".

Configuration
=============

The settings to enable Secure Launch using Kconfig are under::

  "Processor type and features" --> "Secure Launch support"

A kernel with this option enabled can still be booted using other supported
methods.

To reduce the Trusted Computing Base (TCB) of the MLE [1]_, the build
configuration should be pared down as narrowly as one's use case allows.
Fewer drivers (less active hardware) and features reduce the attack surface.
As an example in the extreme, the MLE could only have local disk access with no
other hardware supports except optional network access for remote attestation.

It is also desirable, if possible, to embed the initrd used with the MLE kernel
image to reduce complexity.

The following are important configuration necessities to always consider:

IOMMU Configuration
-------------------

When doing a Secure Launch, the IOMMU should always be enabled and the drivers
loaded. However, IOMMU passthrough mode should never be used. This leaves the
MLE completely exposed to DMA after the PMRs [2]_ are disabled. The current
default mode is to use IOMMU in lazy translated mode, but strict translated
mode, is the preferred IOMMU mode and this should be selected in the build
configuration::

  "Device Drivers" -->
      "IOMMU Hardware Support" -->
          "IOMMU default domain type" -->
              "(X) Translated - Strict"

In addition, the Intel IOMMU should be on by default. The following sets this as the
default in the build configuration::

  "Device Drivers" -->
      "IOMMU Hardware Support" -->
          "Support for Intel IOMMU using DMA Remapping Devices [*]"

and::

  "Device Drivers" -->
      "IOMMU Hardware Support" -->
          "Support for Intel IOMMU using DMA Remapping Devices [*]" -->
              "Enable Intel DMA Remapping Devices by default  [*]"

It is recommended that no other command line options should be set to override
the defaults above. If there is a desire to run an alternate configuration,
then that configuration should be evaluated for what benefits might
be gained against the risks for DMA attacks to which the kernel is likely
going to be exposed.

Secure Launch Resource Table
============================

The Secure Launch Resource Table (SLRT) is a platform-agnostic, standard format
for providing information for the pre-launch environment and to pass
information to the post-launch environment. The table is populated by one or
more bootloaders in the boot chain and used by Secure Launch on how to set up
the environment during post-launch. The details for the SLRT are documented
in the TrenchBoot Secure Launch Specification [3]_.

Intel TXT Interface
===================

The primary interfaces between the various components in TXT are the TXT MMIO
registers and the TXT heap. The MMIO register banks are described in Appendix B
of the TXT MLE [1]_ Development Guide.

The TXT heap is described in Appendix C of the TXT MLE [1]_ Development
Guide. Most of the TXT heap is predefined in the specification. The heap is
initialized by firmware and the pre-launch environment and is subsequently used
by the SINIT ACM. One section, called the OS to MLE Data Table, is reserved for
software to define. This table is set up per the recommendation detailed in
Appendix B of the TrenchBoot Secure Launch Specification::

        /*
         * Secure Launch defined OS/MLE TXT Heap table
         */
        struct txt_os_mle_data {
                u32 version;
                u32 reserved;
                u64 slrt;
                u64 txt_info;
                u32 ap_wake_block;
                u32 ap_wake_block_size;
                u8 mle_scratch[64];
        } __packed;

Description of structure:

=====================  ========================================================================
Field                  Use
=====================  ========================================================================
version                Structure version, current value 1
slrt                   Physical address of the Secure Launch Resource Table
txt_info               Pointer into the SLRT for easily locating TXT specific table
ap_wake_block          Physical address of the block of memory for parking APs after a launch
ap_wake_block_size     Size of the AP wake block
mle_scratch            Scratch area used post-launch by the MLE kernel. Fields:
 
                        - SL_SCRATCH_AP_EBX area to share %ebx base pointer among CPUs
                        - SL_SCRATCH_AP_JMP_OFFSET offset to abs. ljmp fixup location for APs
                        - SL_SCRATCH_AP_STACKS_OFFSET offset to AP startup stacks in wake block
=====================  ========================================================================

Error Codes
===========

The TXT specification defines the layout for TXT 32 bit error code values.
The bit encodings indicate where the error originated (e.g. with the CPU,
in the SINIT ACM, in software). The error is written to a sticky TXT
register that persists across resets called TXT.ERRORCODE (see the TXT
MLE Development Guide). The errors defined by the Secure Launch feature are
those generated in the MLE software. They have the format::

  0xc0008XXX

The low 12 bits are free for defining the following Secure Launch specific
error codes.

0xc0008001: SL_ERROR_GENERIC
----------------------------

Description:

Generic catch all error. Currently unused.

0xc0008002: SL_ERROR_TPM_INIT
-----------------------------

Description:

The Secure Launch code failed to get access to the TPM hardware interface.
This is most likely due to misconfigured hardware or kernel. Ensure the TPM
chip is enabled, and the kernel TPM support is built in (it should not be built
as a module).

0xc0008003: SL_ERROR_TPM_INVALID_LOG20
--------------------------------------

Description:

Either the Secure Launch code failed to find a valid event log descriptor for a
version 2.0 TPM, or the event log descriptor is malformed. Usually this
indicates incompatible versions of the pre-launch environment and the
MLE kernel. The pre-launch environment and the kernel share a structure in the
TXT heap and if this structure (the OS-MLE table) is mismatched, this error is
common. This TXT heap area is set up by the pre-launch environment, so the
issue may originate there. It could also be the sign of an attempted attack.

0xc0008004: SL_ERROR_TPM_LOGGING_FAILED
---------------------------------------

Description:

There was a failed attempt to write a TPM event to the event log early in the
Secure Launch process. This is likely the result of a malformed TPM event log
buffer. Formatting of the event log buffer information is done by the
pre-launch environment, so the issue most likely originates there.

0xc0008005: SL_ERROR_REGION_STRADDLE_4GB
----------------------------------------

Description:

During early validation, a buffer or region was found to straddle the 4Gb
boundary. Because of the way TXT provides DMA memory protection, this is an unsafe
configuration and is flagged as an error. This is most likely a configuration
issue in the pre-launch environment. It could also be the sign of an attempted
attack.

0xc0008006: SL_ERROR_TPM_EXTEND
-------------------------------

Description:

There was a failed attempt to extend a TPM PCR in the Secure Launch platform
module. This is most likely to due to misconfigured hardware or kernel. Ensure
the TPM chip is enabled, and the kernel TPM support is built in (it should not
be built as a module).

0xc0008007: SL_ERROR_MTRR_INV_VCNT
----------------------------------

Description:

During early Secure Launch validation, an invalid variable MTRR count was
found. The pre-launch environment passes several MSR values to the MLE to
restore including the MTRRs. The values are restored by the Secure Launch early
entry point code. After measuring the values supplied by the pre-launch
environment, a discrepancy was found, validating the values. It could be the
sign of an attempted attack.

0xc0008008: SL_ERROR_MTRR_INV_DEF_TYPE
--------------------------------------

Description:

During early Secure Launch validation, an invalid default MTRR type was found.
See SL_ERROR_MTRR_INV_VCNT for more details.

0xc0008009: SL_ERROR_MTRR_INV_BASE
----------------------------------

Description:

During early Secure Launch validation, an invalid variable MTRR base value was
found. See SL_ERROR_MTRR_INV_VCNT for more details.

0xc000800a: SL_ERROR_MTRR_INV_MASK
----------------------------------

Description:

During early Secure Launch validation, an invalid variable MTRR mask value was
found. See SL_ERROR_MTRR_INV_VCNT for more details.

0xc000800b: SL_ERROR_MSR_INV_MISC_EN
------------------------------------

Description:

During early Secure Launch validation, an invalid miscellaneous enable MSR
value was found. See SL_ERROR_MTRR_INV_VCNT for more details.

0xc000800c: SL_ERROR_INV_AP_INTERRUPT
-------------------------------------

Description:

The application processors (APs) wait to be woken up by the SMP initialization
code. The only interrupt that they expect is an NMI; all other interrupts
should be masked. If an AP gets an interrupt other than an NMI, it will
cause this error. This error is very unlikely to occur.

0xc000800d: SL_ERROR_INTEGER_OVERFLOW
-------------------------------------

Description:

A buffer base and size passed to the MLE caused an integer overflow when
added together. This is most likely a configuration issue in the pre-launch
environment. It could also be the sign of an attempted attack.

0xc000800e: SL_ERROR_HEAP_WALK
------------------------------

Description:

An error occurred in TXT heap walking code. The underlying issue is a failure to
early_memremap() portions of the heap, most likely due to a resource shortage.

0xc000800f: SL_ERROR_HEAP_MAP
-----------------------------

Description:

This error is essentially the same as SL_ERROR_HEAP_WALK, but occurred during the
actual early_memremap() operation.

0xc0008010: SL_ERROR_REGION_ABOVE_4GB
-------------------------------------

Description:

A memory region used by the MLE is above 4Gb. In general, this is not a problem
because memory > 4Gb can be protected from DMA. There are certain buffers that
should never be above 4Gb, and one of these caused the violation. This is most
likely a configuration issue in the pre-launch environment. It could also be
the sign of an attempted attack.

0xc0008011: SL_ERROR_HEAP_INVALID_DMAR
--------------------------------------

Description:

The backup copy of the ACPI DMAR table, which is expected to be in the
TXT heap, could not be found. This is due to a bug in the platform's ACM module
or in firmware.

0xc0008012: SL_ERROR_HEAP_DMAR_SIZE
-----------------------------------

Description:

The backup copy of the ACPI DMAR table in the TXT heap is too large to be stored
for later usage. This error is very unlikely to occur since the area reserved
for the copy is far larger than the DMAR should be.

0xc0008013: SL_ERROR_HEAP_DMAR_MAP
----------------------------------

Description:

The backup copy of the ACPI DMAR table in the TXT heap could not be mapped. The
underlying issue is a failure to early_memremap() the DMAR table, most likely
due to a resource shortage.

0xc0008014: SL_ERROR_HI_PMR_BASE
--------------------------------

Description:

On a system with more than 4Gb of RAM, the high PMR [2]_ base address should be
set to 4Gb. This error is due to that not being the case. This PMR value is set
by the pre-launch environment, so the issue most likely originates there. It
could also be the sign of an attempted attack.

0xc0008015: SL_ERROR_HI_PMR_SIZE
--------------------------------

Description:

On a system with more than 4Gb of RAM, the high PMR [2]_ size should be set to
cover all RAM > 4Gb. This error is due to that not being the case. This PMR
value is set by the pre-launch environment, so the issue most likely originates
there. It could also be the sign of an attempted attack.

0xc0008016: SL_ERROR_LO_PMR_BASE
--------------------------------

Description:

The low PMR [2]_ base should always be set to address zero. This error is due
to that not being the case. This PMR value is set by the pre-launch environment
so the issue most likely originates there. It could also be the sign of an
attempted attack.

0xc0008017: SL_ERROR_LO_PMR_MLE
-------------------------------

Description:

This error indicates the MLE image is not covered by the low PMR [2]_ range.
The PMR values are set by the pre-launch environment, so the issue most likely
originates there. It could also be the sign of an attempted attack.

0xc0008018: SL_ERROR_INITRD_TOO_BIG
-----------------------------------

Description:

The external initrd provided is larger than 4Gb. This is not a valid
configuration for Secure Launch due to managing DMA protection.

0xc0008019: SL_ERROR_HEAP_ZERO_OFFSET
-------------------------------------

Description:

During a TXT heap walk, an invalid/zero next table offset value was found. This
indicates the TXT heap is malformed. The TXT heap is initialized by the
pre-launch environment, so the issue most likely originates there. It could
also be a sign of an attempted attack. In addition, ACM is also responsible for
manipulating parts of the TXT heap, so the issue could be due to a bug in the
platform's ACM module.

0xc000801a: SL_ERROR_WAKE_BLOCK_TOO_SMALL
-----------------------------------------

Description:

The AP wake block buffer passed to the MLE via the OS-MLE TXT heap table is not
large enough. This value is set by the pre-launch environment, so the issue
most likely originates there. It also could be the sign of an attempted attack.

0xc000801b: SL_ERROR_MLE_BUFFER_OVERLAP
---------------------------------------

Description:

One of the buffers passed to the MLE via the OS-MLE TXT heap table overlaps
with the MLE image in memory. This value is set by the pre-launch environment
so the issue most likely originates there. It could also be the sign of an
attempted attack.

0xc000801c: SL_ERROR_BUFFER_BEYOND_PMR
--------------------------------------

Description:

One of the buffers passed to the MLE via the OS-MLE TXT heap table is not
protected by a PMR. This value is set by the pre-launch environment, so the
issue most likely originates there. It could also be the sign of an attempted
attack.

0xc000801d: SL_ERROR_OS_SINIT_BAD_VERSION
-----------------------------------------

Description:

The version of the OS-SINIT TXT heap table is bad. It must be 6 or greater.
This value is set by the pre-launch environment, so the issue most likely
originates there. It could also be the sign of an attempted attack. It is also
possible though very unlikely that the platform is so old that the ACM being
used requires an unsupported version.

0xc000801e: SL_ERROR_EVENTLOG_MAP
---------------------------------

Description:

An error occurred in the Secure Launch module while mapping the TPM event log.
The underlying issue is memremap() failure, most likely due to a resource
shortage.

0xc000801f: SL_ERROR_TPM_INVALID_ALGS
-------------------------------------

Description:

The TPM 2.0 event log reports either no hashing algorithms, invalid algorithm ID
or an algorithm size larger than the max size recognized by the TPM support code.

0xc0008020: SL_ERROR_TPM_EVENT_COUNT
------------------------------------

Description:

The TPM 2.0 event log contains an event with a digest count that is not equal
to the algorithm count of the overall log. This is an invalid configuration
that could indicate either a bug or a possible attack.

0xc0008021: SL_ERROR_TPM_INVALID_EVENT
--------------------------------------

Description:

An invalid/malformed event was found in the TPM event log while reading it.
Since only trusted entities are supposed to be writing the event log, this
would indicate either a bug or a possible attack.

0xc0008022: SL_ERROR_INVALID_SLRT
---------------------------------

Description:

The Secure Launch Resource Table is invalid or malformed and is unusable. This
implies the pre-launch code did not properly set up the SLRT.

0xc0008023: SL_ERROR_SLRT_MISSING_ENTRY
---------------------------------------

Description:

The Secure Launch Resource Table is missing a required entry within it. This
implies the pre-launch code did not properly set up the SLRT.

0xc0008024: SL_ERROR_SLRT_MAP
-----------------------------

Description:

An error occurred in the Secure Launch module while mapping the Secure Launch
Resource table. The underlying issue is memremap() failure, most likely due to
a resource shortage.


Resources
=========

The TrenchBoot project:

https://trenchboot.org

Secure Launch Specification:

https://trenchboot.org/specifications/Secure_Launch/

Trusted Computing Group's D-RTM Architecture:

https://trustedcomputinggroup.org/wp-content/uploads/TCG_D-RTM_Architecture_v1-0_Published_06172013.pdf

TXT documentation in the Intel TXT MLE Development Guide:

https://www.intel.com/content/dam/www/public/us/en/documents/guides/intel-txt-software-development-guide.pdf

TXT instructions documentation in the Intel SDM Instruction Set volume:

https://software.intel.com/en-us/articles/intel-sdm

AMD SKINIT documentation in the System Programming manual:

https://www.amd.com/system/files/TechDocs/24593.pdf

GRUB Secure Launch support:

https://github.com/TrenchBoot/grub/tree/grub-sl-fc-38-dlstub

FOSDEM 2021: Secure Upgrades with DRTM

https://archive.fosdem.org/2021/schedule/event/firmware_suwd/

.. [1]
    MLE: Measured Launch Environment is the binary runtime that is measured and
    then run by the TXT SINIT ACM. The TXT MLE Development Guide describes the
    requirements for the MLE in detail.

.. [2]
    PMR: Intel VTd has a feature in the IOMMU called Protected Memory Registers.
    There are two of these registers and they allow all DMA to be blocked
    to large areas of memory. The low PMR can cover all memory below 4Gb on 2Mb
    boundaries. The high PMR can cover all RAM on the system, again on 2Mb
    boundaries. This feature is used during a Secure Launch by TXT.

.. [3]
    Secure Launch Specification: https://trenchboot.org/specifications/Secure_Launch/

.. [4]
    ACM: Intel's Authenticated Code Module. This is the 32b bit binary blob that
    is run securely by the GETSEC[SENTER] during a measured launch. It is described
    in the Intel documentation on TXT and versions for various chipsets are
    signed and distributed by Intel.
