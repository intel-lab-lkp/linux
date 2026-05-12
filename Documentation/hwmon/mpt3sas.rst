.. SPDX-License-Identifier: GPL-2.0

Kernel driver mpt3sas
=====================

Supported chips:

  * LSI / Broadcom / Avago SAS HBAs handled by the mpt3sas driver,
    such as the 9300, 9400, and 9500 series.

    Prefix: ``mpt3sas``


Description
-----------

The mpt3sas driver exposes the IOC and board temperature sensors of
LSI / Broadcom SAS HBAs through the hwmon interface.
Either or both sensors may be absent depending on the card; the
corresponding sysfs files only appear when the firmware reports the
sensor as present, and cards that report neither sensor do not
register an hwmon device at all.


Sysfs entries
-------------

============  ======================
Name          Description
============  ======================
temp1_input   IOC temperature (mC)
temp1_label   "IOC"
temp2_input   Board temperature (mC)
temp2_label   "Board"
============  ======================


Cross-reference with vendor tooling
-----------------------------------

The hwmon channels correspond to fields reported by Broadcom's
proprietary tools as follows:

=================  ==========================  ===============================
hwmon label        lsiutil                     storcli
=================  ==========================  ===============================
``IOC`` (temp1)    ``IOCTemperature``          ``ROC temperature``
``Board`` (temp2)  ``BoardTemperature``        ``Controller temperature``
=================  ==========================  ===============================

With lsiutil::

    lsiutil -pN -a 25,2,0,0

With storcli::

    storcli /cN show temperature
