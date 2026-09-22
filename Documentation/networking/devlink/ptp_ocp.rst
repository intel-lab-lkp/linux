.. SPDX-License-Identifier: GPL-2.0

========================
ptp_ocp devlink support
========================

This document describes the devlink features implemented by the ``ptp_ocp``
device driver: the info versions it reports and its flash update support.

Info versions
=============

The ``ptp_ocp`` driver reports the following versions

.. list-table:: devlink info versions implemented
   :widths: 5 5 90

   * - Name
     - Type
     - Description
   * - ``fw``
     - running
     - Version of the firmware running on the card.  Reported as ``loader``
       instead when the card is running the firmware loader.
   * - ``board.id``
     - fixed
     - Board identifier, read from the on-card EEPROM.
   * - ``cpld.id``
     - fixed
     - Lattice device ID (IDCODE) of the TAP CPLD, formatted as ``0x%08x``.
       Only present on ADVA TimeCard X1 boards, which are the only ones
       carrying that CPLD.  Reading it claims the shared I2C bus and
       reprograms the on-card mux, so the driver does that from its own
       worker and reports the cached value here; the version is omitted
       until that read has succeeded.  The IDCODE identifies the silicon,
       so it is not affected by a CPLD update.
   * - ``fw.cpld``
     - running
     - USERCODE of the image programmed into the TAP CPLD, formatted as
       ``0x%08x``.  Read together with ``cpld.id`` and reported the same
       way; it is dropped when an update erases the part and reported
       again once the new image has been read back.  This is the
       component name to pass to ``devlink dev flash`` to update the
       CPLD, and the name is reported even while the value is not, so a
       part left holding a bad image can still be reflashed.

Flash update
============

The driver implements ``devlink dev flash`` for two separate targets,
selected with the component name.

.. list-table:: Flash components
   :widths: 20 80

   * - Component
     - Description
   * - (none)
     - The card's own flash, written through the SPI controller the driver
       exposes.  The card runs the new image after its next reset.
   * - ``fw.cpld``
     - The configuration flash of the TAP CPLD on ADVA TimeCard X1 boards,
       programmed over I2C with the MachXO3 in-system programming commands
       and activated with a REFRESH, so the new image runs immediately.
       The image is the raw configuration bitstream.  The driver checks
       only that its length is a non-zero multiple of the 16-byte page
       size and that it is not larger than the part takes, so a container
       such as ``.jed`` has to be converted first rather than passed
       through - one whose length happens to be a multiple of 16 would be
       programmed as if it were a bitstream.

       The erase clears the configuration flash before the first page is
       written, so any failure from that point on - including an abort on
       a fatal signal - leaves the CPLD unconfigured until a valid image
       is written.  The component stays available for that.

Programming the CPLD claims the shared I2C bus for the whole cycle, so
reads of the card's EEPROM block until it completes.  Progress is reported
with the standard devlink status notifications.

Example::

    $ devlink dev flash pci/0000:02:00.0 file adva-cpld.bin component fw.cpld
