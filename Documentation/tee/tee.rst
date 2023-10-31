.. SPDX-License-Identifier: GPL-2.0

===================================
TEE (Trusted Execution Environment)
===================================

This document describes the TEE subsystem in Linux.

Overview
========

A TEE is a trusted OS running in some secure environment, for example,
TrustZone on ARM CPUs, or a separate secure co-processor etc. A TEE driver
handles the details needed to communicate with the TEE.

This subsystem deals with:

- Registration of TEE drivers

- Managing shared memory between Linux and the TEE

- Providing a generic API to the TEE

The TEE interface
=================

include/uapi/linux/tee.h defines the generic interface to a TEE.

User space (the client) connects to the driver by opening /dev/tee[0-9]* or
/dev/teepriv[0-9]*.

- TEE_IOC_SHM_ALLOC allocates shared memory and returns a file descriptor
  which user space can mmap. When user space doesn't need the file
  descriptor any more, it should be closed. When shared memory isn't needed
  any longer it should be unmapped with munmap() to allow the reuse of
  memory.

- TEE_IOC_VERSION lets user space know which TEE this driver handles and
  its capabilities.

- TEE_IOC_OPEN_SESSION opens a new session to a Trusted Application.

- TEE_IOC_INVOKE invokes a function in a Trusted Application.

- TEE_IOC_CANCEL may cancel an ongoing TEE_IOC_OPEN_SESSION or TEE_IOC_INVOKE.

- TEE_IOC_CLOSE_SESSION closes a session to a Trusted Application.

There are two classes of clients, normal clients and supplicants. The latter is
a helper process for the TEE to access resources in Linux, for example file
system access. A normal client opens /dev/tee[0-9]* and a supplicant opens
/dev/teepriv[0-9].

Much of the communication between clients and the TEE is opaque to the
driver. The main job for the driver is to receive requests from the
clients, forward them to the TEE and send back the results. In the case of
supplicants the communication goes in the other direction, the TEE sends
requests to the supplicant which then sends back the result.

The TEE kernel interface
========================

Kernel provides a TEE bus infrastructure where a Trusted Application is
represented as a device identified via Universally Unique Identifier (UUID) and
client drivers register a table of supported device UUIDs.

TEE bus infrastructure registers following APIs:

match():
  iterates over the client driver UUID table to find a corresponding
  match for device UUID. If a match is found, then this particular device is
  probed via corresponding probe API registered by the client driver. This
  process happens whenever a device or a client driver is registered with TEE
  bus.

uevent():
  notifies user-space (udev) whenever a new device is registered on
  TEE bus for auto-loading of modularized client drivers.

TEE bus device enumeration is specific to underlying TEE implementation, so it
is left open for TEE drivers to provide corresponding implementation.

Then TEE client driver can talk to a matched Trusted Application using APIs
listed in include/linux/tee_drv.h.

TEE client driver example
-------------------------

Suppose a TEE client driver needs to communicate with a Trusted Application
having UUID: ``ac6a4085-0e82-4c33-bf98-8eb8e118b6c2``, so driver registration
snippet would look like::

	static const struct tee_client_device_id client_id_table[] = {
		{UUID_INIT(0xac6a4085, 0x0e82, 0x4c33,
			   0xbf, 0x98, 0x8e, 0xb8, 0xe1, 0x18, 0xb6, 0xc2)},
		{}
	};

	MODULE_DEVICE_TABLE(tee, client_id_table);

	static struct tee_client_driver client_driver = {
		.id_table	= client_id_table,
		.driver		= {
			.name		= DRIVER_NAME,
			.bus		= &tee_bus_type,
			.probe		= client_probe,
			.remove		= client_remove,
		},
	};

	static int __init client_init(void)
	{
		return driver_register(&client_driver.driver);
	}

	static void __exit client_exit(void)
	{
		driver_unregister(&client_driver.driver);
	}

	module_init(client_init);
	module_exit(client_exit);
