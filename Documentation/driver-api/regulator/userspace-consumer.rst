============================
Regulator Userspace Consumer
============================

This document describes the Userspace Consumer Regulator Driver
(`drivers/regulator/userspace-consumer.c`) and its userspace interface.

Introduction
------------

The Userspace Consumer Regulator provides userspace with direct control
and monitoring of regulator outputs. It now supports reporting regulator
events directly via uevents, enabling userspace to handle events such as
overcurrent, voltage changes, enabling/disabling regulators, and more.

Supported Events
----------------

The driver emits uevents corresponding to the regulator events defined in
``include/uapi/regulator/regulator.h``.

Currently supported regulator event uevents are:

- ``EVENT=ABORT_DISABLE``
- ``EVENT=ABORT_VOLTAGE_CHANGE``
- ``EVENT=DISABLE``
- ``EVENT=ENABLE``
- ``EVENT=FAIL``
- ``EVENT=FORCE_DISABLE``
- ``EVENT=OVER_CURRENT``
- ``EVENT=OVER_TEMP``
- ``EVENT=PRE_DISABLE``
- ``EVENT=PRE_VOLTAGE_CHANGE``
- ``EVENT=REGULATION_OUT``
- ``EVENT=UNDER_VOLTAGE``
- ``EVENT=VOLTAGE_CHANGE``

Monitoring Events from Userspace
--------------------------------

Userspace applications can monitor these regulator uevents using ``udevadm``:

.. code-block:: bash

   udevadm monitor -pku

Example output:

.. code-block::

    KERNEL[152.717414] change   /devices/platform/output-usb3 (platform)
    ACTION=change
    DEVPATH=/devices/platform/output-usb3
    SUBSYSTEM=platform
    NAME=event
    EVENT=OVER_CURRENT
    DRIVER=reg-userspace-consumer

Handling Events with Udev Rules
-------------------------------

Userspace can react to these events by creating udev rules. For example,
to trigger a script on an OVER_CURRENT event:

.. code-block:: udev

    # /etc/udev/rules.d/99-regulator-events.rules
    ACTION=="change", SUBSYSTEM=="platform", DRIVER="reg-userspace-consumer", ENV{EVENT}=="OVER_CURRENT", RUN+="/usr/local/bin/handle-regulator-event.sh"

A sample handler script:

.. code-block:: bash

    #!/bin/sh
    logger "Handle regulator ${EVENT} on ${DEVPATH}"
    # Add additional handling logic here
    case "${EVENT}" in
    OVER_CURRENT)
      echo disabled > /sys"${DEVPATH}"/state
    esac

API Stability
-------------

This interface is considered stable. New regulator events may be added
in the future, with corresponding documentation updates.

References
----------

- Kernel Header File: ``include/uapi/regulator/regulator.h``
- Driver Source: ``drivers/regulator/userspace-consumer.c``
