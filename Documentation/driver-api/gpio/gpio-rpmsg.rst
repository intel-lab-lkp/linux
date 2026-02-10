.. SPDX-License-Identifier: GPL-2.0-or-later

GPIO RPMSG Protocol
===================

The GPIO RPMSG transport protocol is used for communication and interaction
with GPIO controllers located on remote cores on the RPMSG bus.

Message Format
--------------

The RPMSG message consists of a 14-byte packet with the following layout:

.. code-block:: none

   +-----+-------+--------+-----+-----+------------+-----+-----+-----+----+
   |0x00 |0x01   |0x02    |0x03 |0x04 |0x05..0x09  |0x0A |0x0B |0x0C |0x0D|
   | ID  |vendor |version |type |cmd  |reserved[5] |line |port |  data    |
   +-----+-------+--------+-----+-----+------------+-----+-----+-----+----+

- **ID (Message Identification Code)**: Must be 0x5. Indicates the GPIO message.

- **Vendor**: Vendor ID number.
  - 0: Reserved
  - 1: NXP

- **Version**: Vendor-specific version number (such as software release).

- **Type (Message Type)**: The message type can be one of:

  - 0: GPIO_RPMSG_SETUP
  - 1: GPIO_RPMSG_REPLY
  - 2: GPIO_RPMSG_NOTIFY

- **Cmd**: Command code, used for GPIO_RPMSG_SETUP messages.

- **reserved[5]**: Reserved bytes. Should always be 0.

- **line**: The GPIO line(pin) index of the port.

- **port**: The GPIO port(bank) index.

- **data**: See details in the command description below.

GPIO Commands
-------------

Commands are specified in the **Cmd** field for **GPIO_RPMSG_SETUP** (Type=0) messages.

The SETUP message is always sent from Linux to the remote firmware. Each
SETUP corresponds to a single REPLY message. The GPIO driver should
serialize messages and determine whether a REPLY message is required. If a
REPLY message is expected but not received within the specified timeout
period (currently 1 second in the Linux driver), the driver should return
-ETIMEOUT.

GPIO_RPMSG_INPUT_INIT (Cmd=0)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Request:**

.. code-block:: none

   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05..0x09 |0x0A |0x0B |0x0C |0x0D|
   | 5   | 1   | 0   | 0   | 0   |  0        |line |port | val | wk |
   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+----+

- **val**: Interrupt trigger type.

  - 0: Interrupt disabled
  - 1: Rising edge trigger
  - 2: Falling edge trigger
  - 3: Both edge trigger
  - 4: Low level trigger
  - 5: High level trigger

- **wk**: Wakeup enable.

  The remote system should always aim to stay in a power-efficient state by
  shutting down or clock-gating the GPIO blocks that aren't in use. Since
  the remoteproc driver is responsible for managing the power states of the
  remote firmware, the GPIO driver does not require to know the firmware's
  running states.

  When the wakeup bit is set, the remote firmware should configure the line
  as a wakeup source. The firmware should send the notification message to
  Linux after it is woken from the GPIO line.

  - 0: Disable wakeup from GPIO
  - 1: Enable wakeup from GPIO

**Reply:**

.. code-block:: none

   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05..0x09 |0x0A |0x0B |0x0C |0x0D|
   | 5   | 1   | 0   | 1   | 1   |  0        |line |port | err | 0  |
   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+----+

- **err**: Error code from the remote core.

  - 0: Success
  - 1: General error (Early remote software only returns this unclassified error)
  - 2: Not supported (A command is not supported by the remote firmware)
  - 3: Resource not available (The resource is not allocated to Linux)
  - 4: Resource busy (The resource is already in use)
  - 5: Parameter error

GPIO_RPMSG_OUTPUT_INIT (Cmd=1)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Request:**

.. code-block:: none

   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05..0x09 |0x0A |0x0B |0x0C |0x0D|
   | 5   | 1   | 0   | 0   | 1   |  0        |line |port | val | 0  |
   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+----+

- **val**: Output level.

  - 0: Low
  - 1: High

**Reply:**

.. code-block:: none

   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05..0x09 |0x0A |0x0B |0x0C |0x0D|
   | 5   | 1   | 0   | 1   | 1   |  0        |line |port | err | 0  |
   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+----+

- **err**: See above for definitions.

GPIO_RPMSG_INPUT_GET (Cmd=2)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Request:**

.. code-block:: none

   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05..0x09 |0x0A |0x0B |0x0C |0x0D|
   | 5   | 1   | 0   | 0   | 2   |  0        |line |port | 0   | 0  |
   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+----+

**Reply:**

.. code-block:: none

   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+-----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05..0x09 |0x0A |0x0B |0x0C |0x0D |
   | 5   | 1   | 0   | 1   | 2   |  0        |line |port | err |level|
   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+-----+

- **err**: See above for definitions.

- **level**: Input level.

  - 0: Low
  - 1: High

GPIO_RPMSG_GET_DIRECTION (Cmd=3)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Request:**

.. code-block:: none

   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05..0x09 |0x0A |0x0B |0x0C |0x0D|
   | 5   | 1   | 0   | 0   | 3   |  0        |line |port | 0   | 0  |
   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+----+

**Reply:**

.. code-block:: none

   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+-----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05..0x09 |0x0A |0x0B |0x0C |0x0D |
   | 5   | 1   | 0   | 1   | 3   |  0        |line |port | err | dir |
   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+-----+

- **err**: See above for definitions.

- **dir**: Direction.

  - 0: Output
  - 1: Input

GPIO_RPMSG_NOTIFY_REPLY (Cmd=4)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
The reply message for the notification is optional. The remote firmware can
implement it to simulate the interrupt acknowledgment behavior.

**Request:**

.. code-block:: none

   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05..0x09 |0x0A |0x0B |0x0C |0x0D|
   | 5   | 1   | 0   | 0   | 4   |  0        |line |port |level| 0  |
   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+----+

- **line**: The GPIO line(pin) index of the port.
- **port**: The GPIO port(bank) index.

Notification Message
--------------------

Notifications are sent with **Type=2 (GPIO_RPMSG_NOTIFY)**:

When a GPIO line asserts an interrupt on the remote processor, the firmware
should immediately mask the corresponding interrupt source and send a
notification message to the Linux. Upon completion of the interrupt
handling on the Linux side, the driver should issue a
**GPIO_RPMSG_INPUT_INIT** command to the firmware to unmask the interrupt.

A Notification message can arrive between a SETUP and its REPLY message,
and the driver is expected to handle this scenario.

.. code-block:: none

   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05..0x09 |0x0A |0x0B |0x0C |0x0D|
   | 5   | 1   | 0   | 2   | 0   |  0        |line |port |type | 0  |
   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+----+

- **line**: The GPIO line(pin) index of the port.
- **port**: The GPIO port(bank) index.
- **type**: Optional pamameter to indicate the trigger event type.

