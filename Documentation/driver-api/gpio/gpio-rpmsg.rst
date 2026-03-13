.. SPDX-License-Identifier: GPL-2.0-or-later

GPIO RPMSG (Remote Processor Messaging) Protocol
================================================

The GPIO RPMSG transport protocol is used for communication and interaction
with GPIO controllers on remote processors via the RPMSG bus.

Message Format
--------------

The RPMSG message consists of a 6-byte packet with the following layout:

.. code-block:: none

   +-----+-----+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05|
   |type |cmd  |port |line |  data    |
   +-----+-----+-----+-----+-----+----+

- **type (Message Type)**: The message type can be one of:

  - 0: GPIO_RPMSG_SEND
  - 1: GPIO_RPMSG_REPLY
  - 2: GPIO_RPMSG_NOTIFY

- **cmd**: Command code, used for GPIO_RPMSG_SEND messages.

- **port**: The GPIO port (bank) index.

- **line**: The GPIO line (pin) index of the port.

- **data**: See details in the command description below.

- **reply err**: Error code from the remote core.

  - 0: Success
  - 1: General error (Early remote software only returns this unclassified error)
  - 2: Not supported (A command is not supported by the remote firmware)
  - 3: Resource not available (The resource is not allocated to Linux)
  - 4: Resource busy (The resource is already in use)
  - 5: Parameter error


GPIO Commands
-------------

Commands are specified in the **Cmd** field for **GPIO_RPMSG_SEND** (Type=0) messages.

The SEND message is always sent from Linux to the remote firmware. Each
SEND corresponds to a single REPLY message. The GPIO driver should
serialize messages and determine whether a REPLY message is required. If a
REPLY message is expected but not received within the specified timeout
period (currently 1 second in the Linux driver), the driver should return
-ETIMEOUT.

GET_DIRECTION (Cmd=2)
~~~~~~~~~~~~~~~~~~~~~

**Request:**

.. code-block:: none

   +-----+-----+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05|
   | 0   | 2   |port |line | 0   | 0  |
   +-----+-----+-----+-----+-----+----+

**Reply:**

.. code-block:: none

   +-----+-----+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05|
   | 1   | 2   |port |line | err | dir|
   +-----+-----+-----+-----+-----+----+

- **err**: See above for definitions.

- **dir**: Direction.

  - 0: None
  - 1: Output
  - 2: Input

SET_DIRECTION (Cmd=3)
~~~~~~~~~~~~~~~~~~~~~

**Request:**

.. code-block:: none

   +-----+-----+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05|
   | 0   | 3   |port |line | dir | 0  |
   +-----+-----+-----+-----+-----+----+

- **dir**: Direction.

  - 0: None
  - 1: Output
  - 2: Input

**Reply:**

.. code-block:: none

   +-----+-----+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05|
   | 1   | 3   |port |line | err | 0  |
   +-----+-----+-----+-----+-----+----+

- **err**: See above for definitions.


GET_VALUE (Cmd=4)
~~~~~~~~~~~~~~~~~

**Request:**

.. code-block:: none

   +-----+-----+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05|
   | 0   | 4   |port |line | 0   | 0  |
   +-----+-----+-----+-----+-----+----+

**Reply:**

.. code-block:: none

   +-----+-----+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05|
   | 1   | 4   |port |line | err | val|
   +-----+-----+-----+-----+-----+----+

- **err**: See above for definitions.

- **val**: Line level.

  - 0: Low
  - 1: High

SET_VALUE (Cmd=5)
~~~~~~~~~~~~~~~~~

**Request:**

.. code-block:: none

   +-----+-----+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05|
   | 0   | 5   |port |line | val | 0  |
   +-----+-----+-----+-----+-----+----+

- **val**: Output level.

  - 0: Low
  - 1: High

**Reply:**

.. code-block:: none

   +-----+-----+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05|
   | 1   | 5   |port |line | err | 0  |
   +-----+-----+-----+-----+-----+----+

- **err**: See above for definitions.

SET_IRQ_TYPE (Cmd=6)
~~~~~~~~~~~~~~~~~~~~

**Request:**

.. code-block:: none

   +-----+-----+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05|
   | 0   | 6   |port |line | val | wk |
   +-----+-----+-----+-----+-----+----+

- **val**: IRQ types.

  - 0: Interrupt disabled
  - 1: Rising edge trigger
  - 2: Falling edge trigger
  - 3: Both edge trigger
  - 4: High level trigger
  - 8: Low level trigger

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

   +-----+-----+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05|
   | 1   | 6   |port |line | err | 0  |
   +-----+-----+-----+-----+-----+----+

- **err**: See above for definitions.

NOTIFY_REPLY (Cmd=10)
~~~~~~~~~~~~~~~~~~~~~
The reply message for the notification is optional. The remote firmware can
implement it to simulate the interrupt acknowledgment behavior.

**Request:**

.. code-block:: none

   +-----+-----+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05|
   | 0   | 10  |port |line |level| 0  |
   +-----+-----+-----+-----+-----+----+

- **port**: The GPIO port (bank) index.

- **line**: The GPIO line (pin) index of the port.

- **level**: GPIO line status.

Notification Message
--------------------

Notifications are sent by the remote core and they have
**Type=2 (GPIO_RPMSG_NOTIFY)**:

When a GPIO line asserts an interrupt on the remote processor, the firmware
should immediately mask the corresponding interrupt source and send a
notification message to the Linux. Upon completion of the interrupt
handling on the Linux side, the driver should issue a
command **SET_IRQ_TYPE** to the firmware to unmask the interrupt.

A Notification message can arrive between a SEND and its REPLY message,
and the driver is expected to handle this scenario.

.. code-block:: none

   +-----+-----+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05|
   | 2   | 0   |port |line |type | 0  |
   +-----+-----+-----+-----+-----+----+

- **port**: The GPIO port (bank) index.

- **line**: The GPIO line (pin) index of the port.

- **type**: Optional parameter to indicate the trigger event type.

