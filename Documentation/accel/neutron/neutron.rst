.. SPDX-License-Identifier: GPL-2.0-only

.. include:: <isonum.txt>

====================
 Neutron NPU Driver
====================

:Copyright: |copy| 2026 NXP

Overview
========

Neutron is NXP's eIQ Neutron Neural Processing Unit (NPU). It is a highly
scalable, power-efficient machine learning accelerator targeting quantized
ML models for edge AI applications. Neutron is integrated into i.MX95 and
other NXP platforms.

A more detailed description of Neutron NPU and usage scenarios can be
found at [1]_.

Hardware Description
====================

Neutron has the following hardware components:

- RISC-V core: this is the "brain" of the Neutron NPU. It runs a proprietary
  firmware responsible for programming registers, processing commands and
  managing the other hardware components
- one or more Neutron cores: the main computation engine performing Machine
  Learning (ML) operations
- TCM: a dedicated fast memory
- Data Mover: a DMA engine that handles data transfers between system memory
  and Neutron's internal memory

Software Stack
==============

The following software components are required for running an inference
on the Neutron accelerator:

- Neutron converter [2]_, [3]_: this is an offline tool that converts models
  from standard TFLite (LiteRT) format to a custom format for execution on the
  Neutron NPU;
- An inference engine, e.g. LiteRT's XNNPack, which in turn uses
- A LiteRT custom delegate [4]_ to dispatch custom operators to Neutron NPU;
- A userspace library [5]_ that the delegate links to, which wraps IOCTLs
  to the kernel driver in a higher-level API. It handles microcode, weights
  and kernels preparation and base address computations needed by the NPU for
  job execution. It also triggers cache syncs when required;
- The Neutron kernel driver, which handles device initialization and
  communicates directly with the Neutron firmware;
- Neutron firmware [5]_, a proprietary firmware that executes on the RISC-V
  core and directly drives the execution of the NPU hardware.

Usage Flow
==========

This section describes the steps required to run an inference job on the
Neutron NPU.

Offline Conversion
------------------

The first step is to convert a standard TFLite model using the Neutron
converter. Supported standard operators are extracted together and mapped
to one or multiple **NeutronGraph** custom operators in the converted model.
Standard operators that are not supported by the NPU are left unchanged and
will be executed on the CPU.

Runtime Flow
------------

On the platform's Cortex-A cores running Linux, the LiteRT inference engine
is responsible for loading the ML model, pre-processing the input data and
handing over the tensor computation to the NPU via the custom delegate.

The inference engine can be exercised via one of the standard TFLite tools
(e.g. benchmark_model, label_image, etc) or via any custom application that
uses the LiteRT runtime API.

When preparing to run an inference job, userspace requests a memory buffer
from the kernel driver. It loads both the model and the input data in the
buffer, while also reserving a section for the inference output. It then
issues a job submission command with the prepared buffer and waits for
completion.

The kernel driver sends the inference job details to the Neutron firmware
via mailbox registers. The NPU executes the inference and issues an interrupt
to the Linux core once it is finished. The driver in return marks the job
as complete so userspace can access and post-process the output.

Boot Sequence
=============

The Neutron driver is responsible for loading the firmware image and
initiating the NPU boot sequence. The device is powered down during suspend
and each resume operation implies running the firmware load and boot sequence
again.

Hardware Constraints
====================

Cache Coherency
---------------

Some of the NXP platforms that Neutron is integrated on, including i.MX95,
do not ensure Neutron memory coherency at hardware level, generating the
need for explicit DMA sync operations. Given that only parts of the memory
buffer may require syncing at any given time (e.g. multiple inferences using
the same model but different input data) and that the kernel driver is unaware
of the buffer partitioning, the sync operations are driven from userspace.

Buffer alignment
----------------

The Neutron DMA engine requires the inference buffers to be aligned to 1MB
boundary. We allocate buffers for Neutron NPU from a reserved CMA pool that
satisfies this alignment requirement.

References
==========

.. [1] i.MX Machine Learning User's Guide: https://www.nxp.com/docs/en/user-guide/UG10166.pdf
.. [2] Neutron Converter binary and User Guide available for download here:
       https://www.nxp.com/design/design-center/software/eiq-ai-development-environment/eiq-toolkit-for-end-to-end-model-development-and-deployment:EIQ-TOOLKIT
.. [3] NXP's eIQ PyPi repository: https://eiq.nxp.com/repository/eiq-neutron-sdk/
.. [4] TFLite delegate source code: https://github.com/nxp-imx/tflite-neutron-delegate
.. [5] Neutron firmware, library and TFLite delegate available here as binaries:
       https://github.com/nxp-upstream/neutron/tree/upstream

