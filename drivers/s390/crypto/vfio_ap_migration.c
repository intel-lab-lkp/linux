// SPDX-License-Identifier: GPL-2.0
/*
 * Drives vfio_ap mdev migration.
 *
 * Copyright IBM Corp. 2025
 */
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include "uapi/linux/vfio_ap.h"

#include "ap_bus.h"
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

static ssize_t vfio_ap_save_read(struct file *, char __user *, size_t, loff_t *)
{
	/* TODO */
	return -EOPNOTSUPP;
}

static int vfio_ap_release_migf(struct inode *, struct file *)
{
	/* TODO */
	return -EOPNOTSUPP;
}

static const struct file_operations vfio_ap_save_fops = {
	.owner = THIS_MODULE,
	.read = vfio_ap_save_read,
	.compat_ioctl = compat_ptr_ioctl,
	.release = vfio_ap_release_migf,
};

static struct vfio_ap_config
*vfio_ap_allocate_config(struct ap_matrix_mdev *matrix_mdev, size_t *config_sz)
{
	struct vfio_ap_config *ap_config;
	size_t qinfo_size;
	int num_queues;

	lockdep_assert_held(&matrix_dev->mdevs_lock);
	num_queues = vfio_ap_mdev_get_num_queues(&matrix_mdev->shadow_apcb);
	qinfo_size = num_queues * sizeof(struct vfio_ap_queue_info);
	*config_sz = qinfo_size + sizeof(struct vfio_ap_config);
	ap_config = kzalloc(*config_sz, GFP_KERNEL_ACCOUNT);

	if (!ap_config)
		return ERR_PTR(-ENOMEM);

	ap_config->num_queues = num_queues;

	return ap_config;
}

static void vfio_ap_store_queue_info(struct vfio_ap_migration_file *migf)
{
	unsigned long *apm, *aqm, num_queues, apid, apqi, apqn, data;
	struct ap_matrix_mdev *matrix_mdev;
	struct vfio_ap_queue *q;

	lockdep_assert_held(&matrix_dev->mdevs_lock);
	matrix_mdev = migf->matrix_mdev;
	apm = matrix_mdev->shadow_apcb.apm;
	aqm = matrix_mdev->shadow_apcb.aqm;
	num_queues = 0;

	for_each_set_bit_inv(apid, apm, AP_DEVICES) {
		for_each_set_bit_inv(apqi, aqm, AP_DOMAINS) {
			apqn = AP_MKQID(apid, apqi);
			q = vfio_ap_mdev_get_queue(matrix_mdev, apqn);

			if (!q)
				continue;

			migf->ap_config->qinfo[num_queues].apqn = apqn;
			data = q->hwinfo.value;
			migf->ap_config->qinfo[num_queues].data = data;
			num_queues += 1;
			dev_dbg(matrix_mdev->vdev.dev,
				"%s (%d): qinfo: apqn=%04lx data=%016lx\n",
				__func__, __LINE__, apqn, data);
		}
	}
}

static struct vfio_ap_migration_file
*vfio_ap_allocate_migf(struct ap_matrix_mdev *matrix_mdev)
{
	struct vfio_ap_migration_file *migf;
	struct vfio_ap_config *ap_config;
	size_t config_size;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	migf = kzalloc_obj(struct vfio_ap_migration_file, GFP_KERNEL_ACCOUNT);
	if (!migf)
		return ERR_PTR(-ENOMEM);

	ap_config = vfio_ap_allocate_config(matrix_mdev, &config_size);
	if (IS_ERR(ap_config)) {
		kfree(migf);
		return ERR_CAST(ap_config);
	}

	migf->ap_config = ap_config;
	migf->config_sz = config_size;
	migf->matrix_mdev = matrix_mdev;

	return migf;
}

static void vfio_ap_deallocate_migf(struct vfio_ap_migration_file *migf)
{
	kfree(migf->ap_config);
	kfree(migf);
}

static struct file *vfio_ap_open_file_stream(struct vfio_ap_migration_file *migf,
					     const struct file_operations *fops,
					     int flags)
{
	struct file *filp;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	filp = anon_inode_getfile("vfio_ap_migf", fops, migf, flags);
	if (IS_ERR(filp))
		return ERR_CAST(filp);

	stream_open(filp->f_inode, filp);

	return filp;
}

static struct vfio_ap_migration_file *
vfio_ap_save_mdev_state(struct ap_matrix_mdev *matrix_mdev)
{
	struct vfio_ap_migration_data *mig_data;
	struct vfio_ap_migration_file *migf;
	struct file *filp;

	lockdep_assert_held(&matrix_dev->mdevs_lock);
	mig_data = matrix_mdev->mig_data;

	migf = vfio_ap_allocate_migf(matrix_mdev);
	if (IS_ERR(migf))
		return ERR_CAST(migf);

	filp = vfio_ap_open_file_stream(migf, &vfio_ap_save_fops, O_RDONLY);
	if (IS_ERR(filp)) {
		vfio_ap_deallocate_migf(migf);
		return ERR_CAST(filp);
	}

	migf->filp = filp;
	mig_data->saving_migf = migf;
	vfio_ap_store_queue_info(mig_data->saving_migf);

	return mig_data->saving_migf;
}

static struct file *
vfio_ap_transition_to_state(struct ap_matrix_mdev *matrix_mdev,
			    enum vfio_device_mig_state new_state)
{
	struct vfio_ap_migration_data *mig_data;
	enum vfio_device_mig_state cur_state;
	struct vfio_ap_migration_file *migf;

	lockdep_assert_held(&matrix_dev->mdevs_lock);
	mig_data = matrix_mdev->mig_data;
	cur_state = mig_data->mig_state;
	dev_dbg(matrix_mdev->vdev.dev, "%s: %d -> %d\n", __func__, cur_state,
		new_state);

	/*
	 * Begins the process of saving the vfio device state by creating and
	 * returning a streaming data_fd to be used to read out the internal
	 * state of the vfio-ap device on the source host.
	 */
	if (cur_state == VFIO_DEVICE_STATE_STOP &&
	    new_state == VFIO_DEVICE_STATE_STOP_COPY) {
		migf = vfio_ap_save_mdev_state(matrix_mdev);
		if (IS_ERR(migf))
			return ERR_CAST(migf);

		if (migf) {
			get_file(migf->filp);
			return migf->filp;
		}

		return NULL;
	}

	if (cur_state == VFIO_DEVICE_STATE_STOP &&
	    new_state == VFIO_DEVICE_STATE_RESUMING) {
		/* TODO */
		return ERR_PTR(-EOPNOTSUPP);
	}

	if (cur_state == VFIO_DEVICE_STATE_RESUMING &&
	    new_state == VFIO_DEVICE_STATE_STOP) {
		/* TODO */
		return ERR_PTR(-EOPNOTSUPP);
	}

	if (cur_state == VFIO_DEVICE_STATE_STOP_COPY &&
	    new_state == VFIO_DEVICE_STATE_STOP) {
		/* TODO */
		return ERR_PTR(-EOPNOTSUPP);
	}

	if ((cur_state == VFIO_DEVICE_STATE_STOP &&
	     new_state == VFIO_DEVICE_STATE_RUNNING) ||
	    (cur_state == VFIO_DEVICE_STATE_RUNNING &&
	     new_state == VFIO_DEVICE_STATE_STOP)) {
		/* TODO */
		return ERR_PTR(-EOPNOTSUPP);
	}

	/* vfio_mig_get_next_state() does not use arcs other than the above */
	WARN_ON(true);

	return ERR_PTR(-EINVAL);
}

static struct file *vfio_ap_set_state(struct vfio_device *vdev,
				      enum vfio_device_mig_state  new_state)
{
	int ret;
	struct file *filp = NULL;
	struct ap_matrix_mdev *matrix_mdev;
	enum vfio_device_mig_state next_state;
	struct vfio_ap_migration_data *mig_data;

	matrix_mdev = container_of(vdev, struct ap_matrix_mdev, vdev);

	mutex_lock(&matrix_dev->mdevs_lock);
	if (ap_is_se_guest()) {
		dev_err(matrix_mdev->vdev.dev,
				"Migration not allowed from or to a Secure Execution guest\n");
		mutex_unlock(&matrix_dev->mdevs_lock);
		return ERR_PTR(-EPERM);
	}

	mig_data = matrix_mdev->mig_data;
	dev_dbg(vdev->dev, "%s -> %d\n", __func__, new_state);

	while (mig_data->mig_state != new_state) {
		ret = vfio_mig_get_next_state(vdev, mig_data->mig_state,
					      new_state, &next_state);
		if (ret) {
			filp = ERR_PTR(ret);
			break;
		}

		filp = vfio_ap_transition_to_state(matrix_mdev, next_state);
		if (IS_ERR(filp))
			break;

		mig_data->mig_state = next_state;

		if (WARN_ON(filp && new_state != next_state)) {
			fput(filp);
			filp = ERR_PTR(-EINVAL);
			break;
		}
	}

	mutex_unlock(&matrix_dev->mdevs_lock);

	return filp;
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
