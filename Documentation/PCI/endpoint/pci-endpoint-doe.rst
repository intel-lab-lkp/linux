.. SPDX-License-Identifier: GPL-2.0-only or MIT

.. include:: <isonum.txt>

=============================================
Data Object Exchange (DOE) for PCIe Endpoint
=============================================

:Copyright: |copy| 2026 Texas Instruments Incorporated
:Author: Aksh Garg <a-garg7@ti.com>
:Co-Author: Siddharth Vadapalli <s-vadapalli@ti.com>

Overview
========

DOE (Data Object Exchange) is a standard PCIe extended capability feature
introduced in the Data Object Exchange (DOE) ECN for PCIe r5.0. It is an optional
mechanism for system firmware/software running on root complex (host) to perform
:ref:`data object <data-object-term>` exchanges with an endpoint function. Each
data object is uniquely identified by the Vendor ID of the vendor publishing the
data object definition and a Data Object Type value assigned by that vendor.

Think of DOE as a sophisticated mailbox system built into PCIe. The root complex
can send structured requests to the endpoint device through DOE mailboxes, and
the endpoint device responds with appropriate data. DOE mailboxes are implemented
as PCIe Extended Capabilities in endpoint devices, allowing multiple mailboxes
per function, each potentially supporting different data object protocols.

The DOE support for root complex devices has already been implemented in
``drivers/pci/doe.c``.

How DOE Works
=============

The DOE mailbox operates through a simple request-response model:

1. **Host sends request**: The root complex writes a data object (vendor ID, type,
   and payload) to the DOE write mailbox register (one DWORD at a time) of the
   endpoint function's config space and sets the GO bit in the DOE Status register
   to indicate that a request is ready for processing.
2. **Endpoint processes**: The endpoint function reads the request from DOE write
   mailbox register, sets the BUSY bit in the DOE Status register, identifies the
   protocol of the data object, and executes the appropriate handler.
3. **Endpoint responds**: The endpoint function writes the response data object to the
   DOE read mailbox register (one DWORD at a time), and sets the READY bit in the DOE
   Status register to indicate that the response is ready. If an error occurs during
   request processing (such as unsupported protocol or handler failure), the endpoint
   sets the ERROR bit in the DOE Status register instead of the READY bit.
4. **Host reads response**: The root complex retrieves the response data from the DOE read
   mailbox register once the READY bit is set in the DOE Status register, and then writes
   any value to this register to indicate a successful read. If the ERROR bit was set,
   the root complex discards the response and performs error handling as needed.

Each mailbox operates independently and can handle one transaction at a time. The
DOE specification supports data objects of size up to 256KB (2\ :sup:`18` dwords).

For complete DOE capability details, refer to `PCI Express Base Specification Revision 7.0,
Section 6.30 - Data Object Exchange (DOE)`.

Key Terminologies
=================

.. _data-object-term:

**Data Object**
  A structured, vendor-defined, or standard-defined message exchanged between
  root complex and endpoint function via DOE capability registers in configuration
  space of the function.

**Mailbox**
  A DOE capability on the endpoint device, where each physical function can have
  multiple mailboxes.

**Protocol**
  A specific type of DOE communication data object identified by a Vendor ID and Type.

**Handler**
  A function that processes DOE requests of a specific protocol and generates responses.

Architecture of DOE Implementation for Endpoint
===============================================

.. code-block:: text

       +------------------+
       |                  |
       |   Root Complex   |
       |                  |
       +--------^---------+
                |
                | Config space access
                |   over PCIe link
                |
     +----------v-----------+
     |                      |
     |    PCIe Controller   |
     |      as Endpoint     |
     |                      |
     |  +-----------------+ |
     |  |   DOE Mailbox   | |
     |  +-------^---------+ |
     +----------|-----------+
    +-----------|---------------------------------------------------------------+
    |           |                                       +--------------------+  |
    | +---------v--------+           Allocate           |  +--------------+  |  |
    | |                  |-------------------------------->|   Request    |  |  |
    | |   EP Controller  |                            +--->|    Buffer    |  |  |
    | |      Driver      |             Free           | |  +--------------+  |  |
    | |                  |--------------------------+ | |                    |  |
    | +--------^---------+                          | | |                    |  |
    |          |                                    | | |                    |  |
    |          |                                    | | |                    |  |
    |          | pci_ep_doe_process_request()       | | |                    |  |
    |          |                                    | | |                    |  |
    | +--------v---------+             Free         | | |                    |  |
    | |                  |----------------------------+ |         DDR        |  |
    | |    DOE EP Core   |<----+                    |   |                    |  |
    | |    (doe-ep.c)    |     |     Discovery      |   |                    |  |
    | |                  |-----+  Protocol Handler  |   |                    |  |
    | +--------^---------+                          |   |                    |  |
    |          |                                    |   |                    |  |
    |          | protocol_handler()                 |   |                    |  |
    |          |                                    |   |                    |  |
    | +--------v---------+                          |   |                    |  |
    | |                  |                          |   |  +--------------+  |  |
    | | Protocol Handler |                          +----->|   Response   |  |  |
    | |      Module      |-------------------------------->|    Buffer    |  |  |
    | | (CMA/SPDM/Other) |           Allocate           |  +--------------+  |  |
    | |                  |                              |                    |  |
    | +------------------+                              |                    |  |
    |                                                   +--------------------+  |
    +---------------------------------------------------------------------------+

Initialization and Cleanup
--------------------------

**Framework Initialization and DOE Setup**

The EPC core provides the ``pci_epc_doe_setup(epc)`` API for centralized DOE
mailbox discovery and registration. The controller driver calls this API during
its probe sequence if DOE is supported.

This API performs the following steps:

1. Calls ``pci_ep_doe_init(epc)``, which initializes the xarray data structure
   (a resizable array data structure defined in linux) named ``doe_mbs`` that
   stores metadata of DOE mailboxes for the controller in ``struct pci_epc``.
2. Discovers all DOE capabilities in the endpoint function's configuration space
   for each function. For each discovered DOE capability, calls
   ``pci_ep_doe_add_mailbox(epc, func_no, cap_offset)`` to register the mailbox.

Each DOE mailbox structure created by ``pci_ep_doe_add_mailbox()`` gets an
ordered workqueue allocated for processing DOE requests sequentially for that
mailbox, enabling concurrent request handling across different mailboxes. Each
mailbox is uniquely identified by the combination of physical function number
and capability offset for that controller.

**Cleanup**

The EPC core provides the ``pci_epc_doe_destroy(epc)`` API for centralized DOE
cleanup. The controller driver calls this API during its remove sequence
if DOE is supported.

This API calls ``pci_ep_doe_destroy(epc)``, which destroys all registered
mailboxes, cancels any pending tasks, flushes and destroys the workqueues,
and frees all memory allocated to the mailboxes.

Protocol Handler Support
------------------------

Protocol implementations (such as CMA, SPDM, or vendor-specific protocols) are
supported through a static array of protocol handlers.

When a new DOE protocol library is introduced, its handler function is added to
the static ``pci_doe_protocols`` array in ``drivers/pci/endpoint/pci-ep-doe.c``.
The discovery protocol (VID = 0x0001 (PCI-SIG vendor ID), Type = 0x00 (discovery
protocol)) is included in this static array and handled internally by the
DOE EP core.

Request Handling
----------------

The complete flow of a DOE request from the root complex to the response:

**Step 1: Root Complex → EP Controller Driver**

The root complex writes a DOE request (Vendor ID, Type, and Payload) to the
DOE write mailbox register in the endpoint function's configuration space and sets
the GO bit in the DOE Control register, indicating that the request is ready for
processing.

**Step 2: EP Controller Driver → DOE EP Core**

The controller driver reads the request header to determine the data object
length. Based on this length field, it allocates a request buffer in memory
(DDR) of the appropriate size. The driver then reads the complete request
payload from the DOE write mailbox register and converts the data from
little-endian format (the format followed in the PCIe transactions over the
link) to CPU-native format using ``le32_to_cpu()``. The driver defines a
completion callback function with signature ``void (*complete)(u8 func_no,
u16 cap_offset, int status, u16 vendor, u8 type, void *response_pl,
size_t response_pl_sz)`` to be invoked when the request processing completes.
The driver then calls ``pci_ep_doe_process_request(epc, func_no, cap_offset,
vendor, type, request, request_sz, complete)`` to hand off the request to the
DOE EP core. This function returns immediately after queuing the work
(without blocking), and the driver sets the BUSY bit in the DOE Status register.

**Step 3: DOE EP Core Processing**

The DOE EP core creates a task structure and submits it to the mailbox's ordered
workqueue. This ensures that requests for each mailbox are processed
sequentially, one at a time, as required by the DOE specification. It looks up
the protocol handler based on the Vendor ID and Type from the request header,
and executes the handler function.

**Step 4: Protocol Handler Execution**

The workqueue executes the task by calling the registered protocol handler:
``handler(request, request_sz, &response, &response_sz)``. The handler processes
the request, allocates a response buffer in memory (DDR), builds the response
data, and returns the response pointer and size. For the discovery protocol,
the DOE EP core handles this directly without invoking an external handler.

**Step 5: DOE EP Core → EP Controller Driver**

After the protocol handler completes, the DOE EP core frees the request buffer,
and invokes the completion callback provided by the controller driver asynchronously.
The callback receives the function number, capability offset (to identify the mailbox),
status code indicating the result of request processing, vendor ID and type of the data
object, the response buffer, and its size.

**Step 6: EP Controller Driver → Root Complex**

The controller driver converts the response from CPU-native format to
little-endian format using ``cpu_to_le32()``, writes the response to DOE read
mailbox register, and sets the READY bit in the DOE Status register. The root
complex then reads the response from the read mailbox register. Finally, the controller
driver frees the response buffer (which the handler allocated).

Asynchronous Request Processing
-------------------------------

The DOE-EP framework implements asynchronous request processing because an
endpoint function can have multiple instances of DOE mailboxes, and requests may
be interleaved across these mailboxes. Request processing of one mailbox should
not result in blocking request processing of other mailboxes. Hence, requests
on each mailbox need to be handled in parallel for optimization.

For the EP controller driver to handle requests on multiple mailboxes in
parallel, ``pci_ep_doe_process_request()`` must be asynchronous. The function
returns immediately after submitting the request to the mailbox's workqueue,
without waiting for the request to complete. A completion callback provided by
the controller driver is invoked asynchronously when request processing
finishes. This asynchronous design enables concurrent processing of requests
across different mailboxes.

Abort Handling
--------------

The DOE specification allows the root complex to abort ongoing DOE operations
by setting the ABORT bit in the DOE Control register.

**Trigger**

When the root complex sets the ABORT bit, the EP controller driver detects this
condition (typically in an interrupt handler or register polling routine). The
action taken depends on the timing of the abort:

- **ABORT during request transfer**: If the ABORT bit is set while the root complex
  is still transferring the request to the mailbox registers, the controller driver
  discards the request and no call to ``pci_ep_doe_abort()`` is needed.

- **ABORT after request submission**: If the ABORT bit is set after the request
  has been fully received and submitted to the DOE EP core via
  ``pci_ep_doe_process_request()``, the controller driver must call
  ``pci_ep_doe_abort(epc, func_no, cap_offset)`` for the affected mailbox to
  perform abort sequence in the DOE EP core.

**Abort Sequence**

The abort function performs the following actions:

1. Sets the CANCEL flag on the mailbox to prevent queued requests from starting
2. Flushes the workqueue to wait for any currently executing handler to complete
   (handlers cannot be interrupted mid-execution)
3. Clears the CANCEL flag to allow the mailbox to accept new requests

Queued requests that have not started execution will be aborted with an error
status. The currently executing request will complete normally, and the controller
will reject the response if it arrives after the abort sequence has been triggered.

.. note::
   Independent of when the ABORT bit is triggered, the controller driver must
   clear the ERROR, BUSY, and READY bits in the DOE Status register after
   completing the abort operation to reset the mailbox to an idle state.

Error Handling
--------------

Errors can occur during DOE request processing for various reasons, such as
unsupported protocols, handler failures, or memory allocation failures.

**Error Detection**

When an error occurs during DOE request processing, the DOE EP core propagates this error
back to the controller driver either through the ``pci_ep_doe_process_request()`` return value,
or the status code passed to the completion callback.

**Error Response**

When the controller driver receives an error code, it sets the ERROR bit in the DOE Status
register instead of writing a response to the read mailbox register, and frees the buffers.

API Reference
=============

.. kernel-doc:: drivers/pci/endpoint/pci-ep-doe.c
   :export:
