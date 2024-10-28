.. SPDX-License-Identifier: GPL-2.0

.. _per_console_loglevel:

Per-console loglevel support
============================

Motivation
----------

Consoles can have vastly different latencies and throughputs. For example,
writing a message to the serial console can take on the order of tens of
milliseconds to get the UART to successfully write a message. While this might
be fine for a single, one-off message, this can cause significant
application-level stalls in situations where the kernel writes large amounts of
information to the console.

This means that while you might want to send at least INFO level messages to
(for example) netconsole, which is relatively fast, you may only want to send
at least WARN level messages to the serial console. This permits debugging
using the serial console in cases that netconsole doesn't receive messages
during particularly bad system issues, while still keeping the noise low enough
to avoid inducing latency in userspace applications.

Loglevel
--------

Kernel loglevels are defined thus:

+---+--------------+-----------------------------------+
| 0 | KERN_EMERG   | system is unusable                |
+---+--------------+-----------------------------------+
| 1 | KERN_ALERT   | action must be taken immediately  |
+---+--------------+-----------------------------------+
| 2 | KERN_CRIT    | critical conditions               |
+---+--------------+-----------------------------------+
| 3 | KERN_ERR     | error conditions                  |
+---+--------------+-----------------------------------+
| 4 | KERN_WARNING | warning conditions                |
+---+--------------+-----------------------------------+
| 5 | KERN_NOTICE  | normal but significant condition  |
+---+--------------+-----------------------------------+
| 6 | KERN_INFO    | informational                     |
+---+--------------+-----------------------------------+
| 7 | KERN_DEBUG   | debug-level messages              |
+---+--------------+-----------------------------------+

Tunables
--------

In order to allow tuning per-console loglevels, the following controls exist:

Global
~~~~~~

The global loglevel is set by the ``kernel.console_loglevel`` sysctl, which can
also be set as ``loglevel=`` on the kernel command line.

The printk module also takes two parameters which modify this behaviour
further:

* ``ignore_loglevel`` on the kernel command line or set in printk parameters:
  Emit all messages. All other controls are ignored if this is present.

* ``ignore_per_console_loglevel`` on the kernel command line or set in printk
  parameters: Ignore all per-console loglevels and use the global loglevel.

The default value for ``kernel.console_loglevel`` comes from
``CONFIG_CONSOLE_LOGLEVEL_DEFAULT``, or ``CONFIG_CONSOLE_LOGLEVEL_QUIET`` if
``quiet`` is passed on the kernel command line.

Console attributes
~~~~~~~~~~~~~~~~~~

Registered consoles are exposed at ``/sys/class/console``. For example, if you
are using ``ttyS0``, the console backing it can be viewed at
``/sys/class/console/ttyS0/``. The following files are available:

* ``effective_loglevel`` (r): The effective loglevel after considering all
  loglevel authorities. For example, it shows the value of the console-specific
  loglevel when a console-specific loglevel is defined, and shows the global
  console loglevel value when the console-specific one is not defined.

* ``effective_loglevel_source`` (r): The loglevel authority which resulted in
  the effective loglevel being set. The following values can be present:

    * ``local``: The console-specific loglevel is in effect.

    * ``global``: The global loglevel (``kernel.console_loglevel``) is in
      effect. Set a console-specific loglevel to override it.

    * ``ignore_loglevel``: ``ignore_loglevel`` was specified on the kernel
      command line or at ``/sys/module/printk/parameters/ignore_loglevel``.
      Disable it to use level controls.

* ``enabled`` (r): Whether the console is enabled.

* ``loglevel`` (rw): The local, console-specific loglevel for this console.
  This will be in effect if no other global control overrides it. Look at
  ``effective_loglevel`` and ``effective_loglevel_source`` to verify that.

Deprecated
~~~~~~~~~~

* ``kernel.printk`` sysctl: this takes four values, setting
  ``kernel.console_loglevel``, ``kernel.default_message_loglevel``, the minimum
  console loglevel, and a fourth unused value. The interface is generally
  considered to quite confusing, doesn't perform checks on the values given,
  and is unaware of per-console loglevel semantics.

Chris Down <chris@chrisdown.name>, 13-October-2024
