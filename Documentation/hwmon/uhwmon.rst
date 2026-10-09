.. SPDX-License-Identifier: GPL-2.0-only

Userspace hardware monitoring
=============================

Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>

``uhwmon`` connects userspace drivers to ``hwmon_ops`` through the control
device ``/dev/uhwmon``. The hwmon core creates and formats sysfs attributes.
The daemon supplies values in the units defined by :doc:`sysfs-interface`.

Enable ``CONFIG_HWMON`` and ``CONFIG_SENSORS_UHWMON`` and load ``uhwmon``.
Include ``<linux/uhwmon.h>``.

Register a device
-----------------

Open ``/dev/uhwmon`` with ``O_RDWR`` for each device. Zero-initialize all
UAPI structures.

Call ``UHWMON_CREATE`` with the NUL-terminated chip name, attribute array and
its count in ``struct uhwmon_create``. Each ``struct uhwmon_attribute`` contains:

* ``type``, ``attr``: hwmon enum IDs, not ``HWMON_*`` bitmasks.
* ``channel``: zero-based index; zero for chip attributes.
* ``mode``: nonzero permissions using only bits from 0644.
* ``flags``: zero, or ``UHWMON_ATTR_CONSTANT`` for read-only constants.
* ``value``: a constant number, otherwise zero.
* ``text``: pointer to a constant label, otherwise zero.
* ``size``: NUL-inclusive label capacity from 1 to ``PAGE_SIZE``; zero
  for numbers.

Labels are read-only, NUL-terminated and omit the trailing newline.

Handle requests
---------------

Read a ``struct uhwmon_request`` and write a ``struct uhwmon_reply`` on the
owning file. Request and reply headers are 24 bytes. Only successful label
replies append ``size`` bytes of text. Buffers smaller than the message are
rejected with ``EINVAL``.

``struct uhwmon_request``:

* ``id``: request identifier; copy it into the reply.
* ``attr``: index in the registered attribute array.
* ``op``: ``UHWMON_READ`` or ``UHWMON_WRITE``.
* ``value``: number to apply for a write; zero for a read.

``struct uhwmon_reply``:

* ``id``: the request's identifier.
* ``status``: zero on success or a negative errno; use ``-ENODATA`` for
  unavailable or stale readings. Return ``-EINTR`` only before applying a write.
* ``reserved``: zero.
* ``value``: successful numeric read result; ignored otherwise.
* ``text``: NUL-terminated result of a successful label read; absent otherwise.

Numbers use signed 64-bit fields and must fit the kernel's ``long``,
except for energy64 reads.
Discard replies rejected with ``ESTALE`` (canceled request).

This example registers ``temp1_input`` and ``temp1_enable`` and reports
42000 millidegrees Celsius while enabled:

.. code-block:: c

	struct uhwmon_attribute attrs[] = {
		{ .type = hwmon_temp, .attr = hwmon_temp_input, .mode = 0444 },
		{ .type = hwmon_temp, .attr = hwmon_temp_enable, .mode = 0644 },
	};
	struct uhwmon_create create = {
		.name = (uintptr_t)"example",
		.attrs = (uintptr_t)attrs,
		.num_attrs = 2,
	};
	struct uhwmon_request req;
	struct uhwmon_reply reply = {};
	int enabled = 1;

	if (ioctl(fd, UHWMON_CREATE, &create) < 0)
		return;

	for (;;) {
		if (read(fd, &req, sizeof(req)) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		reply.id = req.id;
		reply.status = -EINVAL;
		if (req.attr == 0 && req.op == UHWMON_READ) {
			reply.value = 42000;
			reply.status = enabled ? 0 : -ENODATA;
		} else if (req.attr == 1 && req.op == UHWMON_READ) {
			reply.value = enabled;
			reply.status = 0;
		} else if (req.attr == 1 && req.op == UHWMON_WRITE &&
			   (req.value == 0 || req.value == 1)) {
			enabled = req.value;
			reply.status = 0;
		}
		if (write(fd, &reply, sizeof(reply)) < 0 && errno != ESTALE)
			break; // Fatal error.
	}

Disconnect and reconnect
------------------------

Sysfs requests can be interrupted or restarted before delivery to the daemon.
After delivery, they wait for a reply or disconnection; only fatal signals
can interrupt this wait. Completed replies take precedence over signals.

Releasing the last control file reference disconnects the daemon: dynamic
reads return ``ENODATA``, writes return ``ENODEV``, and constants remain readable.
Closing an fd does not cancel I/O in another thread; duplicated fds retain
ownership. Interrupt blocked control reads or use ``UHWMON_DESTROY``.

The device persists after disconnection. To reconnect, open a new control fd
and register the same name and attribute array, including constant values.

``UHWMON_DESTROY`` removes the device. Close and reopen the control file
before registering another device.

Notify changes
--------------

The kernel cannot detect changes to values held by the daemon. After changing
a value, call ``UHWMON_NOTIFY`` with its 32-bit attribute index. This wakes
sysfs pollers waiting for ``POLLPRI`` and sends a ``KOBJ_CHANGE`` uevent with
``NAME=<attribute>``. Applications read the attribute again from offset 0 to
get the new value. The notification carries no value and does not replace a
read reply.

For example, after changing ``enabled``, notify listeners of ``temp1_enable``:

.. code-block:: c

	__u32 index = 1; /* attrs[1]: temp1_enable */
	if (ioctl(fd, UHWMON_NOTIFY, &index) < 0)
		return;
