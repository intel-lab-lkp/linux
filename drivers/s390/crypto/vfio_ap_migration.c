// SPDX-License-Identifier: GPL-2.0
/*
 * Drives vfio_ap mdev migration.
 *
 * Copyright IBM Corp. 2025
 */
#include "vfio_ap_private.h"

/* Magic number and version for the vfio_ap_config migration blob */
#define VFIO_AP_MIG_MAGIC			0x76666170U  /* "vfap" */
#define VFIO_AP_MIG_VERSION			1U

/**
 * struct vfio_ap_migration_file
 *
 * This object is used for chunk processing of multiple reads and writes of
 * AP configuration information.
 *
 * @filp:	file stream used to read or write AP configuration data
 * @ap_config:	object used to store AP configuration data between read or write
 *		calls
 * @config_sz:	the size (in bytes) of @ap_config
 */
struct vfio_ap_migration_file {
	struct file		*filp;
	struct vfio_ap_config	*ap_config;
	unsigned long		config_sz;
};

/**
 * struct vfio_ap_migration_data:
 *
 * Manages the migration state for the VFIO device that maintains the AP
 * configuration of the guest being migrated.
 *
 * @mig_state:		the current migration state
 * @resuming_mig_file:	the object used to restore the state of the vfio-ap
 *			device on the destination host.
 * @stop_copy_mig_file:	the object used to store the AP configuration of the
 *			source guest for transfer to the destination host.
 */
struct vfio_ap_migration_data {
	enum vfio_device_mig_state	mig_state;
	struct vfio_ap_migration_file	resuming_mig_file;
	struct vfio_ap_migration_file	stop_copy_mig_file;
};

/**
 * struct vfio_ap_queue_info - the information for an AP queue
 *
 * @data: contains the queue information returned in GR2 from the PQAP(TAPQ)
 *	  command
 * @apqn: the APQN of the queue
 * @reserved: padding to ensure consistent structure size across platforms
 */
struct vfio_ap_queue_info {
	u64 data;
	u16 apqn;
	u8  reserved[6];
};

/**
 * struct vfio_ap_config:
 *
 * Stores the state of a guest's AP configuration.
 *
 * VFIO device migration state transition from STOP to STOP_COPY:
 * -------------------------------------------------------------
 * When the migration state transitions from STOP to STOP_COPY, the vfio_ap device
 * driver will open a file stream in read-only mode and return the fd to userspace.
 * This fd is used during the STOP_COPY phase to read the current state of the
 * vfio-ap device on the source host. In response, the driver will store the
 * source guest's AP configuration data in a vfio_ap_config object and copy it to
 * userspace.
 *
 * VFIO device migration state transition from STOP to RESUMING:
 * ------------------------------------------------------------
 * When the VFIO migration state transitions from STOP to RESUMING,
 * the vfio_ap device driver will open a file stream in write-only mode and
 * return the fd to userspace. This fd is used during the RESUMING phase to
 * write the source guest's vfio_ap_config data that was read in during the
 * STOP_COPY phase to the vfio_ap device driver on the destination host. In
 * response, the device driver will copy the data sent from userspace to a
 * vfio_ap_config instance which is then used to update the destination guest's
 * AP configuration.
 *
 * Since the source and destination hosts may be running different versions of
 * the linux kernel, the vfio_ap_config object provides two fields (@magic and
 * @version) which must be set by the source device driver and verified by the
 * destination device driver to ensure the migration ABI of the source and
 * destination hosts are compatible.
 *
 * Note: Since a guest's AP configuration could be comprise of a large number of
 *	 AP queue devices, a vfio_ap_config object should be allocated using
 *	 kvzalloc.
 *
 * @magic:	identifies this as a valid vfio_ap_config migration blob;
 *		must equal VFIO_AP_MIG_MAGIC
 * @version:	layout version; must equal VFIO_AP_MIG_VERSION
 * @num_queues:	the number of queues passed through to the guest
 * @reserved:	padding to ensure proper alignment of @adm
 * @adm:	bitmap specifying the control domains in the AP configuration
 * @qinfo:	an array of vfio_ap_queue_info objects, each specifying the
 *		queue information for a queue passed through to the guest
 */
struct vfio_ap_config {
	u32				magic;
	u32				version;
	u32				num_queues;
	u8				reserved[4];
	u64				adm[DIV_ROUND_UP(AP_DOMAINS, 64)];
	struct vfio_ap_queue_info	qinfo[] __counted_by(num_queues);
};
