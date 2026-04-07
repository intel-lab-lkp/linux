// SPDX-License-Identifier: GPL-2.0
/*
 * Drives vfio_ap mdev migration.
 *
 * Copyright IBM Corp. 2025
 */
#include "uapi/linux/vfio_ap.h"

#include "vfio_ap_private.h"

/**
 * vfio_ap_migration_data - the data needed to migrate a guest with pass-through
 *			    access to AP devices
 *
 * @mig_state:		the current migration state
 * @resuming_migf:	the object used to resume the target guest
 * @saving_migf:	the object used to save the state of the source guest
 */
struct vfio_ap_migration_data {
	enum vfio_device_mig_state	mig_state;
	struct vfio_ap_migration_file	*resuming_migf;
	struct vfio_ap_migration_file	*saving_migf;
};

/**
 * vfio_ap_queue_info - the information for an AP queue
 *
 * @data: contains the queue information returned in GR2 from the PQAP(TAPQ)
 *	  command
 * @apqn: the APQN of the queue
 */
struct vfio_ap_queue_info {
	u64 data;
	u16 apqn;
};

/**
 * vfio_ap_config - the guest's AP configuration
 *
 * @num_queues:	the number of queues passed through to the guest
 * @qinfo:	an array of vfio_ap_queue_info objects, each specifying the
 *		queue information for a queue passed through to the guest
 */
struct vfio_ap_config {
	unsigned int			num_queues;
	struct vfio_ap_queue_info	qinfo[];
};

/**
 * vfio_ap_migration_file - object used to facilitate migration of a guest with
 *			    pass-through access to AP devices
 *
 * @matrix_mdev: the mediated device attached to the guest being migrated
 * @filp:	 the file used to facilitate communication between userspace
 *		 and the vfio_ap device driver during a particular phase of
 *		 the migration
 * @disabled:	 boolean value indicating whether this object is disabled (true)
 *		 or not (false)
 * @ap_config:	 the information for each queue passed through to a guest
 * @config_sz:	 the size of @ap_config when filled with queue information
 */
struct vfio_ap_migration_file {
	struct ap_matrix_mdev	*matrix_mdev;
	struct file		*filp;
	bool			disabled;
	struct vfio_ap_config	*ap_config;
	size_t			config_sz;
};
