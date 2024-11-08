===========================================================================
Fujitsu Uncore MAC Performance Monitoring Unit (PMU)
===========================================================================

This driver supports the Uncore MAC PMUs found in Fujitsu chips.
Each MAC PMU on these chips is exposed as a uncore perf PMU with device name
mac_iod<iod>_mac<mac>_ch<ch>.

The driver provides a description of its available events and configuration
options in sysfs, see /sys/bus/event_sources/devices/mac_iod<iod>_mac<mac>_ch<ch>/.
Given that these are uncore PMUs the driver also exposes a "cpumask" sysfs
attribute which contains a mask consisting of one CPU which will be used to
handle all the PMU events.

Examples for use with perf::

  perf stat -e mac_iod0_mac0_ch0/ea-mac/ ls

Given that these are uncore PMUs the driver does not support sampling, therefore
"perf record" will not work. Per-task perf sessions are not supported.
