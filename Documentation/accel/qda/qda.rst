.. SPDX-License-Identifier: GPL-2.0-only

==================================
Qualcomm Hexagon DSP (QDA) Driver
==================================

Introduction
============

The **QDA** (Qualcomm DSP Accelerator) driver is a new DRM-based
accelerator driver for Qualcomm's Hexagon DSPs. It provides a standardized
interface for user-space applications to offload computational tasks ranging
from audio processing and sensor offload to computer vision and AI
inference to the Hexagon DSPs found on Qualcomm SoCs.

This driver is designed to align with the Linux kernel's modern **Compute
Accelerators** subsystem (`drivers/accel/`), providing a robust and modular
alternative to the legacy FastRPC driver in `drivers/misc/`, offering
improved resource management and better integration with standard kernel
subsystems.

Motivation
==========

The existing FastRPC implementation in the kernel utilizes a custom character
device and lacks integration with modern kernel memory management frameworks.
The QDA driver addresses these limitations by:

1.  **Adopting the DRM accel Framework**: Leveraging standard uAPIs for device
    management, job submission, and synchronization.
2.  **Utilizing GEM for Memory**: Providing proper buffer object management,
    including DMA-BUF import/export capabilities.
3.  **Improving Isolation**: Using IOMMU context banks to enforce memory
    isolation between different DSP user sessions.

Key Features
============

*   **Standard Accelerator Interface**: Exposes a standard character device
    node (e.g., `/dev/accel/accel0`) via the DRM subsystem.
*   **Unified Offload Support**: Supports all DSP domains (ADSP, CDSP, SDSP,
    GDSP) via a single driver architecture.
*   **FastRPC Protocol**: Implements the reliable Remote Procedure Call
    (FastRPC) protocol for communication between the application processor
    and DSP.
*   **DMA-BUF Interop**: Seamless sharing of memory buffers between the DSP
    and other multimedia subsystems (GPU, Camera, Video) via standard DMA-BUFs.
*   **Modular Design**: Clean separation between the core DRM logic, the memory
    manager, and the RPMsg-based transport layer.

Architecture
============

The QDA driver is composed of several modular components:

1.  **Core Driver (`qda_drv`)**: Manages device registration, file operations,
    and bridges the driver with the DRM accelerator subsystem.
2.  **Memory Manager (`qda_memory_manager`)**: A flexible memory management
    layer that handles IOMMU context banks. It supports pluggable backends
    (such as DMA-coherent) to adapt to different SoC memory architectures.
3.  **GEM Subsystem**: Implements the DRM GEM interface for buffer management:

    * **`qda_gem`**: Core GEM object management, including allocation, mmap
      operations, and buffer lifecycle management.
    * **`qda_prime`**: PRIME import functionality for DMA-BUF interoperability,
      enabling seamless buffer sharing with other kernel subsystems.

4.  **Transport Layer (`qda_rpmsg`)**: Abstraction over the RPMsg framework
    to handle low-level message passing with the DSP firmware.
5.  **Compute Bus (`qda_compute_bus`)**: A custom virtual bus used to
    enumerate and manage the specific compute context banks defined in the
    device tree.
6.  **FastRPC Core (`qda_fastrpc`)**: Implements the protocol logic for
    marshalling arguments and handling remote invocations.

User-Space API
==============

The driver exposes a set of DRM-compliant IOCTLs. Note that these are designed
to be familiar to existing FastRPC users while adhering to DRM standards.

*   `DRM_IOCTL_QDA_QUERY`: Query DSP type (e.g., "cdsp", "adsp")
    and capabilities.
*   `DRM_IOCTL_QDA_INIT_ATTACH`: Attach a user session to the DSP's protection
    domain.
*   `DRM_IOCTL_QDA_INIT_CREATE`: Initialize a new process context on the DSP.
*   `DRM_IOCTL_QDA_INVOKE`: Submit a remote method invocation (the primary
    execution unit).
*   `DRM_IOCTL_QDA_GEM_CREATE`: Allocate a GEM buffer object for DSP usage.
*   `DRM_IOCTL_QDA_GEM_MMAP_OFFSET`: Retrieve mmap offsets for memory mapping.
*   `DRM_IOCTL_QDA_MAP` / `DRM_IOCTL_QDA_MUNMAP`: Map or unmap buffers into the
    DSP's virtual address space.

Usage Example
=============

A typical lifecycle for a user-space application:

1.  **Discovery**: Open `/dev/accel/accel*` and check
    `DRM_IOCTL_QDA_QUERY` to find the desired DSP (e.g., CDSP for
    compute workloads).
2.  **Initialization**: Call `DRM_IOCTL_QDA_INIT_ATTACH` and
    `DRM_IOCTL_QDA_INIT_CREATE` to establish a session.
3.  **Memory**: Allocate buffers via `DRM_IOCTL_QDA_GEM_CREATE` or import
    DMA-BUFs (PRIME fd) from other drivers using `DRM_IOCTL_PRIME_FD_TO_HANDLE`.
4.  **Execution**: Use `DRM_IOCTL_QDA_INVOKE` to pass arguments and execute
    functions on the DSP.
5.  **Cleanup**: Close file descriptors to automatically release resources and
    detach the session.

Internal Implementation
=======================

Memory Management
-----------------
The driver's memory manager creates virtual "IOMMU devices" that map to
hardware context banks. This allows the driver to manage multiple isolated
address spaces. The implementation currently uses a **DMA-coherent backend**
to ensure data consistency between the CPU and DSP without manual cache
maintenance in most cases.

Debugging
=========
The driver includes extensive dynamic debug support. Enable it via the
kernel's dynamic debug control:

.. code-block:: bash

    echo "file drivers/accel/qda/* +p" > /sys/kernel/debug/dynamic_debug/control
