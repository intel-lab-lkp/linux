.. SPDX-License-Identifier: GPL-2.0-or-later

==========================
Samsung Galaxy Book Extras
==========================

December 9, 2024

Joshua Grisham <josh@joshuagrisham.com>

This is a Linux x86 platform driver for Samsung Galaxy Book series notebook
devices which utilizes Samsung's ``SCAI`` ACPI device in order to control
extra features and receive various notifications.


Supported devices
=================

"SAMSUNG ELECTRONICS CO., LTD." devices of type "Notebook" which have one of the
supported ACPI device IDs should be supported. This covers most of the "Samsung
Galaxy Book" series notebooks that are currently available as of this writing,
and could include other Samsung notebook devices as well.


Status
======

The following features are currently supported:

- :ref:`Keyboard backlight <keyboard-backlight>` control
- :ref:`Performance mode <performance-mode>` control implemented using the
  platform profile interface
- :ref:`Battery charge control end threshold
  <battery-charge-control-end-threshold>` (stop charging battery at given
  percentage value) implemented as a battery device extension
- :ref:`Fan speed <fan-speed>` monitoring via ``fan_speed_rpm`` sysfs attribute
  plus a new hwmon device
- :ref:`Settings Attributes <settings-attributes>` to allow control of various
  device settings
- :ref:`Handling of Fn hotkeys <keyboard-hotkey-actions>` for various actions

Because different models of these devices can vary in their features, there is
logic built within the driver which attempts to test each implemented feature
for a valid response before enabling its support (registering additional devices
or extensions, adding sysfs attributes, etc). Therefore, it can be important to
note that not all features may be supported for your particular device.

The following features might be possible to implement but will require
additional investigation and are therefore not supported at this time:

- "Dolby Atmos" mode for the speakers
- "Outdoor Mode" for increasing screen brightness on models with ``SAM0427``
- "Silent Mode" on models with ``SAM0427``


Parameters
==========

The driver includes a list of boolean parameters that allow for manually
enabling or disabling various features:

- ``kbd_backlight``: Enable Keyboard Backlight control (default on)
- ``performance_mode``: Enable Performance Mode control (default on)
- ``battery_threshold``: Enable battery charge threshold control (default on)
- ``fan_speed``: Enable fan speed (default on)
- ``allow_recording``: Enable control to allow or block access to camera and
  microphone (default on)
- ``i8042_filter``: Enable capture and execution of keyboard-based hotkey events
  (default on)

.. note::
  Even if you explicitly try to enable a feature using its parameter, support
  for it will still be evaluated by the driver, and the feature will be
  disabled if it does not appear to be supported on your device.

The availability of various sysfs file-based "settings" attributes
(``usb_charge``, ``start_on_lid_open``, etc) will be determined automatically
and cannot be manually disabled at this time.


.. _keyboard-backlight:

Keyboard backlight
==================

Controlled by parameter: ``kbd_backlight``

A new LED class named ``samsung-galaxybook::kbd_backlight`` is created which
will then expose the device using the standard sysfs-based LED interface at
``/sys/class/leds/samsung-galaxybook::kbd_backlight``. Brightness can be
controlled by writing values 0 to 3 to the ``brightness`` sysfs attribute or
with any other desired userspace utility.

.. note::
  Most of these devices have an ambient light sensor which also turns
  off the keyboard backlight under well-lit conditions. This behavior does not
  seem possible to control at this time, but can be good to be aware of.


.. _performance-mode:

Performance mode
================

Controlled by parameter: ``performance_mode``

This driver implements the
Documentation/userspace-api/sysfs-platform_profile.rst interface for working
with the "performance mode" function of the Samsung ACPI device.

Mapping of each Samsung "performance mode" to its respective platform profile is
done dynamically based on a list of the supported modes reported by the device
itself. Preference is given to always try and map ``low-power``, ``balanced``,
and ``performance`` profiles, as these seem to be the most common profiles
utilized (and sometimes even required) by various userspace tools.

The result of the mapping will be printed in the kernel log when the module is
loaded. Supported profiles can also be retrieved from
``/sys/firmware/acpi/platform_profile_choices``, while
``/sys/firmware/acpi/platform_profile`` can be used to read or write the
currently selected profile.

The ``balanced`` platform profile will be set during module load if no profile
has been previously set.


.. _battery-charge-control-end-threshold:

Battery charge control end threshold
====================================

Controlled by parameter: ``battery_threshold``

This platform driver will add the ability to set the battery's charge control
end threshold, but does not have the ability to set a start threshold.

This feature is typically called "Battery Saver" by the various Samsung
applications in Windows, but in Linux we have implemented the standardized
"charge control threshold" sysfs interface on the battery device to allow for
controlling this functionality from the userspace.

The sysfs attribute
``/sys/class/power_supply/BAT1/charge_control_end_threshold`` can be used to
read or set the desired charge end threshold.

If you wish to maintain interoperability with Windows, then you should set the
value to 80 to represent "on", or 0 to represent "off", as these are the values
currently recognized by the various Windows-based Samsung applications and
services as "on" or "off". Otherwise, the device will accept any value between 0
(off) and 99 as the percentage that you wish the battery to stop charging at.

.. note::
  If you try to set a value of 100, the driver will also accept this input, but
  will set the attribute value to 0 (i.e. 100% will "remove" the end threshold).


.. _fan-speed:

Fan speed
=========

Controlled by parameter: ``fan_speed``

The number and type of fans on these devices can vary, and different methods
must be used in order to be able to successfully read their status.

In cases where Samsung has implemented the standard ACPI method ``_FST`` for a
fan device, the other methods in the ACPI specification which would cause
the kernel to automatically add the ``fan_speed_rpm`` attribute are not always
present. On top of this, it seems that there are some bugs in the firmware that
throw an exception when the ``_FST`` method is executed.

This platform driver attempts to resolve all PNP fans that are present in the
ACPI of supported devices, and add support for reading their speed using the
following decision tree:

1. Do all 4 required methods exist so that the fan speed should be reported
   out-of-the-box by ACPI? If yes, then assume this fan is already set up and
   available.

2. Does the method ``_FST`` exist and appears to be working (returns a speed
   value greater than 0)? If yes, add an attribute ``fan_speed_rpm`` to this fan
   device and add a fan input channel for it to the hwmon device. The returned
   value will be directly read from the ``_FST`` method.

3. Does the field ``FANS`` (fan speed level) exist on the embedded controller,
   and the table ``FANT`` (fan speed level table) exist on the fan device? If
   yes, add the ``fan_speed_rpm`` attribute to this fan device and add a fan
   input channel for it to the hwmon device. The returned value will be based
   on a match of the current value of ``FANS`` compared to a list of level
   speeds from the ``FANT`` table.

The fan speed for all supported fans can be monitored using hwmon sensors or by
reading the ``fan_speed_rpm`` sysfs attribute of each fan device.


.. _settings-attributes:

Settings Attributes
===================

Various hardware settings can be controlled by the following sysfs attributes:

- ``allow_recording`` (allows or blocks usage of built-in camera and microphone)
- ``start_on_lid_open`` (power on automatically when opening the lid)
- ``usb_charge`` (allows USB ports to provide power even when device is off)

These attributes will be available under the path for your supported ACPI Device
ID's platform device (``SAM0428``, ``SAM0429``, etc), and can most reliably
be found by seeing which device has been bound to the ``samsung-galaxybook``
driver. Here are some examples: ::

  # find which device ID has been bound to the driver
  ls /sys/bus/platform/drivers/samsung-galaxybook/ | grep SAM

  # see SAM0429 attributes
  ls /sys/bus/platform/drivers/samsung-galaxybook/SAM0429\:00

  # see attributes no matter the device ID (using wildcard expansion)
  ls /sys/bus/platform/drivers/samsung-galaxybook/SAM*

Most shells should support using wildcard expansion to directly read and write
these attributes using the above pattern. Example: ::

  # read value of start_on_lid_open
  cat /sys/bus/platform/drivers/samsung-galaxybook/SAM*/start_on_lid_open

  # turn on start_on_lid_open
  echo true | sudo tee /sys/bus/platform/drivers/samsung-galaxybook/SAM*/start_on_lid_open

It is also possible to use a udev rule to create a fixed-path symlink to your
device under ``/dev`` (e.g. ``/dev/samsung-galaxybook``), no matter the device
ID, to further simplify reading and writing these attributes in the userspace.

Allow recording (allow_recording)
---------------------------------

``/sys/bus/platform/drivers/samsung-galaxybook/SAM*/allow_recording``

Controlled by parameter: ``allow_recording``

Controls the "Allow recording" setting, which allows or blocks usage of the
built-in camera and microphone (boolean).

Start on lid open (start_on_lid_open)
-------------------------------------

``/sys/bus/platform/drivers/samsung-galaxybook/SAM*/start_on_lid_open``

Controls the "Start on lid open" setting, which sets the device to power on
automatically when the lid is opened (boolean).

USB charge (usb_charge)
-----------------------

``/sys/bus/platform/drivers/samsung-galaxybook/SAM*/usb_charge``

Controls the "USB charge" setting, which allows USB ports to provide power even
when the device is turned off (boolean).

.. note::
  For most devices, this setting seems to only apply to the USB-C ports.


.. _keyboard-hotkey-actions:

Keyboard hotkey actions (i8042 filter)
======================================

Controlled by parameter: ``i8042_filter``

The i8042 filter will swallow the keyboard events for the Fn+F9 hotkey (Multi-
level keyboard backlight toggle) and Fn+F10 hotkey (Allow/block recording
toggle) and instead execute their actions within the driver itself.

Fn+F9 will cycle through the brightness levels of the keyboard backlight. A
notification will be sent using ``led_classdev_notify_brightness_hw_changed``
so that the userspace can be aware of the change. This mimics the behavior of
other existing devices where the brightness level is cycled internally by the
embedded controller and then reported via a notification.

Fn+F10 will toggle the value of the "Allow recording" setting.


ACPI notifications and ACPI hotkey actions
==========================================

There is a new "Samsung Galaxy Book extra buttons" input device created which
will send input events for the following notifications from the ACPI device:

- Notification when the battery charge control end threshold has been reached
  and the "battery saver" feature has stopped the battery from charging
- Notification when the device has been placed on a table (not available on all
  models)
- Notification when the device has been lifted from a table (not available on
  all models)

The Fn+F11 Performance mode hotkey is received as an ACPI notification. It will
be handled in a similar way as the Fn+F9 and Fn+F10 hotkeys; namely, that the
keypress will be swallowed by the driver and each press will cycle to the next
available platform profile.
