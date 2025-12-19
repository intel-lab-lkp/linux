.. SPDX-License-Identifier: GPL-2.0

=========================
PRAMIN aperture mechanism
=========================

.. note::
   The following description is approximate and current as of the Ampere family.
   It may change for future generations and is intended to assist in understanding
   the driver code.

Introduction
============

PRAMIN is a hardware aperture mechanism that provides CPU access to GPU Video RAM (VRAM) before
the GPU's Memory Management Unit (MMU) and page tables are initialized. This 1MB sliding window,
located at a fixed offset within BAR0, is essential for setting up page tables and other critical
GPU data structures without relying on the GPU's MMU.

Architecture Overview
=====================

The PRAMIN aperture mechanism is logically implemented by the GPU's PBUS (PCIe Bus Controller Unit)
and provides a CPU-accessible window into VRAM through the PCIe interface::

    +-----------------+    PCIe     +------------------------------+
    |      CPU        |<----------->|           GPU                |
    +-----------------+             |                              |
                                    |  +----------------------+    |
                                    |  |       PBUS           |    |
                                    |  |  (Bus Controller)    |    |
                                    |  |                      |    |
                                    |  |  +--------------+<------------ (window starts at
                                    |  |  |   PRAMIN     |    |    |     BAR0 + 0x700000)
                                    |  |  |   Window     |    |    |
                                    |  |  |   (1MB)      |    |    |
                                    |  |  +--------------+    |    |
                                    |  |         |            |    |
                                    |  +---------|------------+    |
                                    |            |                 |
                                    |            v                 |
                                    |  +----------------------+<------------ (Program PRAMIN to any
                                    |  |       VRAM           |    |    64KB-aligned VRAM boundary)
                                    |  |    (Several GBs)     |    |
                                    |  |                      |    |
                                    |  |  FB[0x000000000000]  |    |
                                    |  |          ...         |    |
                                    |  |  FB[0x7FFFFFFFFFF]   |    |
                                    |  +----------------------+    |
                                    +------------------------------+

PBUS (PCIe Bus Controller) is responsible for, among other things, handling MMIO
accesses to the BAR registers.

PRAMIN Window Operation
=======================

The PRAMIN window provides a 1MB sliding aperture that can be repositioned over
the entire VRAM address space using the ``NV_PBUS_BAR0_WINDOW`` register.

Window Control Mechanism
-------------------------

The window position is controlled via the PBUS ``BAR0_WINDOW`` register::

    NV_PBUS_BAR0_WINDOW Register (0x1700):
    +-------+--------+--------------------------------------+
    | 31:26 | 25:24  |               23:0                   |
    | RSVD  | TARGET |            BASE_ADDR                 |
    |       |        |        (bits 39:16 of VRAM address)  |
    +-------+--------+--------------------------------------+

    BASE_ADDR field (bits 23:0):
    - Contains bits [39:16] of the target VRAM address
    - Provides 40-bit (1TB) address space coverage
    - Must be programmed with 64KB-aligned addresses

    TARGET field (bits 25:24):
    - 0x0: VRAM (Video Memory)
    - 0x1: SYS_MEM_COH (Coherent System Memory)
    - 0x2: SYS_MEM_NONCOH (Non-coherent System Memory)
    - 0x3: Reserved

    .. note::
       Nova only uses TARGET=VRAM (0x0) for video memory access. The SYS_MEM
       target values are documented here for hardware completeness but are
       not used by the driver.

64KB Alignment Requirement
---------------------------

The PRAMIN window must be aligned to 64KB boundaries in VRAM. This is enforced
by the ``BASE_ADDR`` field representing bits [39:16] of the target address::

    VRAM Address Calculation:
    actual_vram_addr = (BASE_ADDR << 16) + pramin_offset
    Where:
    - BASE_ADDR: 24-bit value from NV_PBUS_BAR0_WINDOW[23:0]
    - pramin_offset: 20-bit offset within the PRAMIN window [0x00000-0xFFFFF]

    Example Window Positioning:
    +---------------------------------------------------------+
    |                    VRAM Space                           |
    |                                                         |
    |  0x000000000  +-----------------+ <-- 64KB aligned      |
    |               | PRAMIN Window   |                       |
    |               |    (1MB)        |                       |
    |  0x0000FFFFF  +-----------------+                       |
    |                                                         |
    |       |              ^                                  |
    |       |              | Window can slide                 |
    |       v              | to any 64KB-aligned boundary     |
    |                                                         |
    |  0x123400000  +-----------------+ <-- 64KB aligned      |
    |               | PRAMIN Window   |                       |
    |               |    (1MB)        |                       |
    |  0x1234FFFFF  +-----------------+                       |
    |                                                         |
    |                       ...                               |
    |                                                         |
    |  0x7FFFF0000  +-----------------+ <-- 64KB aligned      |
    |               | PRAMIN Window   |                       |
    |               |    (1MB)        |                       |
    |  0x7FFFFFFFF  +-----------------+                       |
    +---------------------------------------------------------+
