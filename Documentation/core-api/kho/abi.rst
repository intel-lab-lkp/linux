.. SPDX-License-Identifier: GPL-2.0-or-later

==================
Kexec Handover ABI
==================

Core Kexec Handover ABI
========================

.. kernel-doc:: include/linux/kho/abi/kexec_handover.h
   :doc: Kexec Handover ABI

vmalloc preservation ABI
========================

.. kernel-doc:: include/linux/kho/abi/vmalloc.h

memblock preservation ABI
=========================

.. kernel-doc:: include/linux/kho/abi/memblock.h
   :doc: memblock kexec handover ABI

KHO persistent memory tracker ABI
=================================

.. kernel-doc:: include/linux/kho/abi/radix_tree.h

KHO serialization block ABI
===========================

.. kernel-doc:: include/linux/kho/abi/block.h

See Also
========

- :doc:`/admin-guide/mm/kho`
