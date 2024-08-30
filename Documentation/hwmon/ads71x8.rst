.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver ads71x8
=====================

Supported chips:

  * Texas Instruments ADS7138

	Prefix: 'ads7128'

	Datasheet: Publicly available at the Texas Instruments website:
	http://focus.ti.com/lit/ds/symlink/ads7128.pdf

  * Texas Instruments ADS7138

	Prefix: 'ads7138'

	Datasheet: Publicly available at the Texas Instruments website:
	http://focus.ti.com/lit/ds/symlink/ads7138.pdf

Author: Tobias Sperling <tobias.sperling@softing.com>
	(based on ads7828 by Steve Hardy)

Description
-----------

This driver implements support for the Texas Instruments ADS7128 and ADS7138,
which are 8-channel 12-bit A/D converters.

The chip requires an external analog supply voltage AVDD which is also used as
reference voltage. If it is missing or too low, the chip won't show up as I2C
device.

The driver can be run in different modes. In manual mode a new (averaged) sample
is created when the according input is read.

In auto mode all channels are sampled sequentially automatically. Reading an
input returns the last valid sample. In this mode there are also further
features like statistics and the possibility to trigger an interrupt if a
voltage drops/raises below/above a specific value (DWC - Digital Window
Comparator).
The overall update time (after which all channels are updated) depends on the
number of samples, the update interval and the amount of channels (8).

	update time = samples * update_interval * 8

There is no reliable way to identify this chip, so the driver will not scan
some addresses to try to auto-detect it. That means that you will have to
statically declare the device in the device tree.

sysfs-Interface
---------------

The following interfaces are available in all modes.

+----------------+----+---------------------------------------------+
| in[0-7]_input  | ro | Voltage in mV sampled at channel [0-7]      |
+----------------+----+---------------------------------------------+
| samples        | rw | Number of samples used for averaging 1-128. |
|                |    | Automatically set to closest power of 2.    |
+----------------+----+---------------------------------------------+
| calibrate      | rw | Write any value greater than 0 to trigger   |
|                |    | self-calibration. Reads as 0 if finished.   |
+----------------+----+---------------------------------------------+

If the device is running in auto mode there are also the following interfaces.

+------------------+----+-----------------------------------------------------+
| in[0-7]_max      | ro | Maximum value in mV that occurred at channel [0-7]  |
+------------------+----+-----------------------------------------------------+
| in[0-7]_min      | ro | Minimal value in mV that occurred at channel [0-7]  |
+------------------+----+-----------------------------------------------------+
| update_interval  | ro | Time in microseconds after which the next sample is |
|                  |    | executed.                                           |
+------------------+----+-----------------------------------------------------+

If the device is running in auto mode and the interrupt is configured also the
following interfaces are added. If CONFIG_SYSFS is set in the kernel
configuration it is also possible to poll the 'alrarms', see example below.

+--------------------+----+---------------------------------------------------+
| alarms             | ro | | Contains the flags of DWC events. Once read it  |
|                    |    |   is reset to 0.                                  |
|                    |    | | BIT0 equals the low event flag of channel 0.    |
|                    |    | | BIT7 equals the low event flag of channel 7.    |
|                    |    | | BIT8 equals the high event flag of channel 0.   |
|                    |    | | BIT15 equals the high event flag of channel 7.  |
+--------------------+----+---------------------------------------------------+
| in[0-7]_max_alarm  | rw | Set high threshold in mV of DWC for channel [0-7] |
+--------------------+----+---------------------------------------------------+
| in[0-7]_min_alarm  | rw | Set low threshold in mV of DWC for channel [0-7]  |
+--------------------+----+---------------------------------------------------+

Example
-------

.. code:: c

	#include <stdio.h>
	#include <stdlib.h>
	#include <fcntl.h>
	#include <sys/select.h>
	#include <unistd.h>

	int main(void)
	{
		int		retval, fd;
		fd_set	exceptfds;
		char	buf[16];

		fd = open("/sys/class/hwmon/hwmon1/alarms", O_RDONLY);

		while (1) {

			FD_ZERO(&exceptfds);
			FD_SET(fd, &exceptfds);

			/* Must be assigned to 'exceptional conditions'. For poll() use
				POLLPRI. */
			retval = select(fd + 1, NULL, NULL, &exceptfds, NULL);
			if (retval == -1)
				perror("select()");
			else if (retval) {
				/* Close and reopen is required, since it's a sysfs file */
				close(fd);
				fd = open("/sys/class/hwmon/hwmon1/alarms", O_RDONLY);
				retval = read(fd, buf, sizeof(buf));
				printf("Received: %.*s\n", retval,buf);
			}
		}

	close(fd);
	exit(EXIT_SUCCESS);
	}

Notes
-----

TODO support for GPIOs, ADC hysteresis and counts is missing yet.
