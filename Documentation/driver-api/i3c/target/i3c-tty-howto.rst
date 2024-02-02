.. SPDX-License-Identifier: GPL-2.0

===================
I3C TTY User Guide
===================

:Author: Frank Li <Frank.Li@nxp.com>

This document is a guide to help users use i3c-target-tty function driver and
i3ctty master driver for testing I3C. The list of steps to be followed in the
master side and target side is given below.

Endpoint Device
===============

Endpoint Controller Devices
---------------------------

To find the list of target controller devices in the system::

	# ls  /sys/class/i3c_target/
	  44330000.i3c-target

If CONFIG_I3C_SLAVE_CONFIGFS is enabled::

	# ls /sys/kernel/config/i3c_target/controllers/
	  44330000.i3c-target


Endpoint Function Drivers
-------------------------

To find the list of target function drivers in the system::

	# ls /sys/bus/i3c_target_func/drivers
	  tty

If CONFIG_I3C_SLAVE_CONFIGFS is enabled::

	# ls /sys/kernel/config/i3c_target/functions
	  tty


Creating i3c-target-tty Device
----------------------------

I3C target function device can be created using the configfs. To create
i3c-target-tty device, the following commands can be used::

	# mount -t configfs none /sys/kernel/config
	# cd /sys/kernel/config/i3c_target/
	# mkdir functions/tty/func1

The "mkdir func1" above creates the i3c-target-tty function device that will
be probed by i3c tty driver.

The I3C target framework populates the directory with the following
configurable fields::

	# ls functions/tty/func1
	bcr  dcr  ext_id  instance_id  max_read_len  max_write_len
	part_id  vendor_id  vendor_info

The I3C target function driver populates these entries with default values when
the device is bound to the driver. The i3c-target-tty driver populates vendorid
with 0xffff and interrupt_pin with 0x0001::

	# cat functions/tty/func1/vendor_id
	  0x0

Configuring i3c-target-tty Device
-------------------------------

The user can configure the i3c-target-tty device using configfs entry. In order
to change the vendorid, the following commands can be used::

	# echo 0x011b > functions/tty/func1/vendor_id
	# echo 0x1000 > functions/tty/func1/part_id
	# echo 0x6 > functions/tty/t/bcr

Binding i3c-target-tty Device to target Controller
------------------------------------------------

In order for the target function device to be useful, it has to be bound to a
I3C target controller driver. Use the configfs to bind the function device to
one of the controller driver present in the system::

	# ln -s functions/tty/func1 controllers/44330000.i3c-target/

I3C Master Device
================

Check I3C tty device is probed

	# ls /sys/bus/i3c/devices/0-23610000000
	0-23610000000:0  bcr  dcr  driver  dynamic_address  hdrcap
	modalias  pid  power  subsystem  tty  uevent

Using Target TTY function Device
-----------------------------------

Host side:
	cat /dev/ttyI3C0
Target side
	echo abc >/dev/ttyI3C0

You will see "abc" show at console.

You can use other tty tool to test I3C target tty device.
