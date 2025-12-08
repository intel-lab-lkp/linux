.. SPDX-License-Identifier: GPL-2.0-or-later

=======
KHO FDT
=======

Kexec Handover ABI
==================

KHO uses the FDT to pass data between kernels. The exact structure of
this FDT is a stable contract between kernels and is documented
directly in the ABI header file.

.. kernel-doc:: include/linux/kho/abi/kexec_handover.h
   :doc: Kexec Handover ABI

Kexec Handover ABI for vmalloc Preservation
===========================================

.. kernel-doc:: include/linux/kho/abi/kexec_handover.h
   :doc: Kexec Handover ABI for vmalloc Preservation

Keep track of memory that is to be preserved across KHO
=======================================================

.. kernel-doc:: include/linux/kho/abi/kexec_handover.h
   :doc: Keep track of memory that is to be preserved across KHO.

See Also
========

- :doc:`/admin-guide/mm/kho`
- :doc:`/core-api/kho/concepts`
- :doc:`/core-api/kho/radix_tree`
