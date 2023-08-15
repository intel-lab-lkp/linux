.. SPDX-License-Identifier: GPL-2.0-or-later

Virtual GPIO Consumer
=====================

The virtual GPIO Consumer module allows users to create dynamic GPIO lookup
tables using the exposed configfs interface and instantiate virtual GPIO
consumer devices that retrieve and use GPIOs using these tables.

Creating GPIO consumers
-----------------------

The gpio-consumer module registers a configfs subsystem called
``'gpio-consumer'``. For details of the configfs filesystem, please refer to
the configfs documentation.

The user can create a hierarchy of configfs groups and items as well as modify
values of exposed attributes. Once the consumer is instantiated, this hierarchy
will be translated to appropriate device properties. The general structure is:

**Group:** ``/config/gpio-consumer``

This is the top directory of the gpio-consumer configfs tree.

**Group:** ``/config/gpio-consumer/consumer-device``

**Attribute:** ``/config/gpio-consumer/gpio-device/function``

**Attribute:** ``/config/gpio-consumer/gpio-device/live``

This is a directory representing a GPIO consumer device. The ``'function'``
attribute defines how the device will use requested GPIOs. It takes the
following values: ``'active'`` - GPIOs will be requested as output-high and
kept in this state until the device is unbound, ``'toggle'`` - GPIOs will
be requested in output mode and then toggled periodically (every second)
between 1 and 0, ``'monitor'`` - GPIOs will be requested in input mode, then
requested as interrupts and their firing reported in the kernel log.

The ``'live'`` attribute allows to trigger the actual creation of the device
once it's fully configured. The accepted values are: ``'1'`` to enable the
virtual device and ``'0'`` to disable and tear it down.

Creating GPIO lookup tables
---------------------------

User can create a number of configfs groups under the device group with the
following properties:

**Group:** ``/config/gpio-consumer/consumer-device/lookupX``

**Attribute:** ``/config/gpio-consumer/consumer-device/lookupX/key``

**Attribute:** ``/config/gpio-consumer/consumer-device/lookupX/offset``

**Attribute:** ``/config/gpio-consumer/consumer-device/lookupX/drive``

**Attribute:** ``/config/gpio-consumer/consumer-device/lookupX/pull``

**Attribute:** ``/config/gpio-consumer/consumer-device/lookupX/active_low``

**Attribute:** ``/config/gpio-consumer/consumer-device/lookupX/transitory``

This group represents a single entry in the GPIO lookup table. The name of the
sub-directory maps to the ``'con_id'`` field of ``'struct gpiod_lookup'``. The
``'key'`` attribute represents either the name of the chip this GPIO belongs to
or the GPIO line name. This depends on the value of the ``'offset'`` attribute:
if its value is >= 0, then ``'key'`` represents the label of the chip to lookup
while ``'offset'`` represents the offset of the line in that chip. If
``'offset'`` is < 0, then ``'key'`` represents the name of the line.

The remaining attributes map to the ``'flags'`` field of the GPIO lookup struct.
The first two take string values as arguments:

**``'drive'``:** ``'push-pull'``, ``'open-drain'``, ``'open-source'``
**``'pull'``:** ``'pull-up'``, ``'pull-down'``, ``'pull-disabled'``, ``'as-is'``

``'active_low'`` and ``'transitory'`` are boolean attributes.

Activating GPIO consumers
-------------------------

Once the confiuration is complete, the ``'live'`` attribute must be set to 1 in
order to instantiate the consumer. It can be set back to 0 to destroy the
virtual devices. The module will synchronously wait for the new simulated device
to be successfully probed and if this doesn't happen, writing to ``'live'`` will
result in an error.

Device-tree
-----------

Virtual GPIO consumers can also be defined in device-tree. The compatible string
must be: ``"gpio-virtual-consumer"``. Supported properties are:

  ``"gpio-virtual-consumer,function"`` - function of the virtual device
  ``"gpio-virtual-consumer,lines"`` - list of lookup keys

An example device-tree code defining a virtual GPIO consumer:

.. code-block :: none

    gpio-virt-consumer {
        compatible = "gpio-virtual-consumer";

        foo-gpios = <&gpio0 5 GPIO_ACTIVE_LOW>;
        bar-gpios = <&gpio0 6 0>;

        gpio-virtual-consumer,function = "toggle";
        gpio-virtual-consumer,lines = "foo", "bar";
    };

.. SPDX-License-Identifier: GPL-2.0-or-later
