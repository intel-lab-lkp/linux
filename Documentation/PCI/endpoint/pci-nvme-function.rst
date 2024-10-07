.. SPDX-License-Identifier: GPL-2.0

=================
PCI NVMe Function
=================

:Author: Damien Le Moal <dlemoal@kernel.org>

The PCI NVMe endpoint function driver implements a PCIe NVMe controller for a
local NVMe fabrics host controller. The fabrics controller target can use any
of the transports supported by the NVMe driver. In practice, using small SBC
boards equipped with a PCI endpoint controller, loop targets to files or block
devices or TCP targets to remote NVMe devices can be easily used.

Overview
========

The NVMe endpoint function driver relies as most as possible on the NVMe
fabrics driver for executing NVMe commands received from the PCI RC host to
minimize NVMe command parsing. However, some admin commands must be modified to
satisfy PCI transport specifications constraints (e.g. queue management
commands support and the optional SGL support).

Capabilities
------------

The NVMe capabilities exposed to the PCI RC host through the BAR 0 registers
are almost identical to the capabilities of the NVMe fabrics controller, with
some exceptions:

1) NVMe-over-fabrics specifications mandate support for SGL. Howerver, this
   capability is not exposed as supported because the current NVMe endpoint
   driver code does not support SGL.

2) The NVMe endpoint function driver can expose a different MDTS (Maximum Data
   Transfer Size) than the fabrics controller used.

Maximum Number of Queue Pairs
-----------------------------

Upon binding of the NVMe endpoint function driver to the endpoint controller,
BAR 0 is allocated with enough space to accommodate up to
PCI_EPF_NVME_MAX_NR_QUEUES (16) queue pairs. This relatively low number is
necessary to avoid running out of memory windows for mapping PCI addresses to
local endpoint controller memory.

The number of memory windows necessary for operation is roughly at most:
1) One memory window for raising MSI/MSI-X interrupts
2) One memory window for command PRP and data transfers
3) One memory window for each submission queue
4) One memory window for each completion queue

Given the highly asynchronous nature of the NVMe endpoint function driver
operation, the memory windows needed as described above will generally not be
used simultaneously, but that may happen. So a safe maximum number of queue
pairs that can be supported is equal to the maximum number of memory windows of
the endpoint controller minus two and divided by two. E.g. for an endpoint PCI
controller with 32 outbound memory windows available, up to 10 queue pairs can
be safely operated without any risk of getting PCI space mapping errors due to
the lack of memory windows.

The NVMe endpoint function driver allows configuring the maximum number of
queue pairs through configfs.

Command Execution
=================

The NVMe endpoint function driver relies on several work items to process NVMe
commands issued by the PCI RC host.

Register Poll Work
------------------

The register poll work is a delayed work used to poll for changes to the
controller state register. This is used to detect operations initiated by the
PCI host such as enabling or enabling the NVMe controller. The register poll
work is scheduled every 5 ms.

Submission Queue Work
---------------------

Upon creation of submission queues, starting with the submission queue for
admin commands, a delayed work is created and scheduled for execution every
jiffy to poll for a submission queue doorbell to detect submission of commands
by the PCI host.

When changes to a submission queue work are detected by a submission queue
work, the work allocates a command structure to copy the NVMe command issued by
the PCI host and schedules processing of the command using the command work.

Command Processing Work
-----------------------

This per-NVMe command work is scheduled for execution when an NVMe command is
received from the host. This work will:

1) Does minimal parsing of the NVMe command to determine if the command has a
   data buffer. If it does, the PRP list for the command is retrieved to
   identify the PCI address ranges used for the command data buffer. This can
   lead to the command buffer being represented using several discontiguous
   memory fragments.  A local memory buffer is also allocated for local
   execution of the command using the fabrics controller.

2) If the command is a write command (DMA direction from host to device), data
   is transferred from the host to the local memory buffer of the command. This
   is handled in a loop to process all fragments of the command buffer as well
   as simultaneously handle PCI address mapping constraints of the PCI endpoint
   controller.

3) The command is then executed using the NVMe driver fabrics code. This blocks
   the command work until the command execution completes.

4) When the command completes, the command work schedules handling of the
   command response using the completion queue work.

Completion Queue Work
---------------------

This per-completion queue work is used to aggregate handling of responses to
completed commands in batches to avoid having to issue an IRQ for every
completed command. This work is sceduled every time a command completes and
does:

1) Post a command completion entry for all completed commands.

2) Update the completion queue doorbell.

3) Raise an IRQ to signal the host that commands have completed.

Configuration
=============

The NVMe endpoint function driver can be fully controlled using configfs, once
a NVMe fabrics target is also setup. The available configfs parameters are:

  ctrl_opts

        Fabrics controller connection arguments, as formatted for
        the nvme cli "connect" command.

  dma_enable

        Enable (default) or disable DMA data transfers.

  mdts_kb

        Change the maximum data transfer size (default: 128 KB).

See Documentation/PCI/endpoint/pci-nvme-howto.rst for a more detailed
description of these parameters and how to use them to configure an NVMe
endpoint function driver.
