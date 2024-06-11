.. SPDX-License-Identifier: (GPL-2.0+ OR MIT)

======================
drm/intel GuC firmware
======================

The graphics microcontroller (GuC) is available starting from Gen9 hardware.

GuC ABI
=======

.. kernel-doc:: drivers/gpu/drm/intel/guc/abi/guc_communication_mmio_abi.h
.. kernel-doc:: drivers/gpu/drm/intel/guc/abi/guc_communication_ctb_abi.h
.. kernel-doc:: drivers/gpu/drm/intel/guc/abi/guc_messages_abi.h
.. kernel-doc:: drivers/gpu/drm/i915/gt/uc/abi/guc_actions_abi.h
.. kernel-doc:: drivers/gpu/drm/intel/guc/abi/guc_actions_sriov_abi.h
.. kernel-doc:: drivers/gpu/drm/intel/guc/abi/guc_klvs_abi.h

GuC Relay ABI
=============

.. kernel-doc:: drivers/gpu/drm/intel/guc/abi/guc_relay_communication_abi.h
.. kernel-doc:: drivers/gpu/drm/intel/guc/abi/guc_relay_actions_abi.h
