.. SPDX-License-Identifier: GPL-2.0

===================================================
Sample pseudo-filesystem built on top of ``kernfs``
===================================================

This directory contains a kernel module that implements a pseudo-filesystem
built on top of ``kernfs`` and it demonstrates the basic of how to use ``kernfs``.

Usage
=====

Compile your kernel with ``CONFIG_SAMPLE_KERNFS=y`` and create a
``sample_kernfs`` mount with::

  # mkdir /sample_kernfs
  # mount -t sample_kernfs none /sample_kernfs

Filesystem layout
=================

The filesystem contains a tree of counters. Here is an example, where
``sample_kernfs`` is mounted at ``/sample_kernfs``::

  /sample_kernfs
  ├── counter
  ├── inc
  ├── sub1/
  │   ├── counter
  │   └── inc
  └── sub2/
      ├── counter
      ├── inc
      ├── sub3/
      │   ├── counter
      │   └── inc
      └── sub4/
          ├── counter
          └── inc

When a directory is created, it is automatically populated with two files:
``counter`` and ``inc``. ``counter`` reports the current count for that node,
and every time it is read it increments by the value in ``inc``. ``counter`` can
be reset to a given value by writing that value to the ``counter`` file::

    $ cat counter
    1
    $ cat counter
    2
    $ echo 4 > counter
    $ cat counter
    5
    $ echo 3 > inc
    $ cat counter
    8
