.. SPDX-License-Identifier: GPL-2.0-or-later

===========================================================
Dell All-In-One Series Platform Integration (dell-wmi-pega)
===========================================================

Introduction
------------

Many Dell All-In-One devices contain a WMI device for notifying the underlying platform
of various OS-specific events like power up, shutdown and backlight brightness control.

Not implementing support for this WMI device might prevent hotkeys exposed over WMI from
working on some models.

WMI interface description
=========================

The WMI interface description can be decoded from the embedded binary MOF (bmof)
data using the `bmfdec <https://github.com/pali/bmfdec>`_ utility:

::

 [WMI, Dynamic, Provider("WmiProv"), Locale("MS\\0x409"),
  Description("Class used to operate method - WMBC"),
  guid("{2D703EE8-36AC-4F88-BDB5-7556CE6B2946}") : ToInstance]
 class AcpiOSD_WMBC_method : WMIMethod {

   [key, read]
   string InstanceName;

   [read]
   boolean Active;

   [WmiMethodId(1), Implemented, read, write,
    Description("Invoked by AP when system up.")]
   void PegaOemNotificationStartUp([out, Description("Return Value")] uint32 RetVal);

   [WmiMethodId(2), Implemented, read, write,
    Description("Invoked by AP when system is going to off state.")]
   void PegaOemNotificationStop([out, Description("Return Value")] uint32 RetVal);

   [WmiMethodId(3), Implemented, read, write,
    Description("Invoked by Application to tune up brightness.")]
   void PegaOemNotificationBrightnessUP([out, Description("Return Value")] uint32 RetVal);

   [WmiMethodId(4), Implemented, read, write,
    Description("Invoked by Application to tune down brightness.")]
   void PegaOemNotificationBrightnessDown([out, Description("Return Value")] uint32 RetVal);

   [WmiMethodId(5), Implemented, read, write, Description("Reserved.")]
   void PegaOemNotificationRSVD1([out, Description("Return Value")] uint32 RetVal);

   [WmiMethodId(6), Implemented, read, write, Description("Reserved.")]
   void PegaOemNotificationRSVD2([out, Description("Return Value")] uint32 RetVal);
 };

The return value of each WMI method should be equal to the WMI method ID.

WMI method PegaOemNotificationStartUp()
---------------------------------------

Notifies the platform that the OS is operational. On some models this WMI method also enables
reporting of hotkey events over WMI.

WMI method PegaOemNotificationStop()
------------------------------------

Notifies the platform that the OS is shutting down.

WMI method PegaOemNotificationBrightnessUP()
--------------------------------------------

Notifies the platform that the user wants to increase the backlight brightness. This WMI method
usually only forwards this request to the acpi-video driver, so the dell-wmi-pega driver does
not use it.

WMI method PegaOemNotificationBrightnessDown()
----------------------------------------------

Notifies the platform that the user wants to decrease the backlight brightness. See
``PegaOemNotificationBrightnessUP()`` for details.
