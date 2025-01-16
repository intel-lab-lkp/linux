====================================================
Fujitsu Uncore PCI Performance Monitoring Unit (PMU)
====================================================

This driver supports the Uncore PCI PMUs found in Fujitsu chips.
Each PCI PMU on these chips is exposed as a uncore perf PMU with device name
pci_iod<iod>_pci<pci>.

The driver provides a description of its available events and configuration
options in sysfs, see /sys/bus/event_sources/devices/pci_iod<iod>_pci<pci>/.
This driver exports:
- formats, used by perf user space and other tools to configure events
- events, used by perf user space and other tools to create events
  symbolically, e.g.:
    perf stat -a -e pci_iod0_pci0/event=0x24/ ls
- cpumask, used by perf user space and other tools to know on which CPUs
  to open the events

This driver supports the following events:
- pci-port0-cycles
  This event counts PCI cycles at PCI frequency in port0.
- pci-port0-read-count
  This event counts read transactions for data transfer in port0.
- pci-port0-read-count-bus
  This event counts read transactions for bus usage in port0.
- pci-port0-write-count
  This event counts write transactions for data transfer in port0.
- pci-port0-write-count-bus
  This event counts write transactions for bus usage in port0.
- pci-port1-cycles
  This event counts PCI cycles at PCI frequency in port1.
- pci-port1-read-count
  This event counts read transactions for data transfer in port1.
- pci-port1-read-count-bus
  This event counts read transactions for bus usage in port1.
- pci-port1-write-count
  This event counts write transactions for data transfer in port1.
- pci-port1-write-count-bus
  This event counts write transactions for bus usage in port1.
- ea-pci
  This event counts energy consumption of the PCI.

  'ea' is the abbreviation for 'Energy Analyzer'.

Examples for use with perf::

  perf stat -e pci_iod0_pci0/ea-pci/ ls

Given that these are uncore PMUs the driver does not support sampling, therefore
"perf record" will not work. Per-task perf sessions are not supported.
