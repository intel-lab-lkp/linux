.. SPDX-License-Identifier: GPL-2.0-or-later

GPIO RPMSG (Remote Processor Messaging) Protocol
================================================

The GPIO RPMSG transport protocol is used for communication and interaction
with GPIO controllers on remote processors via the RPMSG bus.

Message Format
--------------

The out message to the remote consists of a 8-byte packet with the
following layout:

.. code-block:: none

   +------+------+------+------+------+------+------+------+
   | 0x00 | 0x01 | 0x02 | 0x03 | 0x04 | 0x05 | 0x06 | 0x07 |
   |     cmd     |    line     |           value           |
   +------+------+------+------+------+------+------+------+

- **cmd**: Little endian. Command type.

- **line**: Little endian. The GPIO line (pin) index.

- **value**: Little endian. See details in the command description below.


The in message from the remote has the following layout:

.. code-block:: none

   +------+--------+--------+
   | 0x00 |  0x01  |  0x02  |
   | type |     payload     |
   +------+--------+--------+

- **type**: Message types.
  - 0: Command reply messages.
  - 1: Interrupt messages.

- **payload**: See details below.

GPIO Commands
-------------

Commands are specified in the **Cmd** field.

The SEND message is always sent from Linux to the remote firmware. Each
SEND corresponds to a single REPLY message. The GPIO driver should
serialize messages and determine whether a REPLY message is required. If a
REPLY message is expected but not received within the specified timeout
period (currently 1 second in the Linux driver), the driver should return
-ETIMEDOUT.

GET_DIRECTION (Cmd=2)
~~~~~~~~~~~~~~~~~~~~~

**Request:**

.. code-block:: none

   +------+------+------+------+------+------+------+------+
   | 0x00 | 0x01 | 0x02 | 0x03 | 0x04 | 0x05 | 0x06 | 0x07 |
   |      2      |    line     |             0             |
   +------+------+------+------+------+------+------+------+

**Reply:**

.. code-block:: none

   +------+--------+--------+
   | 0x00 |  0x01  |  0x02  |
   |   0  | status | value  |
   +------+--------+--------+

- **status**:

  - 0: Ok
  - 1: Error

- **value**: Direction.

  - 0: None
  - 1: Output
  - 2: Input


SET_DIRECTION (Cmd=3)
~~~~~~~~~~~~~~~~~~~~~

**Request:**

.. code-block:: none

   +------+------+------+------+------+------+------+------+
   | 0x00 | 0x01 | 0x02 | 0x03 | 0x04 | 0x05 | 0x06 | 0x07 |
   |      3      |    line     |           value           |
   +------+------+------+------+------+------+------+------+

- **value**: Direction.

  - 0: None
  - 1: Output
  - 2: Input

**Reply:**

.. code-block:: none

   +------+--------+--------+
   | 0x00 |  0x01  |  0x02  |
   |   0  | status |    0   |
   +------+--------+--------+

- **status**:

  - 0: Ok
  - 1: Error


GET_VALUE (Cmd=4)
~~~~~~~~~~~~~~~~~

**Request:**

.. code-block:: none

   +------+------+------+------+------+------+------+------+
   | 0x00 | 0x01 | 0x02 | 0x03 | 0x04 | 0x05 | 0x06 | 0x07 |
   |      4      |    line     |             0             |
   +------+------+------+------+------+------+------+------+

**Reply:**

.. code-block:: none

   +------+--------+--------+
   | 0x00 |  0x01  |  0x02  |
   |   0  | status | value  |
   +------+--------+--------+

- **status**:

  - 0: Ok
  - 1: Error

- **value**: Level.

  - 0: Low
  - 1: High


SET_VALUE (Cmd=5)
~~~~~~~~~~~~~~~~~

**Request:**

.. code-block:: none

   +------+------+------+------+------+------+------+------+
   | 0x00 | 0x01 | 0x02 | 0x03 | 0x04 | 0x05 | 0x06 | 0x07 |
   |      5      |    line     |           value           |
   +------+------+------+------+------+------+------+------+

- **value**: Output level.

  - 0: Low
  - 1: High

**Reply:**

.. code-block:: none

   +------+--------+--------+
   | 0x00 |  0x01  |  0x02  |
   |   0  | status |    0   |
   +------+--------+--------+

- **status**:

  - 0: Ok
  - 1: Error


SET_IRQ_TYPE (Cmd=6)
~~~~~~~~~~~~~~~~~~~~

**Request:**

.. code-block:: none

   +------+------+------+------+------+------+------+------+
   | 0x00 | 0x01 | 0x02 | 0x03 | 0x04 | 0x05 | 0x06 | 0x07 |
   |      6      |    line     |           value           |
   +------+------+------+------+------+------+------+------+

- **value**: IRQ types.

  - 0: Interrupt disabled
  - 1: Rising edge trigger
  - 2: Falling edge trigger
  - 3: Both edge trigger
  - 4: High level trigger
  - 8: Low level trigger

**Reply:**

.. code-block:: none

   +------+--------+--------+
   | 0x00 |  0x01  |  0x02  |
   |   0  | status |    0   |
   +------+--------+--------+

- **status**:

  - 0: Ok
  - 1: Error


Interrupt Messages
------------------

Interrupt messages are sent by the remote core and they have
**Type=1 (GPIO_RPMSG_NOTIFY)**:

When a GPIO line asserts an interrupt on the remote processor, the firmware
should immediately mask the corresponding interrupt source and send a
notification message to the Linux. Upon completion of the interrupt
handling on the Linux side, the driver should issue a
command **SET_IRQ_TYPE** to the firmware to unmask the interrupt.

.. code-block:: none

   +------+------+--------+
   | 0x00 | 0x01 |  0x02  |
   |   1  |     line      |
   +------+------+--------+

- **line**: Little endian. The GPIO line (pin) index.

