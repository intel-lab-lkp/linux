.. SPDX-License-Identifier: GPL-2.0

===================
ena devlink support
===================

This document describes the devlink features implemented by the ``ena``
device driver.

Parameters
==========

The ``ena`` driver implements the following driver-specific parameters.

.. list-table:: Driver-specific parameters implemented
   :widths: 5 5 5 85

   * - Name
     - Type
     - Mode
     - Description
   * - ``phc_enable``
     - Boolean
     - driverinit
     - Enables/disables the PHC feature
