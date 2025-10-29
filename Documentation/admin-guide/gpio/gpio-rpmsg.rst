.. SPDX-License-Identifier: GPL-2.0-or-later

GPIO RPMSG Protocol
===================

The GPIO RPMSG transport protocol is used for communication and interaction
with GPIO controllers located on remote cores on the RPMSG bus.

Message Format
--------------

The RPMSG message consists of a 14-byte packet with the following layout:

.. code-block:: none

   +-----+------+------+-----+-----+------------+-----+-----+-----+----+
   |0x00 |0x01  |0x02  |0x03 |0x04 |0x05..0x09  |0x0A |0x0B |0x0C |0x0D|
   |cate |major |minor |type |cmd  |reserved[5] |line |port |  data    |
   +-----+------+------+-----+-----+------------+-----+-----+-----+----+

- **Cate (Category field)**: Indicates the category of the message, such as GPIO, I2C, PMIC, AUDIO, etc.

  Defined categories:

  - 1: RPMSG_LIFECYCLE
  - 2: RPMSG_PMIC
  - 3: RPMSG_AUDIO
  - 4: RPMSG_KEY
  - 5: RPMSG_GPIO
  - 6: RPMSG_RTC
  - 7: RPMSG_SENSOR
  - 8: RPMSG_AUTO
  - 9: RPMSG_CATEGORY
  - A: RPMSG_PWM
  - B: RPMSG_UART

- **Major**: Major version number.

- **Minor**: Minor version number.

- **Type (Message Type)**: For the GPIO category, can be one of:

  - 0: GPIO_RPMSG_SETUP
  - 1: GPIO_RPMSG_REPLY
  - 2: GPIO_RPMSG_NOTIFY

- **Cmd**: Command code, used for GPIO_RPMSG_SETUP messages.

- **reserved[5]**: Reserved bytes.

- **line**: The GPIO line index.

- **port**: The GPIO controller index.

GPIO Commands
-------------

Commands are specified in the **Cmd** field for **GPIO_RPMSG_SETUP** (Type=0) messages.

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
  - 1: General error (early remote software only returns this unclassified error)
  - 2: Not supported
  - 3: Resource not available
  - 4: Resource busy
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

Notification Message
--------------------

Notifications are sent with **Type=2 (GPIO_RPMSG_NOTIFY)**:

.. code-block:: none

   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+----+
   |0x00 |0x01 |0x02 |0x03 |0x04 |0x05..0x09 |0x0A |0x0B |0x0C |0x0D|
   | 5   | 1   | 0   | 2   | 0   |  0        |line |port | 0   | 0  |
   +-----+-----+-----+-----+-----+-----------+-----+-----+-----+----+

- **line**: The GPIO line index.
- **port**: The GPIO controller index.

