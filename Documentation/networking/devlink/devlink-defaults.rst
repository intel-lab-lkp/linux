.. SPDX-License-Identifier: GPL-2.0

================
Devlink Defaults
================

Devlink defaults allow selected devlink settings to be provided on the
kernel command line and applied to matching devlink instances during device
initialization.

The devlink device is selected by its devlink handle. For PCI devices this is
the same handle shown by ``devlink dev show``, for example
``pci/0000:08:00.0``.

Kernel command line syntax
==========================

Defaults are specified with the ``devlink=`` kernel command line parameter.

The general syntax is::

  devlink=[<selector>]:esw:mode:<mode>

``<selector>`` is either ``*`` or one or more devlink handles::

  * | <bus-name>/<dev-name>[,<bus-name>/<dev-name>...]

``*`` applies the default to every devlink instance. All handles in the same
``[]`` list receive the same eswitch mode setting.

``<mode>`` is one of ``legacy``, ``switchdev`` or ``switchdev_inactive``.

Syntax rules
------------

The following syntax rules apply:

* Specify the default in one ``devlink=`` parameter. Repeated ``devlink=``
  parameters are not accumulated.
* The ``devlink=`` value is limited by the kernel command line size.
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
* Duplicate handles are rejected and the devlink default is ignored.

Supported defaults
==================

The supported command is ``esw``:

.. list-table::
   :widths: 10 25 35
   :header-rows: 1

   * - Command
     - Options
     - Values
   * - ``esw``
     - ``mode:<mode>``
     - ``legacy``, ``switchdev``, ``switchdev_inactive``

The ``esw:mode`` default corresponds to the userspace command::

  devlink dev eswitch set <handle> mode <value>


Examples
========

Set all devlink instances to switchdev mode::

  devlink=[*]:esw:mode:switchdev

Set one PCI devlink instance to switchdev mode::

  devlink=[pci/0000:08:00.0]:esw:mode:switchdev

Set two PCI devlink instances to legacy mode::

  devlink=[pci/0000:08:00.0,pci/0000:09:00.1]:esw:mode:legacy

The following is invalid because comma-separated default groups are not
supported::

  devlink=[pci/0000:08:00.0]:esw:mode:switchdev,[pci/0000:09:00.0]:esw:mode:switchdev_inactive
