.. SPDX-License-Identifier: GPL-2.0

================
fwctl bnxt driver
================

:Author: Pavan Chebbi

Overview
========

BNXT driver makes a fwctl service available through an auxiliary_device.
The bnxt_fwctl driver binds to this device and registers itself with the
fwctl subsystem.

The bnxt_fwctl driver is agnostic to the device firmware internals. It
uses the ULP conduit provided by bnxt to send requests (HWRM commands)
to firmware.

bnxt_fwctl User API
==================

Each RPC request contains a message request structure (HWRM input) its,
legth, optional request timeout, and dma buffers' information if the
command needs any DMA. The request is then put together with the request
data and sent through bnxt's message queue to the firmware, and the results
are returned to the caller.
