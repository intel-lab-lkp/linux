// SPDX-License-Identifier: GPL-2.0
/*
 * Drives vfio_ap mdev migration.
 *
 * Copyright IBM Corp. 2025
 */
#include <linux/anon_inodes.h>
#include <linux/file.h>
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

	struct {
		struct file *filp;
		struct vfio_ap_config	*ap_config;
		size_t			config_sz;
	} resuming_mig_state;

	struct file			*stop_copy_mig_file;
};

/**
 * vfio_ap_queue_info - the information for an AP queue
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
 * vfio_ap_config - the guest's AP configuration
 *
 * @num_queues:	the number of queues passed through to the guest
 * @reserved: padding to ensure proper alignment of qinfo array
 * @qinfo:	an array of vfio_ap_queue_info objects, each specifying the
 *		queue information for a queue passed through to the guest
 */
struct vfio_ap_config {
	u32				num_queues;
	u8				reserved[4];
	size_t				config_sz;
	struct vfio_ap_queue_info	qinfo[] __counted_by(num_queues);
};

static void
vfio_ap_release_stop_copy_file(struct vfio_ap_migration_data *mig_data)
{
	if (mig_data->stop_copy_mig_file)
		mig_data->stop_copy_mig_file = NULL;
}

static int vfio_ap_release_mig_file(struct inode *file_inode, struct file *filp)
{
	struct ap_matrix_mdev *matrix_mdev = filp->private_data;

	if (!matrix_mdev || !matrix_mdev->mig_data)
		return -ENODEV;

	if (filp == matrix_mdev->mig_data->stop_copy_mig_file)
		vfio_ap_release_stop_copy_file(matrix_mdev->mig_data);
	else
		return -ENOENT;

	return 0;
}

/**
 * validate_stop_copy_read_parms: Validate the input parameters to the
 *                                vfio_ap_stop_copy_read function
 *
 * @matrix_mdev: The object device containing the state to be read
 * @filp: Pointer to the file stream used to read the vfio-ap device state
 * @pos:  The file offset from which to start reading data
 * @len:  The length of the data to be read
 *
 * Verify the following:
 * - @filp private data is an ap_matrix_mdev instance
 * - @filp is the instance opened when state transitioned from STOP to STOP_COPY
 * - @pos + @len does not cause integer overflow
 *
 * Returns: 0 if the parameters pass validation; otherwise returns an error
 */
static int validate_stop_copy_read_parms(struct file *filp, loff_t *pos,
					 size_t len)
{
	struct vfio_ap_migration_data *mig_data;
	struct ap_matrix_mdev *matrix_mdev;
	loff_t total_len;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	if (check_add_overflow((loff_t)len, *pos, &total_len))
		return -EIO;

	matrix_mdev = filp->private_data;

	if (!matrix_mdev || !matrix_mdev->mig_data)
		return -ENODEV;

	mig_data = matrix_mdev->mig_data;

	if (mig_data->stop_copy_mig_file != filp)
		return -EINVAL;

	return 0;
}

static size_t vfio_ap_config_size(struct ap_matrix_mdev *matrix_mdev,
				  int *num_queues)
{
	size_t qinfo_size;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	*num_queues = vfio_ap_mdev_get_num_queues(&matrix_mdev->shadow_apcb);
	qinfo_size = *num_queues * sizeof(struct vfio_ap_queue_info);

	return qinfo_size + sizeof(struct vfio_ap_config);
}

static int get_hardware_info_for_queue(struct ap_matrix_mdev *matrix_mdev,
				       struct ap_tapq_hwinfo *hwinfo,
				       unsigned long apqn)
{
	struct ap_queue_status status;

	status = ap_tapq(apqn, hwinfo);

	switch (status.response_code) {
	case AP_RESPONSE_NORMAL:
	case AP_RESPONSE_RESET_IN_PROGRESS:
	case AP_RESPONSE_DECONFIGURED:
	case AP_RESPONSE_CHECKSTOPPED:
	case AP_RESPONSE_BUSY:
		/* For all these RCs the tapq info should be available */
		return 0;
	case AP_RESPONSE_Q_NOT_AVAIL:
		dev_err(matrix_mdev->vdev.dev,
			"migration failed: Failed to get hwinfo for queue %02lx.%04lx on target host: TAPQ rc=%d",
			AP_QID_CARD(apqn), AP_QID_QUEUE(apqn), status.response_code);
		return -ENODEV;
	default:
		/* On a pending async error the tapq info should be available */
		if (!status.async)
			return 0;

		dev_err(matrix_mdev->vdev.dev,
			"Failed to get hwinfo for queue %02lx.%04lx: TAPQ rc=%d",
			AP_QID_CARD(apqn), AP_QID_QUEUE(apqn), status.response_code);
		return -EIO;
	}

	return -EINVAL;
}

static int vfio_ap_store_queue_info(struct ap_matrix_mdev *matrix_mdev,
				    struct vfio_ap_config *ap_config)
{
	unsigned long *apm, *aqm, num_queues, apid, apqi, apqn;
	struct ap_tapq_hwinfo source_hwinfo;
	int ret;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	apm = matrix_mdev->shadow_apcb.apm;
	aqm = matrix_mdev->shadow_apcb.aqm;
	num_queues = 0;

	for_each_set_bit_inv(apid, apm, AP_DEVICES) {
		for_each_set_bit_inv(apqi, aqm, AP_DOMAINS) {
			apqn = AP_MKQID(apid, apqi);

			ret = get_hardware_info_for_queue(matrix_mdev,
							  &source_hwinfo, apqn);
			if (ret)
				return ret;

			ap_config->qinfo[num_queues].apqn = apqn;
			ap_config->qinfo[num_queues].data = source_hwinfo.value;
			num_queues += 1;
		}
	}

	return (num_queues != ap_config->num_queues) ? -EINVAL : 0;
}

static int
vfio_ap_get_config(struct ap_matrix_mdev *matrix_mdev,
		   struct vfio_ap_config **ap_config, size_t *ap_config_size)
{
	struct vfio_ap_config *ap_configuration;
	int num_queues, ret;

	*ap_config_size = vfio_ap_config_size(matrix_mdev, &num_queues);

	ap_configuration = kzalloc(*ap_config_size, GFP_KERNEL_ACCOUNT);
	if (!ap_configuration)
		return -ENOMEM;

	ap_configuration->num_queues = num_queues;

	ret = vfio_ap_store_queue_info(matrix_mdev, ap_configuration);
	if (ret) {
		kfree(ap_configuration);
		return ret;
	}

	*ap_config = ap_configuration;

	return 0;
}

static ssize_t vfio_ap_stop_copy_read(struct file *filp, char __user *buf,
				      size_t len, loff_t *pos)
{
	struct ap_matrix_mdev *matrix_mdev;
	size_t ret = 0, ap_config_size;
	struct vfio_ap_config *ap_config;

	/*
	 * When userspace calls read() with an explicit offset (pread), pos is
	 * non-NULL and the function rejects it with -ESPIPE (illegal seek). For
	 * normal read() calls, pos is NULL, so we'll use the file's internal
	 * position filp->f_pos
	 */
	if (pos)
		return -ESPIPE;

	mutex_lock(&matrix_dev->mdevs_lock);

	pos = &filp->f_pos;

	ret = validate_stop_copy_read_parms(filp, pos, len);
	if (ret) {
		mutex_unlock(&matrix_dev->mdevs_lock);
		return ret;
	}

	matrix_mdev = filp->private_data;

	ret = vfio_ap_get_config(matrix_mdev, &ap_config, &ap_config_size);
	if (ret) {
		mutex_unlock(&matrix_dev->mdevs_lock);
		return ret;
	}

	/*
	 * If the position exceeds the size of the AP configuration data,
	 * then indicate EOF; otherwise calculate the length of the data to
	 * read such that a buffer overrun is prevented.
	 */
	if (*pos >= ap_config_size)
		len = 0;
	else
		len = min_t(size_t, ap_config_size - *pos, len);

	/* If we've reached an EOF condition, let the caller know */
	if (len == 0) {
		kfree(ap_config);
		mutex_unlock(&matrix_dev->mdevs_lock);
		return 0;
	}

	mutex_unlock(&matrix_dev->mdevs_lock);

	if (copy_to_user(buf, (char *)ap_config + *pos, len)) {
		kfree(ap_config);
		return -EFAULT;
	}

	kfree(ap_config);
	*pos += len;
	return len;
}

static const struct file_operations vfio_ap_stop_copy_fops = {
	.owner = THIS_MODULE,
	.read = vfio_ap_stop_copy_read,
	.compat_ioctl = compat_ptr_ioctl,
	.release = vfio_ap_release_mig_file,
};

static struct file *vfio_ap_open_file_stream(struct ap_matrix_mdev *matrix_mdev,
					     const struct file_operations *fops,
					     int flags)
{
	struct file *filp;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	filp = anon_inode_getfile("vfio_ap_mig_file", fops, matrix_mdev, flags);
	if (!IS_ERR(filp))
		stream_open(filp->f_inode, filp);

	return filp;
}

static struct file *
vfio_ap_transition_to_state(struct ap_matrix_mdev *matrix_mdev,
			    enum vfio_device_mig_state new_state)
{
	struct vfio_ap_migration_data *mig_data;
	enum vfio_device_mig_state cur_state;

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
		struct file *filp = vfio_ap_open_file_stream(matrix_mdev,
							     &vfio_ap_stop_copy_fops,
							     O_RDONLY);
		if (IS_ERR(filp))
			return ERR_CAST(filp);

		mig_data->stop_copy_mig_file = filp;

		return filp;
	}

	if (cur_state == VFIO_DEVICE_STATE_STOP &&
	    new_state == VFIO_DEVICE_STATE_RESUMING) {
		/* TODO */
		return ERR_PTR(-EOPNOTSUPP);
	}

	if ((cur_state == VFIO_DEVICE_STATE_RESUMING &&
	     new_state == VFIO_DEVICE_STATE_STOP) ||
	    (cur_state == VFIO_DEVICE_STATE_STOP_COPY &&
	     new_state == VFIO_DEVICE_STATE_STOP)) {
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

	/*
	 * The mig_data pointer is set in the vfio_ap_init_migration_data
	 * function which is called when the vfio-ap device fd is opened.
	 * Since the implicit pre-open state is RUNNING, a request to set
	 * RUNNING is a no-op. Any other state transition is invalid before
	 * open_device.
	 */
	if (!mig_data) {
		mutex_unlock(&matrix_dev->mdevs_lock);
		if (new_state == VFIO_DEVICE_STATE_RUNNING)
			return NULL;
		return ERR_PTR(-ENODEV);
	}

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
	struct ap_matrix_mdev *matrix_mdev;
	struct vfio_ap_migration_data *mig_data;

	mutex_lock(&matrix_dev->mdevs_lock);

	matrix_mdev = container_of(vdev, struct ap_matrix_mdev, vdev);
	mig_data =  matrix_mdev->mig_data;

	/*
	 * The mig_data pointer is set in the vfio_ap_init_migration_data
	 * function which is called when the vfio-ap device fd is opened.
	 * If mig_data is NULL, report RUNNING as the implicit pre-open state
	 * so userspace doesn't need to perform any state transition before the
	 * device becomes active.
	 */
	*current_state = (mig_data) ? mig_data->mig_state :
				      VFIO_DEVICE_STATE_RUNNING;

	mutex_unlock(&matrix_dev->mdevs_lock);

	return 0;
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
 * vfio_ap_init_migrations_capabilities - initialize migration capabilities
 *
 * @matrix_mdev: pointer to object containing the mdev state
 */
void vfio_ap_init_migration_capabilities(struct ap_matrix_mdev *matrix_mdev)
{
	matrix_mdev->vdev.migration_flags = VFIO_MIGRATION_STOP_COPY;
	matrix_mdev->vdev.mig_ops = &vfio_ap_migration_ops;
}

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

	kfree(matrix_mdev->mig_data);
	matrix_mdev->mig_data = NULL;
}

/**
 * vfio_ap_reset_migration_state - Reset the vfio-ap migration state
 *
 * @matrix_mdev: pointer to the object maintaining the vfio-ap device state
 *
 * Called during VFIO_DEVICE_RESET to clean up any active migration
 * stte and reset the device to RUNNING state as required by the VFIO
 * migration specification.
 */
void vfio_ap_reset_migration_state(struct ap_matrix_mdev *matrix_mdev)
{
	lockdep_assert_held(&matrix_dev->mdevs_lock);

	if (!matrix_mdev->mig_data)
		return;

	matrix_mdev->mig_data->mig_state = VFIO_DEVICE_STATE_RUNNING;
}
