.. SPDX-License-Identifier: GPL-2.0

====================
I3C Device Interface
====================

This interface allows access to I3C devices from userspace.

As I3C devices adhere to the I3C protocol, simple transfers can be accomplished
using a generic driver. Currently, this interface supports private Single Data
Rate (SDR) read and write transfers.

The i3cdev module will not auto-bind to devices. Userspace needs to explicitly
bind the device to the driver. This is to avoid interfering with the binding of
specialized drivers.

Once bound, a character device interface will be created at:
/dev/bus/i3c/<bus id>-<Provisional ID>.

====================
Usage
====================

Any discovered I3C devices by the I3C subsystem will have device folders under /sys/bus/i3c/devices/<bus id>-<Provisional ID>.
To allow binding of a device with i3cdev driver, set the driver_override:

::

    # echo "i3cdev" > /sys/bus/i3c/devices/<bus id>-<Provisional ID>/driver_override

If the i3cdev driver is not yet loaded, load it and it will cause the driver to bind
to any devices with the override in place.

If the i3cdev driver is already loaded, go ahead and perform a manual bind:

::

    # echo "<bus id>-<Provisional ID>" > /sys/bus/i3c/drivers/i3cdev/bind

Set driver override
::

    # echo "i3cdev" > /sys/bus/i3c/devices/0-deadbeef001/driver_override
    # echo "i3cdev" > /sys/bus/i3c/devices/0-deadbeef002/driver_override

Bind the device to the driver
::

    # echo "0-deadbeef001" > /sys/bus/i3c/drivers/i3cdev/bind
    # echo "0-deadbeef002" > /sys/bus/i3c/drivers/i3cdev/bind

Observe the resulting character device files under /dev/bus/i3c/
::

    # ls -ltr /dev/bus/i3c/
    crw-------    1 root     root      235,   1 Jun 30 17:49 0-deadbeef002
    crw-------    1 root     root      235,   0 Jun 30 17:49 0-deadbeef001

BASIC CHARACTER DEVICE API
===============================
The API supports private Single Data Rate (SDR) read and write transfers.
Those transaction can be achieved by the following:

``read(file, buffer, sizeof(buffer))``
  The standard read() operation will work as a simple transaction of private
  SDR read data followed a stop.
  Return the number of bytes read on success, and a negative error otherwise.

``write(file, buffer, sizeof(buffer))``
  The standard write() operation will work as a simple transaction of private
  SDR write data followed a stop.
  Return the number of bytes written on success, and a negative error otherwise.

``ioctl(file, I3CDEV_XFER, struct i3cdev_xfers *xfers)``
  It combines read/write transactions without a stop in between.
  Return 0 on success, and a negative error otherwise.

C EXAMPLE (PSEUDO CODE)
=======================
You need to open (and get a file descriptor) to /dev/bus/i3c/<bus id>-<Provisional ID>,
do your operations (read, write, ioctl), and then close it.

The following header files should be included in an I3C program::

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <i3c/i3cdev.h>
#include <stdint.h>

These additionally are used by this example::

#include <stdlib.h>
#include <assert.h>

To work with an I3C device, one must call open and get a file descriptor::

	int fd;

	fd = open("/dev/bus/i3c/0-deadbeef001", O_RDWR);
	if (fd < 0)
		exit(EXIT_FAILURE);

Now that the file is open, we can do some operations::

	int ret;

	/* Write function */
	uint8_t  buf[] = {0x00, 0xde, 0xad, 0xbe, 0xef};
	ret = write(fd, buf, 5);
	if (ret != 5) {
		/* ERROR HANDLING: I3C transaction failed */
	}

	/*  Read function */
	ret = read(fd, buf, 4);
	if (ret < 0) {
		/* ERROR HANDLING: I3C transaction failed */
	} else {
		/* Iterate over buf[] to get the read data */
	}

	/* IOCTL function */
	struct i3cdev_xfer xfers[2] = {0}; /* Must zero out for compatibility */
	struct i3cdev_xfers xfers_metadata = { .nxfers = 2,
					       .xfers = (uintptr_t) xfers,
					       .xfer_size = sizeof(struct i3cdev_xfer)
					     };

	uint8_t tx_buf[] = {0x00, 0xde, 0xad, 0xbe, 0xef};
	uint8_t rx_buf[10];

	xfers[0].data = (uintptr_t) tx_buf;
	xfers[0].len = 5;
	xfers[0].rnw = 0;
	xfers[1].data = (uintptr_t) rx_buf;
	xfers[1].len = 10;
	xfers[1].rnw = 1;

	ret = ioctl(fd, I3CDEV_XFER, (uintptr_t) &xfers_metadata);
	if (ret < 0) {
		/* ERROR HANDLING: I3C transaction failed */
	} else {
		/* For reads, optionally verify that the response matches expectations */
		assert(xfers[1].len == xfers[1].actual_len);

		/* For reads, iterate through response data using xfers[i].actual_len */
	}

The device can be closed when the open file descriptor is no longer required::

	close(fd);