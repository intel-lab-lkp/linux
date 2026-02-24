.. SPDX-License-Identifier: GPL-2.0

=====================
DMA Window Attributes
=====================

In PowerPC architecture there are 2 types of DMA windows -

1. Default 2GB DMA window which is backed by 4K page size
2. A bigger Dynamic DMA Window (DDW) which is backed by larger page size
   (64K or 2MB)

A dedicated device will have both the DMA windows instantiated but an SR-IOV
device will only have the bigger Dynamic DMA Window.

The attributes of these 2 DMA windows are exported to user space via sysfs.
Each IOMMU isolation unit will have its directory created under
/sys/devices/virtual/iommu.

As an exapmple, iommu-phb0001

Under each IOMMU isolation unit, there will be a group of attributes for
"Default 2GB DMA Window" and "Dynamic DMA Window" - spapr-tce-dma and
spapr-tce-ddw respectively.

Attributes under each group

spapr-tce-ddw:
direct_address  dynamic_address       dynamic_size  window_type
direct_size     dynamic_pages_mapped  page_size

spapr-tce-dma:
dynamic_address  dynamic_pages_mapped  dynamic_size  page_size


The bigger Dynamic DMA Window is configured into pre-mapped and/or dynamically
allocated TCEs. If the DDW is in "Hybrid" mode, then both the Direct
(pre-mapped) and Dynamic part of the DMA window will have valid values. Hybrid
mode is valid only for SR-IOV devices.

DMA Window properties:

direct_address              Starting address of the pre-mapped DMA window
direct_size                 Size of the pre-mapped DMA Window
dynamic_address             Starting address of the dynamic allocations
dynamic_size                Size of the dynamic allocation window
dynamic_pages_mapped        Pages mapped for DMA by dynamic allocations
page_size                   Page size backing the DMA window
window_type                 Type of the DMA Window (Direct/Dynamic/Hybrid)


An example of DDW attributes for an SR-IOV device::

    $ cd /sys/devices/virtual/iommu/iommu-phb0001/spapr-tce-ddw

    $ grep . *

    direct_address:0x800000000000000   <-- Starting addr of pre-mapped Window
    direct_size:137438953472           <-- Size of pre-mapped Window (128GB)
    dynamic_address:0x800002000000000  <-- Starting addr of Dynamic allocations
    dynamic_size:412316860416          <-- Size of dynamic allocation window (384GB)
    dynamic_pages_mapped:270           <-- Pages mapped by dynamic allocations
    page_size:2097152                  <-- DMA window page size (2MB)
    window_type:Hybrid                 <-- window has both pre-mapped and
                                           dynamic sections
