.. SPDX-License-Identifier: GPL-2.0

==============================
Allocating dma-buf using heaps
==============================

Dma-buf Heaps are a way for userspace to allocate dma-buf objects. They are
typically used to allocate buffers from a specific allocation pool, or to share
buffers across frameworks.

Heaps
=====

A heap represent a specific allocator. The Linux kernel currently supports the
following heaps:

 - The ``system`` heap allocates virtually contiguous, cacheable, buffers

 - The ``reserved`` heap allocates physically contiguous, cacheable, buffers.
   Depending on the platform, it might be called differently:

    - Acer Iconia Tab A500: ``linux,cma``
    - Allwinner sun4i, sun5i and sun7i families: ``default-pool``
    - Amlogic A1: ``linux,cma``
    - Amlogic G12A/G12B/SM1: ``linux,cma``
    - Amlogic GXBB/GXL: ``linux,cma``
    - ASUS EeePad Transformer TF101: ``linux,cma``
    - ASUS Google Nexus 7 (Project Bach / ME370TG) E1565: ``linux,cma``
    - ASUS Google Nexus 7 (Project Nakasi / ME370T) E1565: ``linux,cma``
    - ASUS Google Nexus 7 (Project Nakasi / ME370T) PM269: ``linux,cma``
    - Asus Transformer Infinity TF700T: ``linux,cma``
    - Asus Transformer Pad 3G TF300TG: ``linux,cma``
    - Asus Transformer Pad TF300T: ``linux,cma``
    - Asus Transformer Pad TF701T: ``linux,cma``
    - Asus Transformer Prime TF201: ``linux,cma``
    - ASUS Vivobook S 15: ``linux,cma``
    - Cadence KC705: ``linux,cma``
    - Digi International ConnectCore 6UL: ``linux,cma``
    - Freescale i.MX8DXL EVK: ``linux,cma``
    - Freescale TQMa8Xx: ``linux,cma``
    - Hisilicon Hikey: ``linux,cma``
    - Lenovo ThinkPad T14s Gen 6: ``linux,cma``
    - Lenovo ThinkPad X13s: ``linux,cma``
    - Lenovo Yoga Slim 7x: ``linux,cma``
    - LG Optimus 4X HD P880: ``linux,cma``
    - LG Optimus Vu P895: ``linux,cma``
    - Loongson 2k0500, 2k1000 and 2k2000: ``linux,cma``
    - Microsoft Romulus: ``linux,cma``
    - NXP i.MX8ULP EVK: ``linux,cma``
    - NXP i.MX93 9x9 QSB: ``linux,cma``
    - NXP i.MX93 11X11 EVK: ``linux,cma``
    - NXP i.MX93 14X14 EVK: ``linux,cma``
    - NXP i.MX95 19X19 EVK: ``linux,cma``
    - Ouya Game Console: ``linux,cma``
    - Pegatron Chagall: ``linux,cma``
    - PHYTEC phyCORE-AM62A SOM: ``linux,cma``
    - PHYTEC phyCORE-i.MX93 SOM: ``linux,cma``
    - Qualcomm SC8280XP CRD: ``linux,cma``
    - Qualcomm X1E80100 CRD: ``linux,cma``
    - Qualcomm X1E80100 QCP: ``linux,cma``
    - RaspberryPi: ``linux,cma``
    - Texas Instruments AM62x SK board family: ``linux,cma``
    - Texas Instruments AM62A7 SK: ``linux,cma``
    - Toradex Apalis iMX8: ``linux,cma``
    - TQ-Systems i.MX8MM TQMa8MxML: ``linux,cma``
    - TQ-Systems i.MX8MN TQMa8MxNL: ``linux,cma``
    - TQ-Systems i.MX8MPlus TQMa8MPxL: ``linux,cma``
    - TQ-Systems i.MX8MQ TQMa8MQ: ``linux,cma``
    - TQ-Systems i.MX93 TQMa93xxLA/TQMa93xxCA SOM: ``linux,cma``
    - TQ-Systems MBA6ULx Baseboard: ``linux,cma``

