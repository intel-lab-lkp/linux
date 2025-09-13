==========================================
Using the RAM disk block device with Linux
==========================================

.. Contents:

	1) Overview
	2) Module parameters


1) Overview
-----------

The RAM disk driver is a way to use main system memory as a block device.
It can also be used for a temporary filesystem for crypto work, since the contents
are erased on reboot.

The RAM disk dynamically grows as more space is required. It does this by using
RAM from the buffer cache. The driver marks the buffers it is using as dirty
so that the VM subsystem does not try to reclaim them later.

The RAM disk supports up to 16 RAM disks by default, and can be reconfigured
to support an unlimited number of RAM disks (at your own risk).  Just change
the configuration symbol BLK_DEV_RAM_COUNT in the Block drivers config menu
and (re)build the kernel.

To use RAM disk support with your system, run './MAKEDEV ram' from the /dev
directory.  RAM disks are all major number 1, and start with minor number 0
for /dev/ram0, etc.


2) Module parameters
--------------------

	rd_size=N
		Size of the ramdisk.

This parameter tells the RAM disk driver to set up RAM disks of N k size.  The
default is 4096 (4 MB).

	rd_nr
		/dev/ramX devices created.

	max_part
		Maximum partition number.


						Paul Gortmaker 12/95

Changelog:
----------

SEPT-2020 :

                Removed usage of "rdev"

10-22-04 :
		Updated to reflect changes in command line options, remove
		obsolete references, general cleanup.
		James Nelson (james4765@gmail.com)

12-95 :
		Original Document
