.. SPDX-License-Identifier: GPL-2.0+

======================================
execve(2) internals and Binary Formats
======================================

Overview
========
To perform execve(), the kernel loads the header of a file from disk,
searches through all binary handlers to find a match, and then builds a
new process memory layout with the resulting binfmt, before transferring
userspace execution control to it.

ELF PIE Handling Notes
======================
.. kernel-doc:: fs/binfmt_elf.c
   :doc: PIE handling

brk handling
============
.. kernel-doc:: fs/binfmt_elf.c
   :doc: "brk" handling

Functions and structures
========================
.. kernel-doc:: fs/exec.c
   :identifiers:

.. kernel-doc:: fs/binfmt_elf.c
   :identifiers:
