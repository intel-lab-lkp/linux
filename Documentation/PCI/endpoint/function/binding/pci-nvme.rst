.. SPDX-License-Identifier: GPL-2.0

==========================
PCI NVMe Endpoint Function
==========================

1) Create the function subdirectory pci_epf_nvme.0 in the
pci_ep/functions/pci_epf_nvme directory of configfs.

Standard EPF Configurable Fields:

================   ===========================================================
vendorid           Do not care (e.g. PCI_ANY_ID)
deviceid           Do not care (e.g. PCI_ANY_ID)
revid              Do not care
progif_code	   Must be 0x02 (NVM Express)
baseclass_code     Must be 0x1 (PCI_BASE_CLASS_STORAGE)
subclass_code      Must be 0x08 (Non-Volatile Memory controller)
cache_line_size    Do not care
subsys_vendor_id   Do not care (e.g. PCI_ANY_ID)
subsys_id          Do not care (e.g. PCI_ANY_ID)
msi_interrupts     At least equal to the number of queue pairs desired
msix_interrupts    At least equal to the number of queue pairs desired
interrupt_pin      Interrupt PIN to use if MSI and MSI-X are not supported
================   ===========================================================

The NVMe EPF specific configurable fields are in the nvme subdirectory of the
directory created in 1

================   ===========================================================
ctrl_opts          NVMe target connection parameters
dma_enable         Enable (1) or disable (0) DMA transfers; default = 1
mdts_kb            Maximum data transfer size in KiB; default = 128
================   ===========================================================
