// SPDX-License-Identifier: GPL-2.0
/*
 * Drives vfio_ap mdev migration.
 *
 * Copyright IBM Corp. 2025
 */
#include "vfio_ap_private.h"

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
 * struct vfio_ap_migration_data
 *
 * Manages the migration state for the VFIO device that maintains the AP
 * configuration of the guest being migrated.
 *
 * @mig_state:		the current migration state
 * @resuming_mig_file:	the object used to restore the state of the vfio-ap
 *			device the destination host:
 * @stop_copy_mig_file: the object used to store the AP configuration of the
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
 * struct vfio_ap_config - the guest's AP configuration
 *
 * @num_queues:	the number of queues passed through to the guest
 * @reserved:	padding to ensure proper alignment of @adm
 * @adm:	bitmap specifying the control domains in the AP configuration
 * @qinfo:	an array of vfio_ap_queue_info objects, each specifying the
 *		queue information for a queue passed through to the guest
 */
struct vfio_ap_config {
	u32				num_queues;
	u8				reserved[4];
	u64				adm[DIV_ROUND_UP(AP_DOMAINS, 64)];
	struct vfio_ap_queue_info	qinfo[] __counted_by(num_queues);
};
