.. SPDX-License-Identifier: GPL-2.0

===================================
RPMSG Based Virtual Ethernet Driver
===================================

Overview
========

The RPMSG Based Virtual Ethernet Driver provides a virtual Ethernet interface for
communication between a host processor and a remote processor using the RPMSG
framework. This driver enables Ethernet-like packet transmission and reception
over shared memory, facilitating inter-core communication in systems with
heterogeneous processors.

The driver is designed to work with the RPMSG framework, which is part of the
Linux Remote Processor (remoteproc) subsystem. It uses shared memory for data
exchange and supports features like multicast address management, dynamic MAC
address assignment, and efficient packet processing using NAPI.

This driver is generic and can be used by any vendor. Vendors can develop their
own firmware for the remote processor to make it compatible with this driver.
The firmware must adhere to the shared memory layout, RPMSG communication
protocol, and data exchange requirements described in this documentation.

Key Features
============

- Virtual Ethernet interface using RPMSG.
- Shared memory-based packet transmission and reception.
- Support for multicast address management.
- Dynamic MAC address assignment.
- NAPI (New API) support for efficient packet processing.
- State machine for managing interface states.
- Workqueue-based asynchronous operations.
- Support for notifications and responses from the remote processor.

Magic Number
============

A **magic number** is used in the shared memory layout to validate that the
memory region is correctly initialized and accessible by both the host and the
remote processor. This value is a unique constant (for example,
``0xABCDABCD``) that is written to specific locations (such as the head and
tail structures) in the shared memory by the firmware and checked by the Linux
driver during the handshake process.

Purpose of the Magic Number
---------------------------

- **Validation:** Ensures that the shared memory region has been properly set up
  and is not corrupted or uninitialized.
- **Synchronization:** Both the host and remote processor must agree on the
  magic number value, which helps detect mismatches in memory layout or protocol
  version.
- **Error Detection:** If the driver detects an incorrect magic number during
  initialization or runtime, it can abort the handshake and report an error,
  preventing undefined behavior.

Implementation Details
----------------------

- The magic number is defined as a macro in the driver source (e.g.,
  ``#define RPMSG_ETH_SHM_MAGIC_NUM 0xABCDABCD``).
- The firmware must write this value to the ``magic_num`` field of the head and
  tail structures in the shared memory region.
- During the handshake, the Linux driver reads these fields and compares them to
  the expected value. If any mismatch is detected, the driver will log an error
  and refuse to proceed.

Example Usage in Shared Memory
------------------------------

.. code-block:: text

      Shared Memory Layout:
      ---------------------------
      |   MAGIC_NUM (0xABCDABCD) |   <-- rpmsg_eth_shm_head
      |          HEAD            |
      ---------------------------
      |   MAGIC_NUM (0xABCDABCD) |   <-- rpmsg_eth_shm_tail
      |          TAIL            |
      ---------------------------

The magic number must be present in both the head and tail structures for the
handshake to succeed.

Firmware developers must ensure that the correct magic number is written to the
appropriate locations in shared memory before the Linux driver attempts to
initialize the interface.

Shared Memory Layout
====================

The RPMSG Based Virtual Ethernet Driver uses a shared memory region to exchange
data between the host and the remote processor. The shared memory is divided
into transmit and receive regions, each with its own `head` and `tail` pointers
to track the buffer state.

Shared Memory Parameters
------------------------

The following parameters are exchanged between the host and the firmware to
configure the shared memory layout:

1. **num_pkt_bufs**:

   - The total number of packet buffers available in the shared memory.
   - This determines the maximum number of packets that can be stored in the
     shared memory at any given time.

2. **buff_slot_size**:

   - The size of each buffer slot in the shared memory.
   - This includes space for the packet length, metadata, and the actual packet
     data.

3. **base_addr**:

   - The base address of the shared memory region.
   - This is the starting point for accessing the shared memory.

4. **tx_offset**:

   - The offset from the `base_addr` where the transmit buffers begin.
   - This is used by the host to write packets for transmission.

5. **rx_offset**:

   - The offset from the `base_addr` where the receive buffers begin.
   - This is used by the host to read packets received from the remote
     processor.

Shared Memory Structure
-----------------------

The shared memory layout is as follows:

.. code-block:: text

      Shared Memory Layout:
      ---------------------------
      |        MAGIC_NUM        |   rpmsg_eth_shm_head
      |          HEAD           |
      ---------------------------
      |        MAGIC_NUM        |
      |        PKT_1_LEN        |
      |          PKT_1          |
      ---------------------------
      |           ...           |
      ---------------------------
      |        MAGIC_NUM        |
      |          TAIL           |   rpmsg_eth_shm_tail
      ---------------------------

1. **MAGIC_NUM**:

   - A unique identifier used to validate the shared memory region.
   - Ensures that the memory region is correctly initialized and accessible.

2. **HEAD Pointer**:

   - Tracks the start of the buffer for packet transmission or reception.
   - Updated by the producer (host or remote processor) after writing a packet.

3. **TAIL Pointer**:

   - Tracks the end of the buffer for packet transmission or reception.
   - Updated by the consumer (host or remote processor) after reading a packet.

4. **Packet Buffers**:

   - Each packet buffer contains:

      - **Packet Length**: A 4-byte field indicating the size of the packet.
      - **Packet Data**: The actual Ethernet frame data.

5. **Buffer Size**:

   - Each buffer has a fixed size defined by `RPMSG_ETH_BUFFER_SIZE`, which
     includes space for the packet length and data.

Buffer Management
-----------------

- The host and remote processor use a circular buffer mechanism to manage the shared memory.
- The `head` and `tail` pointers are used to determine the number of packets available for processing:

   .. code-block:: c

         num_pkts = head - tail;
         num_pkts = num_pkts >= 0 ? num_pkts : (num_pkts + max_buffers);

- The producer writes packets to the buffer and increments the `head` pointer.
- The consumer reads packets from the buffer and increments the `tail` pointer.

RPMSG Communication
===================

The driver uses RPMSG channels to exchange control messages between the host and
the remote processor. These messages are used to manage the state of the
Ethernet interface, configure settings, notify events, and exchange runtime
information.

Information Exchanged Between RPMSG Channels
--------------------------------------------

1. **Requests from Host to Remote Processor**:

   - `RPMSG_ETH_REQ_SHM_INFO`: Request shared memory information, such as
     ``num_pkt_bufs``, ``buff_slot_size``, ``base_addr``, ``tx_offset``, and
     ``rx_offset``.
   - `RPMSG_ETH_REQ_SET_MAC_ADDR`: Set the MAC address of the Ethernet
     interface.
   - `RPMSG_ETH_REQ_ADD_MC_ADDR`: Add a multicast address to the remote
     processor's filter list.
   - `RPMSG_ETH_REQ_DEL_MC_ADDR`: Remove a multicast address from the remote
     processor's filter list.

2. **Responses from Remote Processor to Host**:

   - `RPMSG_ETH_RESP_SET_MAC_ADDR`: Acknowledge the MAC address configuration.
   - `RPMSG_ETH_RESP_ADD_MC_ADDR`: Acknowledge the addition of a multicast
     address.
   - `RPMSG_ETH_RESP_DEL_MC_ADDR`: Acknowledge the removal of a multicast
     address.
   - `RPMSG_ETH_RESP_SHM_INFO`: Respond with shared memory information such as
     ``num_pkt_bufs``, ``buff_slot_size``, ``base_addr``, ``tx_offset``, and
     ``rx_offset``.

3. **Notifications from Remote Processor to Host**:

   - `RPMSG_ETH_NOTIFY_PORT_UP`: Notify that the Ethernet port is up and ready
     for communication.
   - `RPMSG_ETH_NOTIFY_PORT_DOWN`: Notify that the Ethernet port is down.
   - `RPMSG_ETH_NOTIFY_PORT_READY`: Notify that the Ethernet port is ready for
     configuration.
   - `RPMSG_ETH_NOTIFY_REMOTE_READY`: Notify that the remote processor is ready
     for communication.

4. **Runtime Information Exchanged**:

   - **Link State**: Notifications about link state changes (e.g., link up or
     link down).
   - **Statistics**: Runtime statistics such as transmitted/received packets,
     errors, and dropped packets.
   - **Error Notifications**: Notifications about errors like buffer overflows
     or invalid packets.
   - **Configuration Updates**: Notifications about changes in configuration,
     such as updated MTU or VLAN settings.

How-To Guide for Vendors
========================

This section provides a guide for vendors to develop firmware for the remote
processor that is compatible with the RPMSG Based Virtual Ethernet Driver.

1. **Implement Shared Memory Layout**:

   - Allocate a shared memory region for packet transmission and reception.
   - Initialize the `MAGIC_NUM`, `num_pkt_bufs`, `buff_slot_size`, `base_addr`,
     `tx_offset`, and `rx_offset`.

2. **Magic Number Requirements**

   - The firmware must write a unique magic number (for example, ``0xABCDABCD``)
     to the `magic_num` field of both the head and tail structures in the shared
     memory region.
   - This magic number is used by the Linux driver to validate that the shared
     memory region is correctly initialized and accessible.
   - If the driver detects an incorrect magic number during the handshake, it
     will abort initialization and report an error.
   - Vendors must ensure the magic number matches the value expected by the
     Linux driver (see the `RPMSG_ETH_SHM_MAGIC_NUM` macro in the driver
     source).

3. **Handle RPMSG Requests**:

   - Implement handlers for the following RPMSG requests:

      - `RPMSG_ETH_REQ_SHM_INFO`
      - `RPMSG_ETH_REQ_SET_MAC_ADDR`
      - `RPMSG_ETH_REQ_ADD_MC_ADDR`
      - `RPMSG_ETH_REQ_DEL_MC_ADDR`

4. **Send RPMSG Notifications**:

   - Notify the host about the state of the Ethernet interface using the
     notifications described above.

5. **Send Runtime Information**:

   - Implement mechanisms to send runtime information such as link state
     changes, statistics, and error notifications.

6. **Implement Packet Processing**:

   - Process packets in the shared memory transmit and receive buffers.

7. **Test the Firmware**:

   - Use the RPMSG Based Virtual Ethernet Driver on the host to test packet
     transmission and reception.

Configuration
=============

The driver relies on the device tree for configuration. The shared memory region
is specified using the `virtual-eth-shm` node in the device tree.

Example Device Tree Node
------------------------

.. code-block:: dts

   virtual-eth-shm {
           compatible = "rpmsg,virtual-eth-shm";
           reg = <0x80000000 0x10000>; /* Base address and size of shared memory */
   };

Limitations
===========

- The driver assumes a specific shared memory layout and may not work with other
  configurations.
- Multicast address filtering is limited to the capabilities of the underlying
  RPMSG framework.
- The driver currently supports only one transmit and one receive queue.

References
==========

- RPMSG Framework Documentation: https://www.kernel.org/doc/html/latest/rpmsg.html
- Linux Networking Documentation: https://www.kernel.org/doc/html/latest/networking/index.html

Authors
=======

- MD Danish Anwar <danishanwar@ti.com>
