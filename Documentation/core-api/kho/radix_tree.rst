.. SPDX-License-Identifier: GPL-2.0

====================
KHO Radix Tree
====================

Description
===========

.. kernel-doc:: include/linux/kho_radix_tree.h
   :doc: Kexec Handover Radix Tree

Public API
==========

.. kernel-doc:: kernel/liveupdate/kexec_handover.c
   :identifiers: kho_radix_encode_key kho_radix_decode_key kho_radix_add_page kho_radix_del_page kho_radix_walk_tree
