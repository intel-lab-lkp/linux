====================================================
Fujitsu Uncore MAC Performance Monitoring Unit (PMU)
====================================================

This driver supports the Uncore MAC PMUs found in Fujitsu chips.
Each MAC PMU on these chips is exposed as a uncore perf PMU with device name
mac_iod<iod>_mac<mac>_ch<ch>.

The driver provides a description of its available events and configuration
options in sysfs, see /sys/bus/event_sources/devices/mac_iod<iod>_mac<mac>_ch<ch>/.
This driver exports:
- formats, used by perf user space and other tools to configure events
- events, used by perf user space and other tools to create events
  symbolically, e.g.:
    perf stat -a -e mac_iod0_mac0_ch0/event=0x21/ ls
- cpumask, used by perf user space and other tools to know on which CPUs
  to open the events

This driver supports the following events:
- cycles
  This event counts MAC cycles at MAC frequency.
- read-count
  This event counts the number of read requests to MAC.
- read-count-request
  This event counts the number of read requests including retry to MAC.
- read-count-return
  This event counts the number of read requests to MAC.
- read-count-request-pftgt
  This event counts the number of read requests including retry with PFTGT
  flag.
- read-count-request-normal
  This event counts the number of read requests including retry without PFTGT
  flag.
- read-count-return-pftgt-hit
  This event counts the number of read requests which hit the PFTGT buffer.
- read-count-return-pftgt-miss
  This event counts the number of read requests which miss the PFTGT buffer.
- read-wait
  This event counts outstanding read requests issued by DDR memory controller
  per cycle.
- write-count
  This event counts the number of write requests to MAC (including zero write,
  full write, partial write, write cancel).
- write-count-write
  This event counts the number of full write requests to MAC (not including
  zero write).
- write-count-pwrite
  This event counts the number of partial write requests to MAC.
- memory-read-count
  This event counts the number of read requests from MAC to memory.
- memory-write-count
  This event counts the number of full write requests from MAC to memory.
- memory-pwrite-count
  This event counts the number of partial write requests from MAC to memory.
- ea-mac
  This event counts energy consumption of the MAC.
- ea-memory
  This event counts energy consumption of the memory.
- ea-memory-mac-read
  This event counts the number of read requests from MAC to memory.
- ea-memory-mac-write
  This event counts the number of write requests from MAC to memory.
- ea-memory-mac-pwrite
  This event counts the number of partial write requests from MAC to memory.
- ea-ha
  This event counts energy consumption of the HA.

  'ea' is the abbreviation for 'Energy Analyzer'.

Examples for use with perf::

  perf stat -e mac_iod0_mac0_ch0/ea-mac/ ls

Given that these are uncore PMUs the driver does not support sampling, therefore
"perf record" will not work. Per-task perf sessions are not supported.
