.. SPDX-License-Identifier: GPL-2.0+

Kernel driver witrn
====================

Supported chips:

  * WITRN K2

    Prefix: 'witrn'

    Addresses scanned: -

Author:

  - Rong Zhang <i@rong.moe>

Description
-----------

This driver implements support for the WITRN USB tester family.

The device communicates with the custom protocol over USB HID.

As current can flow in both directions through the tester the sign of the
channel "curr1_input" (label "IBUS") indicates the direction.

Sysfs entries
-------------

  ===============  ==========  ==============================================================
  Name             Label       Description
  ===============  ==========  ==============================================================
  in0_input        VBUS        Measured VBUS voltage (mV)
  in0_average      VBUS        Calculated average VBUS voltage (mV)
  in1_input        D+          Measured D+ voltage (mV)
  in2_input        D-          Measured D- voltage (mV)
  in3_input        CC1         Measured CC1 voltage (mV)
  in4_input        CC2         Measured CC2 voltage (mV)
  cur1_input       IBUS        Measured VBUS current (mA)
  curr1_average    IBUS        Calculated average VBUS current (mA)
  curr1_rated_min  IBUS        Stop accumulating (recording) below this VBUS current (mA)
  power1_input     PBUS        Calculated VBUS power (uW)
  power1_average   PBUS        Calculated average VBUS power (uW)
  energy1_input    EBUS        Accumulated VBUS energy (uJ)
  charge1_input    CBUS        Accumulated VBUS charge (mC)
  temp1_input      Thermistor  Measured thermistor temperature (m°C), -EXDEV if not connected
  record_group                 ID of the record group for accumulative values
  record_time                  Accumulated time for recording (s), see also curr1_rated_min
  uptime                       Accumulated time since the device has been powered on (s)
  ===============  ==========  ==============================================================

All entries are readonly.
