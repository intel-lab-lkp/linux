.. SPDX-License-Identifier: GPL-2.0

Kernel driver mpm369x
====================

Supported chips:

  * MPS mpm3695-20

    Prefix: 'mpm3695-20'

  * MPS mpm3690S-15

    Prefix: 'mpm3690S-15'

Author:

	Yuxi Wang <Yuxi.Wang@monolithicpower.com>

Description
-----------

This driver implements support for Monolithic Power Systems, Inc. (MPS)
MPM3695-20 and MPM3690S-15 Controller.

Device compliant with:

- PMBus rev 1.3 interface.

The driver exports the following attributes via the 'sysfs' files
for input voltage:

**in1_input**

**in1_label**

**in1_crit**

**in1_crit_alarm**

The driver provides the following attributes for output voltage:

**in2_input**

**in2_label**

**in2_lcrit**

**in2_lcrit_alarm**

**in2_rated_max**

**in2_rated_min**

The driver provides the following attributes for output current:

**curr1_input**

**curr1_label**

**curr1_max**

**curr1_max_alarm**

The driver provides the following attributes for temperature:

**temp1_input**

**temp1_crit**

**temp1_crit_alarm**

**temp1_max**

**temp1_max_alarm**
