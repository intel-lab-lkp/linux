.. SPDX-License-Identifier: GPL-2.0

=========================
octeontx2 devlink support
=========================

This document describes the devlink features implemented by the ``octeontx2 CPT``
device drivers.

Parameters
==========

The ``octeontx2`` driver implements the following driver-specific parameters.

.. list-table:: Driver-specific parameters implemented
   :widths: 5 5 5 85

   * - Name
     - Type
     - Mode
     - Description
   * - ``max_rxc_icb_cnt``
     - u16
     - runtime
     - Configures maximum icb entries that HW can use in RX path.
