.. SPDX-License-Identifier: GPL-2.0-only

=====================================
Qualcomm DSP Accelerator (QDA) Driver
=====================================

Introduction
============

The QDA driver is a DRM accel driver for Qualcomm's DSPs. It provides a
DRM accel based interface for Qualcomm DSP offload, supporting workloads
such as AI inference, computer vision, audio processing, and sensor offload
on Qualcomm SoCs. It uses the FastRPC protocol and integrates with DRM and
GEM infrastructure for device and buffer management.

Key Features
============

*   **DRM accel Interface**: Exposes a standard character device node
    (e.g., ``/dev/accel/accel0``) via the DRM accel subsystem.
*   **FastRPC Protocol**: Implements the FastRPC protocol for communication
    between the application processor and the DSP.
*   **GEM Buffer Management**: Uses the DRM GEM interface for buffer
    allocation, lifecycle management, and DMA-BUF import/export.
*   **IOMMU Isolation**: Uses IOMMU context banks to enforce memory isolation
    between different DSP user sessions.
*   **Modular Design**: Clean separation between the core DRM logic, the
    memory manager, and the RPMsg-based transport layer.

Architecture
============

The QDA driver consists of several functional blocks:

1.  **Core Driver (``qda_drv``)**: Manages device registration, file operations,
    and DRM accel integration.
2.  **Memory Manager (``qda_memory_manager``)**: A flexible memory management
    layer that handles IOMMU context banks. It supports pluggable backends
    (such as DMA-coherent) to adapt to different SoC memory architectures.
3.  **GEM Subsystem**: Implements the DRM GEM interface for buffer management:

    * **``qda_gem``**: Core GEM object management, including allocation, mmap
      operations, and buffer lifecycle management.
    * **``qda_prime``**: PRIME import functionality for DMA-BUF interoperability
      with other kernel subsystems.

4.  **Transport Layer (``qda_rpmsg``)**: Abstraction over the RPMsg framework
    to handle low-level message passing with the DSP firmware.
5.  **Compute Bus (``qda_compute_bus``)**: A custom virtual bus used to
    enumerate and manage the specific compute context banks defined in the
    device tree. The bus was introduced because IOMMU context banks (CBs) are
    synthetic constructs — not real platform devices — making a platform driver
    an incorrect abstraction for them. The earlier platform-driver approach also
    had a race condition: device nodes were created before the RPMsg channel
    resources were fully initialized, and because ``probe`` runs asynchronously,
    applications could open a CB device and attempt to start a session before
    the underlying transport was ready. The compute bus makes CB lifetime
    explicitly subordinate to the parent QDA device, closing that window.
6.  **FastRPC Core (``qda_fastrpc``)**: Implements the protocol logic for
    marshalling arguments and handling remote invocations.

User-Space API
==============

The driver exposes a set of DRM-compliant IOCTLs:

*   ``DRM_IOCTL_QDA_QUERY``: Query DSP type (e.g., "cdsp", "adsp")
    and capabilities.
*   ``DRM_IOCTL_QDA_REMOTE_SESSION_CREATE``: Initialize a new process context
    on the DSP.
*   ``DRM_IOCTL_QDA_REMOTE_INVOKE``: Submit a remote method invocation (the
    primary execution unit).
*   ``DRM_IOCTL_QDA_GEM_CREATE``: Allocate a GEM buffer object for DSP usage.
*   ``DRM_IOCTL_QDA_GEM_MMAP_OFFSET``: Retrieve mmap offsets for memory mapping.
*   ``DRM_IOCTL_QDA_REMOTE_MAP`` / ``DRM_IOCTL_QDA_REMOTE_MUNMAP``: Map or unmap
    buffers into the DSP's virtual address space. Each accepts a ``request``
    field selecting between a legacy operation (``QDA_MAP_REQUEST_LEGACY`` /
    ``QDA_MUNMAP_REQUEST_LEGACY``) and an attribute-based operation
    (``QDA_MAP_REQUEST_ATTR`` / ``QDA_MUNMAP_REQUEST_ATTR``).

Usage Example
=============

A typical lifecycle for a user-space application:

1.  **Discovery**: Open ``/dev/accel/accel*`` and use
    ``DRM_IOCTL_QDA_QUERY`` to identify the DSP domain served by that
    device node.
2.  **Initialization**: Call ``DRM_IOCTL_QDA_REMOTE_SESSION_CREATE`` to
    establish a session and create a process context on the DSP.
3.  **Memory**: Allocate buffers via ``DRM_IOCTL_QDA_GEM_CREATE`` or import
    DMA-BUFs (PRIME fd) from other drivers using ``DRM_IOCTL_PRIME_FD_TO_HANDLE``.
4.  **Execution**: Use ``DRM_IOCTL_QDA_REMOTE_INVOKE`` to pass arguments and
    execute functions on the DSP.
5.  **Cleanup**: Close file descriptors to automatically release resources and
    detach the session.

Internal Implementation
=======================

Memory Management
-----------------
The driver's memory manager creates virtual "IOMMU devices" that map to
hardware context banks. This allows the driver to manage multiple isolated
address spaces. The implementation uses a DMA-coherent backend to ensure data consistency
between the CPU and DSP without manual cache maintenance in most cases.

Debugging
=========
The driver includes extensive dynamic debug support. Enable it via the
kernel's dynamic debug control:

.. code-block:: bash

    echo "file drivers/accel/qda/* +p" > /sys/kernel/debug/dynamic_debug/control

Testing
=======
The QDA driver can be exercised using the ``fastrpc_test`` utility from the
FastRPC userspace library. Run the test application:

.. code-block:: bash

    fastrpc_test -d 3 -U 1 -t linux -a v68

**Options**

``-d domain``
    Select the DSP domain to run on:

    * ``0`` — ADSP
    * ``1`` — MDSP
    * ``2`` — SDSP
    * ``3`` — CDSP *(default on targets with CDSP)*

``-U unsigned_PD``
    Select signed or unsigned protection domain:

    * ``0`` — signed PD
    * ``1`` — unsigned PD *(default)*

``-t target``
    Target platform: ``android`` or ``linux`` *(default: linux)*

``-a arch_version``
    DSP architecture version, e.g. ``v68``, ``v75`` *(default: v68)*
