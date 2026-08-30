.. SPDX-License-Identifier: GPL-2.0

Kernel driver starfive-fan-tach
===============================

Supported chips:

  * StarFive JHB100

    Prefix: 'starfive_fan_tach'

    Addresses scanned: -

Authors:
      - Changhuang Liang <changhuang.liang@starfivetech.com>

Description
-----------

This driver implements support for the fan tachometer controller found on
the StarFive JHB100 SoC. The controller supports up to 16 independent fan
tachometer inputs. Each tachometer channel measures the number of pulses
within a fixed 100 ms window.

Sysfs entries
-------------

===================== =======================================================
fan[1-16]_input       Fan speed in RPM (read-only), without a fan connected,
                      reading this value can be significantly slow.
fan[1-16]_min         Lower fan speed limit in RPM (read/write)
fan[1-16]_enable      Enable/disable the tachometer channel (read/write)
fan[1-16]_fault       Fan stall indication (read-only), with the fan running
                      normally, reading this value can be significantly slow.
fan[1-16]_min_alarm   Fan speed below fan[1-16]_min (read-only), when
                      fan[1-16]_input is greater than or equal fan[1-16]_min,
                      reading this value can be significantly slow.
===================== =======================================================
