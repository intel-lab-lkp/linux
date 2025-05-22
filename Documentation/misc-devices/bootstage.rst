.. SPDX-License-Identifier: GPL-2.0

================
Bootstage driver
================

The bootstage driver exports interfaces to read from a bootstage stash area
saved by a bootloader (e.g.: U-Boot) that ran before the Linux kernel.

Two kind of interfaces are exported:

- a sysfs interface for bootloader- and platform-agnostic data
- a debugfs interface for bootloader- and platform-specific data


The sysfs interface
-------------------

Following sysfs attributes can be found at /sys/devices/platform/<device-name>/:

- start_time_us: bootloader start time in microseconds
- end_time_us: bootloader end time in microseconds


The debugfs interface
---------------------

Following debugfs interfaces can be found at
/sys/kernel/debug/bootstage/<device-name>/:

- stages: details on staged bootloader stages, with start time and duration.
  Example output::

    Mark (us)  Elapsed (us)  Stage
            0             0  reset
       183689        183689  SPL
       489247        305558  end phase
       506987         17740  board_init_f
      1257880        750893  board_init_r
      1622303        364423  eth_common_init
      1888033        265730  eth_initialize
      1893077          5044  main_loop
      4204282       2311205  cli_loop

- accumulated_time: time accumulated during certain bootloader stages.
  Example output::

    Time (us)  Stage
         4902  dm_spl
       322719  dm_f
         9527  dm_r

The number and type of staged stages are bootloader- and platform-specific.
