.. SPDX-License-Identifier: GPL-2.0-or-later

======================================
Uniwill WMI event driver (uniwill-wmi)
======================================

Introduction
============

Many notebooks manufactured by Uniwill (either directly or as ODM) provide a WMI-based
event interface for various platform events like hotkeys. This interface is used by the
``uniwill-wmi`` driver to react to hotkey presses.

WMI interface description
=========================

The WMI interface description can be decoded from the embedded binary MOF (bmof)
data using the `bmfdec <https://github.com/pali/bmfdec>`_ utility:

::

  [WMI, Dynamic, Provider("WmiProv"), Locale("MS\\0x409"),
   Description("Class containing event generated ULong data"),
   guid("{ABBC0F72-8EA1-11d1-00A0-C90629100000}")]
  class AcpiTest_EventULong : WmiEvent {
    [key, read] string InstanceName;
    [read] boolean Active;

    [WmiDataId(1), read, write, Description("ULong Data")] uint32 ULong;
  };

Most of the WMI-related code was copied from the Windows driver samples, which unfortunately means
that the WMI-GUID is not unique. This makes the WMI-GUID unusable for autoloading.

WMI event data
--------------

The WMI event data contains a single 32-bit value which is used to indicate various platform events.
Many non-hotkey events are not directly consumed by the driver itself, but are instead handled by
the ``uniwill-laptop`` driver.

Reverse-Engineering the Uniwill WMI event interface
===================================================

The driver logs debug messages when receiving a WMI event. Thus enabling debug messages will be
useful for finding unknown event codes.

Special thanks go to github user `pobrn` which developed the
`qc71_laptop <https://github.com/pobrn/qc71_laptop>`_ driver on which this driver is partly based.
The same is true for Tuxedo Computers, which developed the
`tuxedo-drivers <https://gitlab.com/tuxedocomputers/development/packages/tuxedo-drivers>`_ package
which also served as a foundation for this driver.
