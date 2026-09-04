.. SPDX-License-Identifier: GPL-2.0

====================
idpf devlink support
====================

This document describes the devlink features implemented by the ``idpf``
device driver.

Info versions
=============

The ``idpf`` driver reports the following versions

.. list-table:: devlink info versions implemented
    :widths: 5 5 5 90

    * - Name
      - Type
      - Example
      - Description
    * - ``fw.mgmt.api``
      - running
      - 2.0
      - 2-digit version number (major.minor) of the communication channel
        (virtchnl) used by the device.

The driver also reports the PCI Device Serial Number through the
``serial_number`` attribute, on devices that implement the PCI Device Serial
Number extended capability.
