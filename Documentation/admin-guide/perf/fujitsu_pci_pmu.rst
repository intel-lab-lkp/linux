===========================================================================
Fujitsu Uncore PCI Performance Monitoring Unit (PMU)
===========================================================================

This driver supports the Uncore PCI PMUs found in Fujitsu chips.
Each PCI PMU on these chips is exposed as a uncore perf PMU with device name
pci_iod<iod>_pci<pci>.

The driver provides a description of its available events and configuration
options in sysfs, see /sys/bus/event_sources/devices/pci_iod<iod>_pci<pci>/.
Given that these are uncore PMUs the driver also exposes a "cpumask" sysfs
attribute which contains a mask consisting of one CPU which will be used to
handle all the PMU events.

Examples for use with perf::

  perf stat -e pci_iod0_pci0/ea-pci/ ls

Given that these are uncore PMUs the driver does not support sampling, therefore
"perf record" will not work. Per-task perf sessions are not supported.
