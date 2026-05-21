.. SPDX-License-Identifier: GPL-2.0

==============================
Devlink Eswitch Mode Defaults
==============================

Devlink eswitch mode defaults allow the eswitch mode to be provided on the
kernel command line and applied to matching devlink instances during device
initialization.

The devlink device is selected by its devlink handle. For PCI devices this is
the same handle shown by ``devlink dev show``, for example
``pci/0000:08:00.0``.

Kernel command line syntax
==========================

Defaults are specified with the ``devlink_eswitch_mode=`` kernel command line
parameter.

The general syntax is::

  devlink_eswitch_mode=[<selector>]:<mode>

``<selector>`` is either ``*`` or one or more devlink handles::

  * | <bus-name>/<dev-name>[,<bus-name>/<dev-name>...]

``*`` applies the mode to every devlink instance. All handles in the same
``[]`` list receive the same eswitch mode.

``<mode>`` is one of ``legacy``, ``switchdev`` or ``switchdev_inactive``.

Syntax rules
------------

The following syntax rules apply:

* Specify the default in one ``devlink_eswitch_mode=`` parameter. Repeated
  ``devlink_eswitch_mode=`` parameters are not accumulated.
* The ``devlink_eswitch_mode=`` value is limited by the kernel command line
  size.
* Whitespace is not allowed within the parameter value.
* ``<selector>`` must be either ``*`` or a handle list. ``*`` cannot be
  combined with explicit handles.
* ``<bus-name>`` and ``<dev-name>`` must not be empty.
* ``<bus-name>`` must not contain ``:``.
* ``<dev-name>`` may contain ``:``. This allows PCI names such as
  ``0000:08:00.0``.
* Handles must not contain whitespace, ``[``, ``]``, ``*`` or more than one
  ``/``.
* A comma inside ``[]`` separates handles.
* Comma-separated default groups are not supported.
* Duplicate handles are rejected and the devlink eswitch mode default is
  ignored.

The eswitch mode default corresponds to the userspace command::

  devlink dev eswitch set <handle> mode <value>


Examples
========

Set all devlink instances to switchdev mode::

  devlink_eswitch_mode=[*]:switchdev

Set one PCI devlink instance to switchdev mode::

  devlink_eswitch_mode=[pci/0000:08:00.0]:switchdev

Set two PCI devlink instances to legacy mode::

  devlink_eswitch_mode=[pci/0000:08:00.0,pci/0000:09:00.1]:legacy

The following is invalid because comma-separated default groups are not
supported::

  devlink_eswitch_mode=[pci/0000:08:00.0]:switchdev,[pci/0000:09:00.0]:switchdev_inactive
