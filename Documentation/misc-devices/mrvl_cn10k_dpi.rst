.. SPDX-License-Identifier: GPL-2.0

===============================================
Marvell CN10K DMA packet interface (DPI) driver
===============================================

Overview
========

DPI is a DMA packet interface hardware block in Marvell's CN10K silicon.
DPI hardware comprises a physical function (PF), its virtual functions,
mailbox logic, and a set of DMA engines & DMA command queues.

DPI PF function is an administrative function which services the mailbox
requests from its VF functions and provisions DMA engine resources to
it's VF functions.

mrvl_cn10k_dpi.ko misc driver loads on DPI PF device and services the
mailbox commands submitted by the VF devices and accordingly initializes
the DMA engines and VF device's DMA command queues. Also, driver creates
/dev/mrvl-cn10k-dpi node to set DMA engine and PEM (PCIe interface) port
attributes like fifo length, molr, mps & mrrs.

DPI PF driver is just an administrative driver to setup its VF device's
queues and provisions the hardware resources, it can not initiate any
DMA operations. Only VF devices are provisioned with DMA capabilities.

Driver location
===============

drivers/misc/mrvl_cn10k_dpi.c

Driver IOCTLs
=============

:c:macro::`DPI_MPS_MRRS_CFG`
ioctl that sets max payload size & max read request size parameters of
a pem port to which DMA engines are wired.


:c:macro::`DPI_ENGINE_CFG`
ioctl that sets DMA engine's fifo sizes & max outstanding load request
thresholds.

Userspace code example
----------------------

DPI VF devices are managed by user space drivers, below is a reference
code to the user space driver's mailbox command exchange with DPI PF
driver through hardware mailbox.

https://github.com/VamsiKrishnaA99/dpi-dma/blob/main/driver/roc_dpi.c

Below is a sample application that uses driver IOCTLs to setup DMA engine
and PEM port attributes over `/dev/mrvl-cn10k-dpi` node.

https://github.com/VamsiKrishnaA99/dpi-dma/blob/main/application/main.c
