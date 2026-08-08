.. SPDX-License-Identifier: GPL-2.0

=============================
Kernel driver i2c-imc-skylake
=============================

Supported adapters:
  * Intel Skylake-X / Cascade Lake-X integrated memory controller (iMC)
    SMBus engine, PCU function 8086:2085 (socket LGA 2066, platform X299)

The same PCI ID appears on Skylake-SP / Cascade Lake-SP servers.  The driver
behaves identically there, but on server boards a BMC is far more likely to be
using the engine already; read the next section twice before opting in.

Author: Simone Chifari <simone.chifari@gmail.com>


Description
-----------

The integrated memory controller of Intel Skylake-X and Cascade Lake-X
processors contains an SMBus engine wired to the DDR4 DIMMs.  It reaches the
SPD EEPROMs (0x50-0x57) and the thermal sensors (TSOD, 0x18-0x1f) of the
installed modules, plus whatever else a particular module carries on those
lines.

The engine has two independent channels.  DIMM slots 1 and 2 are on channel 0,
slots 3 and 4 on channel 1, so the driver registers two i2c adapters::

    $ i2cdetect -l | grep iMC
    i2c-6   smbus   iMC SMBus Skylake-X channel 0   SMBus adapter
    i2c-7   smbus   iMC SMBus Skylake-X channel 1   SMBus adapter

Both channels share one hardware engine, so the driver serializes every
transaction across both adapters.


This driver is not enabled by default
-------------------------------------

The SMBus engine has no arbitration mechanism.  System Management Mode, a BMC,
and the memory controller's own closed-loop thermal throttling (CLTT) can all
drive the same registers, and nothing in the PCI ID tells the driver whether
any of them is doing so on a given system.  Two masters using the engine at the
same time can send a transaction to the wrong address or return the wrong data.

The driver therefore refuses to bind unless the operator opts in::

    modprobe i2c-imc-skylake allow_unsafe_access=1

Do this only on a system where concurrent firmware access has been excluded.
Desktop and HEDT boards have no BMC, but CLTT may still be enabled by the
firmware.

The driver reduces, but cannot remove, the risk.

At probe it reads SMBCNTL on every iMC channel function (device 10 and 12,
function 0; offsets 0xe88 and 0xe98, documented in the Intel Xeon Processor
Scalable Family datasheet volume 2, reference 614073, section 3.1.9).
``SMB_TSOD_POLL_EN`` in that register says whether the memory controller is
polling the DIMM thermal sensors on its own, which Intel documents as mutually
exclusive with SPD command access.  If the bit is set on any channel the driver
refuses to bind: the engine is not ours to drive.  This covers the memory
controller, not SMM or a BMC, which remain the reason for the opt-in.

Per transfer:

  * every transaction waits for both channels to report idle before it
    touches any register;
  * the previous command state of both channels is saved, the TSOD-active
    state is cleared for the duration of the transfer, and both are restored
    afterwards;
  * the command word is read back and must be the one just written, in the
    bits the driver produces; two identical consecutive commands are the
    one case this cannot cover, because bit 29 cannot serve as a nonce -
    the engine errors out any command with it clear;
  * the result is read, and only then are the command registers compared
    against what the driver left in them.  A difference means another master
    used the engine, and the transfer is failed with ``-EAGAIN`` rather than
    reported as successful.

The last check is what makes an unarbitrated system detectable: if the log
shows

  ``chN command changed under us (...): another master is using the engine``

then firmware is using the engine and the driver should not be loaded on that
system.

The TSOD-polling check runs again on resume from suspend, because firmware
can enable the polling across a sleep state.  If it did, the adapters stay
suspended and transfers fail with ``-ESHUTDOWN`` instead of racing the memory
controller.


Config space access
-------------------

The engine's registers live at config space offsets 0x9C-0xB8 of the PCU
function and are reached with the ordinary ``pci_read_config_dword()`` and
``pci_write_config_dword()``.  Nothing special is required.

Supported transactions
----------------------

The driver implements SMBus Receive Byte and Send Byte, Read/Write Byte Data
and Read/Write Word Data.

That covers ``jc42`` (TSOD) and ``ee1004``, so ``decode-dimms`` reads the whole
512-byte DDR4 SPD, including the manufacturer and part-number fields that live
on page 1.

Receive Byte and Send Byte need a command word that puts no register byte on
the bus.  The engine encodes that in bit 18, which is not documented for this
function.  It was found by noting that the interface Intel does document for
the iMC channel functions (SMBCMD, datasheet reference 614073, section 3.1.8)
places ``PNTR_SEL`` immediately above ``WORD_ACCESS``, and that word access
here is bit 17.  With the bit set, consecutive reads of an SPD EEPROM return
consecutive bytes, which is the auto-increment behaviour of a Receive Byte and
cannot be produced by a transfer that names a register.

SMBus Quick is not implemented.  ``i2cdetect`` therefore has to be run in
read mode::

    $ i2cdetect -y -r 6
         0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
    20: -- -- -- -- -- -- -- 27 -- -- -- -- -- -- -- --
    30: 30 31 -- -- 34 35 UU UU -- -- -- -- -- -- -- --
    50: UU -- UU -- -- -- -- -- -- -- -- -- -- -- -- --

The ``UU`` entries are the addresses the driver claims at probe: the SPD
EEPROMs at 0x50 and 0x52, and the page-select pair at 0x36 and 0x37 that
``i2c_register_spd_write_disable()`` reserves so that nothing can write them.
See "Instantiating clients" below.

Not advertising Quick is deliberate as well as accurate.  A write to the
0x30-0x37 range is how an EE1004 SPD is write-protected, permanently on many
modules, so an adapter on a memory bus should not offer probing that writes.

Block transfers are not implemented.  The engine has block primitives, but no
device on this bus needs them.

Word transfers use the standard SMBus byte order, so ``jc42`` reading through
``i2c_smbus_read_word_swapped()`` gets the value it expects.


Instantiating clients
---------------------

SPD EEPROMs are instantiated at probe with ``i2c_register_spd()``, like the
other SMBus host drivers.  It counts the populated slots from DMI and probes
from 0x50 upwards, so an address that does not answer is left alone.  Each
channel carries half the DIMMs and the scan runs per adapter, so each finds
its own::

    i2c i2c-6: Successfully instantiated SPD at 0x50
    ee1004 6-0050: 512 byte EE1004-compliant SPD EEPROM, read-only

``decode-dimms`` therefore works with no manual step.

Nothing else is instantiated: there is no firmware description of these buses
and the driver does not scan for anything but the SPDs.  ``jc42`` has to be
added by hand where a thermal sensor is populated::

    # echo jc42 0x18 > /sys/bus/i2c/devices/i2c-6/new_device

Whether one is populated is visible in ``SMBCNTL.TSOD_PRESENT``; on the
development system that mask is empty and no ``jc42`` binds.

Two things about the SPDs are worth knowing on this bus.

A DDR4 SPD is a 512-byte array reached through a 256-byte window, and which
half is visible is a property of the bus, not of the transfer.  ``ee1004``
selects a page when it needs one and leaves it selected, so a raw ``i2cget``
issued afterwards may be looking at page 1.  Select page 0 explicitly before
reading SPD by hand::

    # i2cset -y 6 0x36 0x00 c

And ``ee1004`` owns those addresses once bound, so userspace cannot reach them
through ``/dev/i2c-*`` any more.  This is ordinary I2C behaviour, but it is
worth stating here because the userspace tools that motivate this driver share
the bus with the SPDs.  Read the SPD through the driver instead::

    $ cat /sys/bus/i2c/drivers/ee1004/6-0050/eeprom | hexdump -C

or, if an address really is needed raw, release it::

    # echo 0x50 > /sys/bus/i2c/devices/i2c-6/delete_device

Module parameters
-----------------

allow_unsafe_access
  Bool, read-only after load, defaults to false.  The driver does not bind
  unless this is set.  See the section above.
