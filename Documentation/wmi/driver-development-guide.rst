.. SPDX-License-Identifier: GPL-2.0-or-later

============================
WMI driver development guide
============================

The WMI subsystem provides a rich driver api for implementing WMI drivers,
documented at Documentation/driver-api/wmi.rst. This document will serve
as an introductory guide for WMI driver writers using this API. It is supposed
t be an successor to the original `LWN article <https://lwn.net/Articles/391230/>`_
which deals with WMI drivers using the deprecated GUID-based WMI interface.

Optaining WMI device information
--------------------------------

Before developing an WMI driver, information about the WMI device in question
must be optained. The `lswmi <https://pypi.org/project/lswmi>`_ utility can be
used to display detailed WMI device information using the following command:

::

  lswmi -V

The resulting output will contain information about all WMI devices inside a given
machine, plus some extra information.

In order to find out more about the interface used to communicate with a WMI device,
the `bmfdec <https://github.com/pali/bmfdec>`_ utilities can be used to decode
the Binary MOF information used to describe WMI devices. The ``wmi-bmof`` driver
exposes this information to userspace, see Documentation/ABI/stable/sysfs-platform-wmi-bmof.
In order to retrieve the decoded Binary MOF information, use the following command (requires root):

::

  ./bmf2mof /sys/bus/wmi/devices/05901221-D566-11D1-B2F0-00A0C9062910[-X]/bmof

Sometimes, looking at the disassembled ACPI tables used to describe the WMI device
helps in understanding how the WMI device is supposed to work. To find out which
ACPI method handles which WMI device, the `fwts <https://github.com/fwts/fwts>`_
program can be used with the following command (requires root):

::

  fwts wmi -

Basic WMI driver structure
--------------------------

The basic WMI driver is build around the struct wmi_driver, which is then bound
to matching WMI devices using an struct wmi_device_id table. Please note that each
WMI driver should be able to be instantiated multiple times.

::

  static const struct wmi_device_id foo_id_table[] = {
         { "936DA01F-9ABD-4D9D-80C7-02AF85C822A8", NULL },
         { }
  };
  MODULE_DEVICE_TABLE(wmi, foo_id_table);

  static struct wmi_driver foo_driver = {
        .driver = {
                .name = "foo",
                .probe_type = PROBE_PREFER_ASYNCHRONOUS,        /* optional */
                .pm = pm_sleep_ptr(&foo_dev_pm_ops),            /* optional */
        },
        .id_table = foo_id_table,
        .probe = foo_probe,
        .remove = foo_remove,         /* optional, devres is preferred */
        .notify = foo_notify,         /* optional, for event handling */
  };
  module_wmi_driver(foo_driver);

If your WMI driver is not using any deprecated GUID-based WMI functions and is
able to be instantiated multiple times, please add its GUID to ``allow_duplicates``
inside drivers/platform/x86/wmi.c, so that the WMI subsystem does not block duplicate
GUIDs for it.

WMI method drivers
------------------

WMI drivers can call WMI device methods using wmidev_evaluate_method(), the
structure of the ACPI buffer passed to this function is device-specific and usually
needs some tinkering to get right. Looking at the ACPI tables containing the WMI
device usually helps here. The method id and instance number passed to this function
are also device-specific, looking at the decoded Binary MOF is usually enough to
find the right values.
The maximum instance number can be retrieved during runtime using wmidev_instance_count().

Take a look at drivers/platform/x86/inspur_platform_profile.c for an example WMI method driver.

WMI data block drivers
----------------------

WMI drivers can query WMI device data blocks using wmidev_block_query(), the
structure of the returned ACPI object is again device-specific. Some WMI devices
also allow for setting data blocks using wmidev_block_set().
The maximum instance number can also be retrieved using wmidev_instance_count().

Take a look at drivers/platform/x86/intel/wmi/sbl-fw-update.c for an example
WMI data block driver.

WMI event drivers
-----------------

WMI drivers can receive WMI event notifications by providing the notify() callback
inside the struct wmi_driver. The WMI subsystem will then take care of setting
up the WMI event accordingly. Plase note that the ACPI object passed to this callback
is optional and its structure device-specific. It also does not need to be freed,
the WMI subsystem takes care of that.

Take a look at drivers/platform/x86/xiaomi-wmi.c for an example WMI event driver.

Things to avoid
---------------

When developing WMI drivers, there are a couple of things which should be avoid
if feasible:

- usage of the deprecated GUID-based WMI interface
- bypassing of the WMI subsystem when talking to WMI devices
- WMI drivers which cannot be instantiated multiple times.

Many older WMI drivers violate one or more points from this list. The reason for
this is that the WMI subsystem evolved significantly over the last two decades,
so there is a lot of legacy cruft inside older WMI drivers.
