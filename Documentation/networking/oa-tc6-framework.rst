.. SPDX-License-Identifier: GPL-2.0+
.. include:: <isonum.txt>

=========================================================================
OPEN Alliance 10BASE-T1x MAC-PHY Serial Interface (TC6) Framework Support
=========================================================================

:Copyright: |copy| 2023 MICROCHIP

Introduction
------------

The IEEE 802.3cg project defines two 10 Mbit/s PHYs operating over a
single pair of conductors. The 10BASE-T1L (Clause 146) is a long reach
PHY supporting full duplex point-to-point operation over 1 km of single
balanced pair of conductors. The 10BASE-T1S (Clause 147) is a short reach
PHY supporting full / half duplex point-to-point operation over 15 m of
single balanced pair of conductors, or half duplex multidrop bus
operation over 25 m of single balanced pair of conductors.

Furthermore, the IEEE 802.3cg project defines the new Physical Layer
Collision Avoidance (PLCA) Reconciliation Sublayer (Clause 148) meant to
provide improved determinism to the CSMA/CD media access method. PLCA
works in conjunction with the 10BASE-T1S PHY operating in multidrop mode.

The aforementioned PHYs are intended to cover the low-speed / low-cost
applications in industrial and automotive environment. The large number
of pins (16) required by the MII interface, which is specified by the
IEEE 802.3 in Clause 22, is one of the major cost factors that need to be
addressed to fulfil this objective.

The MAC-PHY solution integrates an IEEE Clause 4 MAC and a 10BASE-T1x PHY
exposing a low pin count Serial Peripheral Interface (SPI) to the host
microcontroller. This also enables the addition of Ethernet functionality
to existing low-end microcontrollers which do not integrate a MAC
controller.

Overview
--------

The MAC-PHY is specified to carry both data (Ethernet frames) and control
(register access) transactions over a single full-duplex serial
peripheral interface.

Protocol Overview
-----------------

Two types of transactions are defined in the protocol: data transactions
for Ethernet frame transfers and control transactions for register
read/write transfers. A chunk is the basic element of data transactions
and is composed of 4 bytes of overhead plus the configured payload size
for each chunk. Ethernet frames are transferred over one or more data
chunks. Control transactions consist of one or more register read/write
control commands.

SPI transactions are initiated by the SPI host with the assertion of CSn
low to the MAC-PHY and ends with the deassertion of CSn high. In between
each SPI transaction, the SPI host may need time for additional
processing and to setup the next SPI data or control transaction.

SPI data transactions consist of an equal number of transmit (TX) and
receive (RX) chunks. Chunks in both transmit and receive directions may
or may not contain valid frame data independent from each other, allowing
for the simultaneous transmission and reception of different length
frames.

Each transmit data chunk begins with a 32-bit data header followed by a
data chunk payload on MOSI. The data header indicates whether transmit
frame data is present and provides the information to determine which
bytes of the payload contain valid frame data.

In parallel, receive data chunks are received on MISO. Each receive data
chunk consists of a data chunk payload ending with a 32-bit data footer.
The data footer indicates if there is receive frame data present within
the payload or not and provides the information to determine which bytes
of the payload contain valid frame data.

Reference
---------

10BASE-T1x MAC-PHY Serial Interface Specification,

Link: https://www.opensig.org/about/specifications/

Hardware Architecture
---------------------

.. code-block:: none

                    +-------------------------------------+
                    |                MAC-PHY              |
  +----------+      | +-----------+  +-------+  +-------+ |
  | SPI Host |<---->| | SPI Slave |  |  MAC  |  |  PHY  | |
  +----------+      | +-----------+  +-------+  +-------+ |
                    +-------------------------------------+

Software Architecture
---------------------

.. code-block:: none

  +----------------------------------------------------------+
  |                 Networking Subsystem                     |
  +----------------------------------------------------------+
             |                               |
             |                               |
  +----------------------+     +-----------------------------+
  |     MAC Driver       |<--->| OPEN Alliance TC6 Framework |
  +----------------------+     +-----------------------------+
             |                               |
             |                               |
  +----------------------+     +-----------------------------+
  |      PHYLIB          |     |       SPI Subsystem         |
  +----------------------+     +-----------------------------+
                                             |
                                             |
  +----------------------------------------------------------+
  |                10BASE-T1x MAC-PHY Device                 |
  +----------------------------------------------------------+

Implementation
--------------

MAC Driver
~~~~~~~~~~
- Initializes and configures the OA TC6 framework for the MAC-PHY.

- Initializes PHYLIB interface.

- Registers and configures the network device.

- Sends the tx ethernet frame from n/w subsystem to OA TC6 framework.

OPEN Alliance TC6 Framework
~~~~~~~~~~~~~~~~~~~~~~~~~~~
- Registers macphy interrupt which is used to indicate receive data
  available, any communication errors and tx credit count availability in
  case it was 0 already.

- Prepares SPI chunks from the tx ethernet frame enqueued by the MAC
  driver and sends to MAC-PHY.

- Receives SPI chunks from MAC-PHY and prepares ethernet frame and sends
  to n/w subsystem.

- Prepares and performs control read/write commands.

Data Transaction
~~~~~~~~~~~~~~~~

Tx ethernet frame from the n/w layer is divided into multiple chunks in
the oa_tc6_prepare_tx_chunks() function. Each tx chunk will have 4 bytes
header and 64/32 bytes chunk payload.

.. code-block:: none

  +---------------------------------------------------+
  |                     Tx Chunk                      |
  | +---------------------------+  +----------------+ |   MOSI
  | | 64/32 bytes chunk payload |  | 4 bytes header | |------------>
  | +---------------------------+  +----------------+ |
  +---------------------------------------------------+

The number of buffers available in the MAC-PHY to store the incoming tx
chunk payloads is represented as tx credit count (txc). This txc can be
read either from the Buffer Status Register or footer (Refer below in the
rx case for the footer info) received from the MAC-PHY. The number of txc
is needed to transport the ethernet frame is calculated from the size of
the ethernet frame. The header in the each chunk is updated with the
chunk payload details like data not control, data valid, start valid,
start word offset, end byte offset, end valid and parity bit.

Once the tx chunks are ready, oa_tc6_handler() task is triggered to
perform SPI transfer. This task checks for the txc availability in the
MAC-PHY and sends the number of chunks equal to the txc. If there is no
txc is available then the task waits for the interrupt to indicate the
txc availability. If the available txc is less than the needed txc then
the SPI transfer is performed for the available txc and the rx chunks
footer is processed for the txc availability again. For every SPI
transfer the received rx chunks will be processed for the rx ethernet
frame (if any), txc and rca.

Rx ethernet frame from the MAC-PHY is framed from the rx chunks received
from the MAC-PHY and will be transferred to the n/w layer. Each rx chunk
will have 64/32 bytes chunk payload and 4 bytes footer.

.. code-block:: none

  +---------------------------------------------------+
  |                     Rx Chunk                      |
  | +----------------+  +---------------------------+ |   MISO
  | | 4 bytes footer |  | 64/32 bytes chunk payload | |------------>
  | +----------------+  +---------------------------+ |
  +---------------------------------------------------+

In case of interrupt, the oa_tc6_handler() task performs an empty chunk
SPI transfer to get the reason for the interrupt in the received chunk
footer. The reason can be receive chunk available (rca) or extended block
status (exst) or txc availability. Based on this the corresponding
operation is performed. If it is for rca then the SPI transfer is
performed with the empty chunks equal to the rca to get the rx chunks.
Rx ethernet frame is framed from the rx chunks received and transferred
to n/w layer. If it is for exst then the STATUS0 register will be read
for the error detail.

In the beginning the interrupt occurs for indicating the reset complete
from the MAC-PHY and is ready for the configuration. oa_tc6_handler() task
handles this interrupt to get and clear the reset complete status.

Control Transaction
~~~~~~~~~~~~~~~~~~~

Control transactions are performed to read/write the registers in the
MAC-PHY. Each control command headers are 4 bytes length with the
necessary details to read/write the registers.

In case of a register write command, the write command header has the
information like data not control, write not read, address increment
disable, memory map selector, address, length and parity followed by the
data to be written. If the protected mode is enabled in the CONFIG0
register then each data to be written will be sent first followed by its
ones' complement value to ensure the error free transfer. For every write
command, both the write command header and the data will be echoed back in
the rx path to confirm the error free transaction.

In case of a register read command, the read command is preferred with the
above mentioned details and the echoed rx data will be processed for the
register data. In case of protected mode enabled the echoed rx data
contains the ones' complemented data also for verifying the data error.

oa_tc6_perform_ctrl() function prepares control commands based on the
read/write request, performs SPI transfer, checks for the error and
returns read register data in case of control read command.
