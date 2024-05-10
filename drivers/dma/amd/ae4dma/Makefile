# SPDX-License-Identifier: GPL-2.0
#
# AMD AE4DMA driver
#

obj-$(CONFIG_AMD_AE4DMA) += ae4dma.o

ae4dma-objs := ae4dma-dev.o  ../ptdma/ptdma-dmaengine.o ../common/amd_dma.o ../ptdma/ptdma-debugfs.o

ae4dma-$(CONFIG_PCI) += ae4dma-pci.o
