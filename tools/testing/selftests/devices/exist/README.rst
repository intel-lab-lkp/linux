.. SPDX-License-Identifier: GPL-2.0
.. Copyright (c) 2024 Collabora Ltd

==========================
Device existence kselftest
==========================

This test verifies whether all devices still exist on the system when compared
to a reference run, allowing detection of regressions that cause devices to go
missing.

TL;DR
=====

Run ``./test_dev_exist.py -g``, then run ``./test_dev_exist.py``.

Usage
=====

The test program can be found as ``test_dev_exist.py`` in this directory. Run it
with the ``--help`` argument to get information for all available arguments.
Detailed usage follows below.

Reference generation
--------------------

Before running the test, it is necessary to generate a reference. To do that,
run it with the ``--generate-reference`` argument. This will generate a JSON
file encoding all the devices available, per subsystem (class or bus), in the
running system, as well as metadata about the system (kernel version,
configuration and system identifiers).

By default, the file will be saved in the current directory and named based on
the system identifier, but that can be changed through the use of the
``--reference-dir`` and ``--reference-file`` flags.

Running the test
----------------

To run the test, simply execute it **without** the ``--generate-reference``
argument. By default, once again, the test will look for the reference file in
the current directory and named as the system identifier, but that can be
changed through the ``--reference-dir`` and ``--reference-file`` flags.

Reading the results
-------------------

The test outputs in the KTAP format, with one result per subsystem. For each
failure the output shows the devices that were expected by the reference file,
the devices that were found in the running system, and a best-effort guess for
the devices that are missing in the system. For each device, its main properties
are printed out to help in identifying it.

As an example, below is the snippet printed when one of the three devices in the
media bus went missing::

  # Missing devices for subsystem 'media': 1 (Expected 3, found 2)
  # =================
  # Devices expected:
  #
  #   .:
  #     /sys/devices/pci0000:00/0000:00:14.0/usb3/3-8/3-8.3/3-8.3.2/3-8.3.2:1.0/media2
  #   uevent:
  #     MAJOR=237
  #     MINOR=2
  #     DEVNAME=media2
  #
  #   .:
  #     /sys/devices/pci0000:00/0000:00:14.0/usb3/3-9/3-9:1.0/media0
  #   uevent:
  #     MAJOR=237
  #     MINOR=0
  #     DEVNAME=media0
  #
  #   .:
  #     /sys/devices/pci0000:00/0000:00:14.0/usb3/3-9/3-9:1.2/media1
  #   uevent:
  #     MAJOR=237
  #     MINOR=1
  #     DEVNAME=media1
  #
  # -----------------
  # Devices found:
  #
  #   .:
  #     /sys/devices/pci0000:00/0000:00:14.0/usb3/3-9/3-9:1.0/media0
  #   uevent:
  #     MAJOR=237
  #     MINOR=0
  #     DEVNAME=media0
  #
  #   .:
  #     /sys/devices/pci0000:00/0000:00:14.0/usb3/3-9/3-9:1.2/media1
  #   uevent:
  #     MAJOR=237
  #     MINOR=1
  #     DEVNAME=media1
  #
  # -----------------
  # Devices missing (best guess):
  #
  #   .:
  #     /sys/devices/pci0000:00/0000:00:14.0/usb3/3-8/3-8.3/3-8.3.2/3-8.3.2:1.0/media2
  #   uevent:
  #     MAJOR=237
  #     MINOR=2
  #     DEVNAME=media2
  #
  # =================
  not ok 67 bus.media

Updating the reference
----------------------

As time goes on, new devices might be introduced in the system. To replace a
reference file with a more up-to-date one containing more devices, pass both
``--generate-reference`` and ``--update-reference`` arguments. The program will
refuse to replace the reference if the new one doesn't contain all the devices
in the old reference, as that is usually not desirable.

Caveats
=======

The test relies solely on the count of devices per subsystem to detect missing
devices. [#f1]_ That means that it is possible for the test to fail to detect a
missing device.

For example, if the running system contains one extra device and one missing
device on the same subsystem compared to the reference, no test will fail since
the count is the same. To minimize the risk of this happening, it is recommended
to keep the reference file as up-to-date as possible.

.. [#f1] The reason for this is that there aren't any device properties that are
  used for every device and that are guaranteed to uniquely identify them and be
  stable across kernel releases, so any attempt to match devices based on their
  properties would lead to false-positives.

Pre-existing reference files
============================

Due to the per-platform nature of the reference files, it is not viable to keep
them in-tree.

To facilitate running the test, especially by CI systems, a collection of
pre-existing reference files is kept at
https://github.com/kernelci/platform-test-parameters.
