.. SPDX-License-Identifier: GPL-2.0

=======================================
Configuring I3C Target Using CONFIGFS
=======================================

:Author: Frank Li <Frank.Li@nxp.com>

The I3C Target Core exposes configfs entry (i3c_target) to configure the I3C
target function and to bind the target function with the target controller.
(For introducing other mechanisms to configure the I3C Target Function refer to
[1]).

Mounting configfs
=================

The I3C Target Core layer creates i3c_target directory in the mounted configfs
directory. configfs can be mounted using the following command::

	mount -t configfs none /sys/kernel/config

Directory Structure
===================

The i3c_target configfs has two directories at its root: controllers and
functions. Every Controller device present in the system will have an entry in
the *controllers* directory and every Function driver present in the system will
have an entry in the *functions* directory.
::

	/sys/kernel/config/i3c_target/
		.. controllers/
		.. functions/

Creating Function Device
===================

Every registered Function driver will be listed in controllers directory. The
entries corresponding to Function driver will be created by the Function core.
::

	/sys/kernel/config/i3c_target/functions/
		.. <Function Driver1>/
			... <Function Device 11>/
			... <Function Device 21>/
			... <Function Device 31>/
		.. <Function Driver2>/
			... <Function Device 12>/
			... <Function Device 22>/

In order to create a <Function device> of the type probed by <Function Driver>,
the user has to create a directory inside <Function DriverN>.

Every <Function device> directory consists of the following entries that can be
used to configure the standard configuration header of the target function.
(These entries are created by the framework when any new <Function Device> is
created)
::

		.. <Function Driver1>/
			... <Function Device 11>/
				... vendor_id
				... part_id
				... bcr
				... dcr
				... ext_id
				... instance_id
				... max_read_len
				... max_write_len
				... vendor_info

Controller Device
==========

Every registered Controller device will be listed in controllers directory. The
entries corresponding to Controller device will be created by the Controller
core.
::

	/sys/kernel/config/i3c_target/controllers/
		.. <Controller Device1>/
			... <Symlink Function Device11>/
		.. <Controller Device2>/
			... <Symlink Function Device21>/

The <Controller Device> directory will have a list of symbolic links to
<Function Device>. These symbolic links should be created by the user to
represent the functions present in the target device. Only <Function Device>
that represents a physical function can be linked to a Controller device.

::

			 | controllers/
				| <Directory: Controller name>/
					| <Symbolic Link: Function>
			 | functions/
				| <Directory: Function driver>/
					| <Directory: Function device>/
						| vendor_id
						| part_id
						| bcr
						| dcr
						| ext_id
						| instance_id
						| max_read_len
						| max_write_len
						| vendor_info

[1] Documentation/I3C/target/pci-target.rst
