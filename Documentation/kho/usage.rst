.. SPDX-License-Identifier: GPL-2.0-or-later

====================
Kexec Handover Usage
====================

Kexec HandOver (KHO) is a mechanism that allows Linux to preserve state -
arbitrary properties as well as memory locations - across kexec.

This document expects that you are familiar with the base KHO
:ref:`Documentation/kho/concepts.rst <concepts>`. If you have not read
them yet, please do so now.

Prerequisites
-------------

KHO is available when the ``CONFIG_KEXEC_KHO`` config option is set to y
at compile time. Every KHO producer has its own config option that you
need to enable if you would like to preserve their respective state across
kexec.

To use KHO, please boot the kernel with the ``kho_scratch`` command
line parameter set to allocate a scratch region. For example
``kho_scratch=512M`` will reserve a 512 MiB scratch region on boot.

Perform a KHO kexec
-------------------

Before you can perform a KHO kexec, you need to move the system into the
:ref:`Documentation/kho/concepts.rst <KHO active phase>` ::

  $ echo 1 > /sys/kernel/kho/active

After this command, the KHO device tree is available in ``/sys/kernel/kho/dt``.

Next, load the target payload and kexec into it. It is important that you
use the ``-s`` parameter to use the in-kernel kexec file loader, as user
space kexec tooling currently has no support for KHO with the user space
based file loader ::

  # kexec -l Image --initrd=initrd -s
  # kexec -e

The new kernel will boot up and contain some of the previous kernel's state.

For example, if you enabled ``CONFIG_FTRACE_KHO``, the new kernel will contain
the old kernel's trace buffers in ``/sys/kernel/debug/tracing/trace``.

Abort a KHO exec
----------------

You can move the system out of KHO active phase again by calling ::

  $ echo 1 > /sys/kernel/kho/active

After this command, the KHO device tree is no longer available in
``/sys/kernel/kho/dt``.
