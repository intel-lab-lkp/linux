.. SPDX-License-Identifier: GPL-2.0-or-later

============================================
Uniwill WMI Notebook driver (uniwill-laptop)
============================================

Introduction
============

Many notebooks manufactured by Uniwill (either directly or as ODM) provide a WMI-based
EC interface for controlling various platform settings like sensors and fan control.
This interface is used by the ``uniwill-laptop`` driver to map those features onto standard
kernel interfaces.

WMI interface description
=========================

The WMI interface description can be decoded from the embedded binary MOF (bmof)
data using the `bmfdec <https://github.com/pali/bmfdec>`_ utility:

::

  [WMI, Dynamic, Provider("WmiProv"), Locale("MS\\0x409"),
   Description("Class used to operate methods on a ULong"),
   guid("{ABBC0F6F-8EA1-11d1-00A0-C90629100000}")]
  class AcpiTest_MULong {
    [key, read] string InstanceName;
    [read] boolean Active;

    [WmiMethodId(1), Implemented, read, write, Description("Return the contents of a ULong")]
    void GetULong([out, Description("Ulong Data")] uint32 Data);

    [WmiMethodId(2), Implemented, read, write, Description("Set the contents of a ULong")]
    void SetULong([in, Description("Ulong Data")] uint32 Data);

    [WmiMethodId(3), Implemented, read, write,
     Description("Generate an event containing ULong data")]
    void FireULong([in, Description("WMI requires a parameter")] uint32 Hack);

    [WmiMethodId(4), Implemented, read, write, Description("Get and Set the contents of a ULong")]
    void GetSetULong([in, Description("Ulong Data")] uint64 Data,
                     [out, Description("Ulong Data")] uint32 Return);

    [WmiMethodId(5), Implemented, read, write,
     Description("Get and Set the contents of a ULong for Dollby button")]
    void GetButton([in, Description("Ulong Data")] uint64 Data,
                   [out, Description("Ulong Data")] uint32 Return);
  };

Most of the WMI-related code was copied from the Windows driver samples, which unfortunately means
that the WMI-GUID is not unique. This makes the WMI-GUID unusable for autoloading.

WMI method GetULong()
---------------------

This WMI method was copied from the Windows driver samples and has no function.

WMI method SetULong()
---------------------

This WMI method was copied from the Windows driver samples and has no function.

WMI method FireULong()
----------------------

This WMI method allows to inject a WMI event with a 32-bit payload. Its primary purpose seems
to be debugging.

WMI method GetSetULong()
------------------------

This WMI method is used to communicate with the EC. The ``Data`` argument hold the following
information (starting with the least significant byte):

1. 16-bit address
2. 16-bit data (set to ``0x0000`` when reading)
3. 16-bit operation (``0x0100`` for reading and ``0x0000`` for writing)
4. 16-bit reserved (set to ``0x0000``)

The first 8 bits of the ``Return`` value contain the data returned by the EC when reading.
The special value ``0xFEFEFEFE`` is used to indicate a communication failure with the EC.

WMI method GetButton()
----------------------

This WMI method is not implemented on all machines and has an unknown purpose.

Relation with the ``INOU0000`` ACPI device
==========================================

It seems that many of the embedded controller registers can also be accessed by using the ``ECRR``
and ``ECRW`` ACPI control methods under the ``INOU0000`` ACPI device. This sidesteps the overhead
of the WMI interface but does not work for the registers in the range between ``0x1800`` and
``0x18FF``. More research is needed to determine whether this interface imposes additional
restrictions.

Reverse-Engineering the Uniwill WMI interface
=============================================

.. warning:: Randomly poking the EC can potentially cause damage to the machine and other unwanted
             side effects, please be careful.

The EC behind the ``GetSetULong`` method is used by the OEM software supplied by the manufacturer.
Reverse-engineering of this software is difficult since it uses an obfuscator, however some parts
are not obfuscated. In this case `dnSpy <https://github.com/dnSpy/dnSpy>`_ could also be helpful.

The EC can be accessed under Windows using powershell (requires admin privileges):

::

  > $obj = Get-CimInstance -Namespace root/wmi -ClassName AcpiTest_MULong | Select-Object -First 1
  > Invoke-CimMethod -InputObject $obj -MethodName GetSetULong -Arguments @{Data = <input>}

Special thanks go to github user `pobrn` which developed the
`qc71_laptop <https://github.com/pobrn/qc71_laptop>`_ driver on which this driver is partly based.
The same is true for Tuxedo Computers, which developed the
`tuxedo-drivers <https://gitlab.com/tuxedocomputers/development/packages/tuxedo-drivers>`_ package
which also served as a foundation for this driver.
