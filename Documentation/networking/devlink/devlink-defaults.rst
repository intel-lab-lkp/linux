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

  devlink=<default>[,<default>...]

Each default has the following form::

  [<handle-list>]:<cmd>:<cmd-options>

``<handle-list>`` is one or more devlink handles::

  <bus-name>/<dev-name>[,<bus-name>/<dev-name>...]

All handles in the same ``[]`` list receive the same command setting.

Multiple defaults may be specified by separating complete defaults with a
comma after the value::

  devlink=[pci/0000:08:00.0]:esw:mode:switchdev,[pci/0000:08:00.1]:esw:mode:legacy

Syntax rules
------------

The following syntax rules apply:

* Specify all defaults in one ``devlink=`` parameter. Repeated ``devlink=``
  parameters are not accumulated.
* The ``devlink=`` value is limited by the kernel command line size.
* Whitespace is not allowed within the parameter value.
* ``<bus-name>`` and ``<dev-name>`` must not be empty.
* ``<bus-name>`` must not contain ``:``.
* ``<dev-name>`` may contain ``:``. This allows PCI names such as
  ``0000:08:00.0``.
* Handles must not contain whitespace, ``[``, ``]`` or more than one ``/``.
* A comma inside ``[]`` separates handles.
* A comma after the ``<value>`` separates defaults.
* Defaults for the same handle are applied in command-line order.
* The same ``esw`` attribute may be specified only once for a given devlink
  handle.
* The same ``param`` name may be specified only once for a given devlink
  handle.
* Duplicate entries for the same handle are rejected and all devlink defaults
  are ignored.
* Parameter names and values must not contain ``:`` or ``,``.

Supported defaults
==================

The supported commands are ``esw`` and ``param``:

.. list-table::
   :widths: 10 25 35
   :header-rows: 1

   * - Command
     - Options
     - Values
   * - ``esw``
     - ``mode:<mode>``
     - ``legacy``, ``switchdev``, ``switchdev_inactive``
   * - ``param``
     - ``<name>:<value>``
     - ``<value>`` is parsed according to the registered devlink parameter
       type. Only runtime devlink parameters are supported.

The ``esw:mode`` default corresponds to the userspace command::

  devlink dev eswitch set <handle> mode <value>

The ``param`` default applies the named devlink parameter in runtime mode.

Examples
========

Set one PCI devlink instance to switchdev mode::

  devlink=[pci/0000:08:00.0]:esw:mode:switchdev

Set two PCI devlink instances to legacy mode::

  devlink=[pci/0000:08:00.0,pci/0000:08:00.1]:esw:mode:legacy

Set a runtime devlink device parameter::

  devlink=[pci/0000:08:00.0]:param:flow_steering_mode:smfs

Set different modes for different PCI devlink instances::

  devlink=[pci/0000:08:00.0]:esw:mode:switchdev,[pci/0000:08:00.1]:esw:mode:switchdev_inactive

The following is invalid because the same handle receives ``esw:mode`` twice::

  devlink=[pci/0000:08:00.0]:esw:mode:legacy,[pci/0000:08:00.0]:esw:mode:switchdev

The following is invalid because the same handle receives ``param:x`` twice::

  devlink=[pci/0]:param:x:1,[pci/0]:param:x:2
