.. SPDX-License-Identifier: GPL-2.0

========================
mxl862xx devlink support
========================

This document describes the devlink features implemented by the
``mxl862xx`` device driver.

Info versions
=============

The ``mxl862xx`` driver reports the following versions

.. list-table:: devlink info versions implemented
   :widths: 5 5 5 85

   * - Name
     - Type
     - Example
     - Description
   * - ``asic.id``
     - fixed
     - 8628
     - The chip part number read from the CHIP ID registers. Not
       reported for a switch sitting in MCUboot rescue mode as the
       registers are only accessible with a running firmware.
   * - ``asic.rev``
     - fixed
     - 0
     - The chip version read from the CHIP ID registers. Not reported
       in MCUboot rescue mode either.
   * - ``fw``
     - running, stored
     - 1.0.70
     - Version of the firmware running on the switch, reported as both
       running and stored since the switch boots it from its own flash.
       In MCUboot rescue mode nothing is reported while an interrupted
       download is still being recovered in the background; once the
       loader is ready to accept a new image the version is reported (as
       both running and stored), which is the signal that a flash will be
       accepted. It reads "0.0.0" when the switch came up straight into
       MCUboot without ever running firmware.

Flash update
============

The ``mxl862xx`` driver implements support for ``devlink dev flash``.
The signed firmware image is transferred to the switch over the same
MDIO bus which is also used to manage the switch, then verified and
installed by the MCUboot bootloader running on the switch. All ports
of the switch are closed for the duration of the update and the driver
reprobes the switch after it has rebooted into the new firmware. A
complete flash and reprobe cycle takes about one minute.

A switch stuck in MCUboot rescue mode, e.g. after an interrupted
update, is registered without user ports. If the previous download was
interrupted mid-transfer the loader is wedged; the driver drains it
back to a clean ready state in the background, which can easily take
more than 10 minutes. During that recovery ``devlink dev info`` reports
no firmware version and ``devlink dev flash`` returns ``-EBUSY``.
Once the loader is ready the firmware version appears and flashing a
firmware image through the regular update flow recovers the switch.
