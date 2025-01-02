.. SPDX-License-Identifier: GPL-2.0-or-later
======================================================
Lenovo WMI Interface Drivers (lenovo-wmi)
======================================================

Introduction
============
Lenovo WMI interfaces are broken up into multiple GUIDs, some of which
require cross-references between GUIDs for some functionality. The "Custom
Mode" interface is a legacy interface for managing and displaying CPU & GPU
power and hwmon settings and readings. The "Other Method" interface is a
modern interface that replaces most "Custom Mode" interface methods. The
"GameZone" interface adds advanced features such as fan profiles and
overclocking. The "Lighting" interface adds control of various status
lights related to different hardware components.

Each interface has a different data structure associated with it that
provides detailed information about each attribute provided by the
interface. These data structs are retrieved from additional WMI device
data block GUIDs:
 - "Custom Mode" uses LENOVO_FAN_TABLE_DATA, LENOVO_FAN_TEST_DATA,
   LENOVO_CPU_OVERCLOCKING_DATA, LENOVO_DISCRETE_DATA, and
   LENOVO_GPU_OVERCLOCKING_DATA depending on the feature.
 - "Other Method" uses LENOVO_CAPABILITY_DATA_00,
   LENOVO_CAPABILITY_DATA_01, and LENOVO_CAPABILITY_DATA_02 depending on
   the feature.
 - "GameZone" uses LENOVO_GAMEZONE_CPU_OC_DATA and
   LENOVO_GAMEZONE_GPU_OC_DATA depending on the feature.
 - The "Lighting" interface uses LENOVO_LIGHTING_DATA.

.. note::
   Currently only the "GameZone", "Other Method", and
   LENOVO_CAPABILITY_DATA_01 interfaces are implemented by these drivers.

GameZone
--------
WMI GUID "887B54E3-DDDC-4B2C-8B88-68A26A8835D0"

The GameZone WMI interface provides platform-profile and fan curve settings
for devices that fall under the "Gaming Series" of Lenovo devices.

The following platform profiles are supported:
 - quiet
 - balanced
 - performance
 - custom

Custom Profile
~~~~~~~~~~~~~~
The custom profile represents a hardware mode on Lenovo devices that enables
user modifications to Package Power Tracking (PPT) settings. When an
attribute exposed by the "Other Method" WMI interface is to be modified,
the GameZone driver must first be switched to the "custom" profile manually
or the setting will have no effect. If another profile is set from the list
of supported profiles, the BIOS will override any user PPT settings when
switching to that profile.


Other Method
----------
WMI GUID "DC2A8805-3A8C-41BA-A6F7-092E0089CD3B"

The Other Method WMI interface uses the fw_attributes class to expose
various WMI attributes provided by the interface in the sysfs. This enables
CPU and GPU power limit tuning as well as various other attributes for
devices that fall under the "Gaming Series" of Lenovo devices. Each
attribute exposed by the Other Method interface has corresponding
capability data blocks which allow the driver to probe details about the
attribute. Each attibute has multiple pages, one for each of the platform
profiles managed by the "GameZone" interface. For all properties only the
"Custom" profile values are reported by this driver to ensure any userspace
applications reading them have accurate tunable value ranges. Attributes
are exposed in sysfs under the following path:
/sys/class/firmware-attributes/lenovo-wmi-other/attributes

LENOVO_CAPABILITY_DATA_01
~~~~~~~~~~~~~~~~~~~~~~~~~
WMI GUID "7A8F5407-CB67-4D6E-B547-39B3BE018154"

The LENOVO_CAPABILITY_DATA_01 interface provides information on various
power limits of integrated CPU and GPU components.

The following attributes are supported:
 - ppt_pl1_spl: Platform Profile Tracking Sustained Power Limit
 - ppt_pl2_sppt: Platform Profile Tracking Slow Package Power Tracking
 - ppt_pl3_fppt: Platform Profile Tracking Fast Package Power Tracking

Each attribute has the following properties:
 - current_value
 - default_value
 - display_name
 - max_value
 - min_value
 - scalar_increment
 - type


 Camera
 ______
 WMI GUID "50C76F1F-D8E4-D895-0A3D-62F4EA400013"

 The Camera driver provides WMI event notifications for camera button
 toggling.

