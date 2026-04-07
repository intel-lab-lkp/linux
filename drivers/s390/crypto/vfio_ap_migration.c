// SPDX-License-Identifier: GPL-2.0
/*
 * Drives vfio_ap mdev migration.
 *
 * Copyright IBM Corp. 2025
 */
#include <linux/file.h>
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

static void vfio_ap_disable_file(struct vfio_ap_migration_file *migf)
{
	lockdep_assert_held(&matrix_dev->mdevs_lock);
	migf->matrix_mdev = NULL;
	migf->disabled = true;
	migf->filp->f_pos = 0;
}

static void vfio_ap_release_mig_files(struct ap_matrix_mdev *matrix_mdev)
{
	struct vfio_ap_migration_data *mig_data;

	lockdep_assert_held(&matrix_dev->mdevs_lock);
	mig_data = matrix_mdev->mig_data;

	if (mig_data->resuming_migf) {
		vfio_ap_disable_file(mig_data->resuming_migf);
		fput(mig_data->resuming_migf->filp);
		mig_data->resuming_migf = NULL;
	}

	if (mig_data->saving_migf) {
		vfio_ap_disable_file(mig_data->saving_migf);
		fput(mig_data->saving_migf->filp);
		mig_data->saving_migf = NULL;
	}
}

static struct file *vfio_ap_set_state(struct vfio_device *vdev,
				      enum vfio_device_mig_state  new_state)
{
	return NULL;
}

static int vfio_ap_get_state(struct vfio_device *vdev,
			     enum vfio_device_mig_state  *current_state)
{
	return -EOPNOTSUPP;
}

static int vfio_ap_get_data_size(struct vfio_device *vdev,
				 unsigned long *stop_copy_length)
{
	return -EOPNOTSUPP;
}

static const struct vfio_migration_ops vfio_ap_migration_ops = {
	.migration_set_state = vfio_ap_set_state,
	.migration_get_state = vfio_ap_get_state,
	.migration_get_data_size = vfio_ap_get_data_size,
};

/**
 * vfio_ap_init_migration_data - initialize migration data and functions
 *
 * @matrix_mdev: pointer to object containing the mdev state
 *
 * Return: zero if initialization is successful; otherwise, returns a error.
 */
int vfio_ap_init_migration_data(struct ap_matrix_mdev *matrix_mdev)
{
	struct vfio_ap_migration_data *mig_data;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	mig_data = kzalloc_obj(struct vfio_ap_migration_data, GFP_KERNEL);
	if (!mig_data)
		return -ENOMEM;

	mig_data->mig_state = VFIO_DEVICE_STATE_RUNNING;
	matrix_mdev->vdev.migration_flags = VFIO_MIGRATION_STOP_COPY;
	matrix_mdev->vdev.mig_ops = &vfio_ap_migration_ops;
	matrix_mdev->mig_data = mig_data;

	return 0;
}

/**
 * vfio_ap_release_migration_data: reclaim private migration data
 *
 * @vdev: pointer to the mdev
 */
void vfio_ap_release_migration_data(struct ap_matrix_mdev *matrix_mdev)
{
	lockdep_assert_held(&matrix_dev->mdevs_lock);
	if (!matrix_mdev->mig_data)
		return;

	vfio_ap_release_mig_files(matrix_mdev);
	kfree(matrix_mdev->mig_data);
	matrix_mdev->mig_data = NULL;
}
