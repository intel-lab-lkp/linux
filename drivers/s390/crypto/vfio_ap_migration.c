// SPDX-License-Identifier: GPL-2.0
/*
 * Drives vfio_ap mdev migration.
 *
 * Copyright IBM Corp. 2025
 */
#include <linux/anon_inodes.h>
#include <linux/file.h>
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

static void
vfio_ap_release_stop_copy_file(struct vfio_ap_migration_data *mig_data)
{
	kfree(mig_data->stop_copy_mig_file.ap_config);
	mig_data->stop_copy_mig_file.ap_config = NULL;
	mig_data->stop_copy_mig_file.config_sz = 0;
	mig_data->stop_copy_mig_file.filp = NULL;
}

static void
vfio_ap_release_resuming_file(struct vfio_ap_migration_data *mig_data)
{
	kfree(mig_data->resuming_mig_file.ap_config);
	mig_data->resuming_mig_file.ap_config = NULL;
	mig_data->resuming_mig_file.config_sz = 0;
	mig_data->resuming_mig_file.filp = NULL;
}

static int vfio_ap_release_mig_file(struct inode *file_inode, struct file *filp)
{
	struct ap_matrix_mdev *matrix_mdev = filp->private_data;
	int ret = 0;

	mutex_lock(&matrix_dev->mdevs_lock);

	/*
	 * mig_data may be NULL if the device was closed (vfio_ap_mdev_close_device)
	 * before the migration FD was released by userspace. In that case the
	 * migration file state was already cleaned up; nothing to do here.
	 */
	if (!matrix_mdev->mig_data)
		goto done;

	if (filp == matrix_mdev->mig_data->stop_copy_mig_file.filp)
		vfio_ap_release_stop_copy_file(matrix_mdev->mig_data);
	else if (filp == matrix_mdev->mig_data->resuming_mig_file.filp)
		vfio_ap_release_resuming_file(matrix_mdev->mig_data);
	else
		ret = -ENOENT;

done:
	mutex_unlock(&matrix_dev->mdevs_lock);
	vfio_device_put_registration(&matrix_mdev->vdev);
	return ret;
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

	/*
	 * matrix_mdev is guaranteed live here: vfio_ap_open_file_stream() took
	 * a vfio_device registration reference that is held until
	 * vfio_ap_release_mig_file() runs, so the embedding matrix_mdev cannot
	 * be freed while this file descriptor is open.
	 */
	matrix_mdev = filp->private_data;

	if (!matrix_mdev->mig_data)
		return -ENODEV;

	mig_data = matrix_mdev->mig_data;

	if (mig_data->stop_copy_mig_file.filp != filp)
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

static int get_hardware_info_for_queue(const char *mdev_name,
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
		pr_err("vfio_ap_mdev %s: Failed to get hwinfo for queue %02lx.%04lx: TAPQ rc=%d",
		       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn),
		       status.response_code);
		return -ENODEV;
	default:
		/*
		 * Without a pending async error, the tapq info should be
		 * available
		 */
		if (status.async)
			return 0;

		pr_err("vfio_ap_mdev %s:Failed to get hwinfo for queue %02lx.%04lx: TAPQ rc=%d",
		       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn),
		       status.response_code);
		return -EIO;
	}

	return -EINVAL;
}

static int vfio_ap_store_queue_info(const char *mdev_name,
				    struct vfio_ap_config *ap_config)
{
	struct ap_tapq_hwinfo source_hwinfo;
	unsigned long num_queues;
	int ret;

	/*
	 * ap_tapq() is a hardware instruction that may take time to complete.
	 * It must be called without mdevs_lock held to avoid blocking other
	 * mdevs. The apqn list was already snapshotted into ap_config->qinfo[]
	 * by the caller under the lock.
	 */
	for (num_queues = 0; num_queues < ap_config->num_queues; num_queues++) {
		ret = get_hardware_info_for_queue(mdev_name, &source_hwinfo,
						  ap_config->qinfo[num_queues].apqn);
		if (ret)
			return ret;

		ap_config->qinfo[num_queues].data = source_hwinfo.value;
	}

	return 0;
}

static int vfio_ap_get_config(struct ap_matrix_mdev *matrix_mdev)
{
	unsigned long *apm, *aqm, apid, apqi, num_queues;
	struct vfio_ap_config *ap_configuration;
	const char *mdev_name;
	size_t ap_config_size;
	int ret;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	ap_config_size = vfio_ap_config_size(matrix_mdev, (int *)&num_queues);

	ap_configuration = kzalloc(ap_config_size, GFP_KERNEL_ACCOUNT);
	if (!ap_configuration)
		return -ENOMEM;

	/*
	 * num_queues must be set before writing qinfo[] elements; the
	 * __counted_by(num_queues) annotation on qinfo[] causes the compiler to
	 * insert bounds checks that evaluate against ap_configuration->num_queues.
	 * Writing through qinfo[i] with num_queues still 0 would trap.
	 */
	ap_configuration->num_queues = num_queues;

	apm = matrix_mdev->shadow_apcb.apm;
	aqm = matrix_mdev->shadow_apcb.aqm;
	num_queues = 0;
	for_each_set_bit_inv(apid, apm, AP_DEVICES) {
		for_each_set_bit_inv(apqi, aqm, AP_DOMAINS) {
			ap_configuration->qinfo[num_queues].apqn =
				AP_MKQID(apid, apqi);
			num_queues += 1;
		}
	}
	memcpy(ap_configuration->adm, matrix_mdev->shadow_apcb.adm,
	       sizeof(ap_configuration->adm));
	mdev_name = dev_name(matrix_mdev->vdev.dev);

	ret = vfio_ap_store_queue_info(mdev_name, ap_configuration);
	if (ret) {
		kfree(ap_configuration);
		return ret;
	}

	matrix_mdev->mig_data->stop_copy_mig_file.ap_config = ap_configuration;
	matrix_mdev->mig_data->stop_copy_mig_file.config_sz = ap_config_size;

	return 0;
}

static ssize_t vfio_ap_stop_copy_read(struct file *filp, char __user *buf,
				      size_t len, loff_t *pos)
{
	struct vfio_ap_migration_file *mig_file;
	struct ap_matrix_mdev *matrix_mdev;
	loff_t read_pos;
	ssize_t ret;

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
	mig_file = &matrix_mdev->mig_data->stop_copy_mig_file;

	if (!mig_file->ap_config) {
		ret = vfio_ap_get_config(matrix_mdev);
		if (ret) {
			mutex_unlock(&matrix_dev->mdevs_lock);
			return ret;
		}
	}

	/*
	 * Compute the offset and clamped length fully under the lock so that
	 * concurrent read()s on this stream file each see a consistent view of
	 * the current position.  *pos is advanced here while we still hold the
	 * lock; copy_to_user() then uses the snapshot read_pos.  This prevents
	 * two threads from calculating the same offset and both copying the
	 * same region (or one reading past the end of the buffer).
	 */
	if (*pos >= mig_file->config_sz) {
		mutex_unlock(&matrix_dev->mdevs_lock);
		return 0;
	}

	len = min_t(size_t, mig_file->config_sz - *pos, len);
	if (len == 0) {
		mutex_unlock(&matrix_dev->mdevs_lock);
		return 0;
	}

	read_pos = *pos;
	*pos += len;

	/*
	 * Drop the lock only for the copy_to_user().  The ap_config buffer is
	 * stable: it is allocated once in vfio_ap_get_config() and freed only
	 * in vfio_ap_release_mig_files() / vfio_ap_release_stop_copy_file(),
	 * both of which require mdevs_lock.  Since we already advanced *pos
	 * above, no other thread will compute an overlapping region.
	 */
	mutex_unlock(&matrix_dev->mdevs_lock);

	if (copy_to_user(buf, (char *)mig_file->ap_config + read_pos, len))
		return -EFAULT;

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

	/*
	 * Pin the vfio_device registration so that matrix_mdev cannot be freed
	 * while the migration FD is still open. The matching put is in
	 * vfio_ap_release_mig_file().
	 */
	if (!vfio_device_try_get_registration(&matrix_mdev->vdev))
		return ERR_PTR(-ENODEV);

	filp = anon_inode_getfile("vfio_ap_mig_file", fops, matrix_mdev, flags);
	if (IS_ERR(filp)) {
		vfio_device_put_registration(&matrix_mdev->vdev);
		return filp;
	}

	stream_open(filp->f_inode, filp);

	/*
	 * Take a second reference on the file so the driver holds its own
	 * reference independent of the one consumed when the VFIO core
	 * installs the FD into the userspace file table. Without this,
	 * the driver's saved filp could be the only reference; an fput()
	 * during a device reset would prematurely destroy the file while
	 * the userspace FD still points to it.
	 */
	get_file(filp);

	return filp;
}

static ssize_t vfio_ap_resuming_write(struct file *filp, const char __user *buf,
				      size_t len, loff_t *pos)
{
	/* TODO */
	return -EOPNOTSUPP;
}

static const struct file_operations vfio_ap_resume_fops = {
	.owner = THIS_MODULE,
	.write = vfio_ap_resuming_write,
	.release = vfio_ap_release_mig_file,
};

static struct file *vfio_ap_resuming_init(struct ap_matrix_mdev *matrix_mdev)
{
	lockdep_assert_held(&matrix_dev->mdevs_lock);

	return vfio_ap_open_file_stream(matrix_mdev, &vfio_ap_resume_fops, O_WRONLY);
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

		mig_data->stop_copy_mig_file.filp = filp;

		return filp;
	}

	/*
	 * Begins the process of restoring the vfio device state by creating and
	 * returning a streaming data_fd to be used to read in the internal
	 * state of the vfio-ap device on the destination host.
	 */
	if (cur_state == VFIO_DEVICE_STATE_STOP &&
	    new_state == VFIO_DEVICE_STATE_RESUMING) {
		struct file *filp = vfio_ap_resuming_init(matrix_mdev);

		if (IS_ERR(filp))
			return ERR_CAST(filp);

		mig_data->resuming_mig_file.filp = filp;
		return filp;
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
		dev_err_once(matrix_mdev->vdev.dev,
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

	while (mig_data->mig_state != VFIO_DEVICE_STATE_ERROR &&
	       mig_data->mig_state != new_state) {
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
	/* Live guest migration is not supported for SE guests */
	if (ap_is_se_guest())
		return;

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

static void vfio_ap_release_mig_files(struct ap_matrix_mdev *matrix_mdev)
{
	struct vfio_ap_migration_data *mig_data;

	/*
	 * The fput call does not call .release synchronously while the
	 * mdevs_lock mutex is held, so there is no problem with incurring a
	 * deadlock situation if fput is executed in this function.
	 */
	lockdep_assert_held(&matrix_dev->mdevs_lock);

	mig_data = matrix_mdev->mig_data;
	if (!mig_data)
		return;

	if (mig_data->stop_copy_mig_file.filp) {
		fput(mig_data->stop_copy_mig_file.filp);
		mig_data->stop_copy_mig_file.filp = NULL;
	}

	kfree(mig_data->stop_copy_mig_file.ap_config);
	mig_data->stop_copy_mig_file.ap_config = NULL;
	mig_data->stop_copy_mig_file.config_sz = 0;

	if (mig_data->resuming_mig_file.filp) {
		fput(mig_data->resuming_mig_file.filp);
		mig_data->resuming_mig_file.filp = NULL;
	}

	kfree(mig_data->resuming_mig_file.ap_config);
	mig_data->resuming_mig_file.ap_config = NULL;
	mig_data->resuming_mig_file.config_sz = 0;
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

	/*
	 * Drop the driver's get_file() references on any open migration FDs
	 * and free the associated ap_config buffers before freeing mig_data.
	 * This ensures that if the device is closed while a migration FD is
	 * still held by userspace, vfio_ap_release_mig_file() will see
	 * mig_data == NULL and skip the cleanup (the fput() here will
	 * eventually trigger .release, but mig_data is gone by then).
	 */
	vfio_ap_release_mig_files(matrix_mdev);
	kfree(matrix_mdev->mig_data);
	matrix_mdev->mig_data = NULL;
}

/**
 * vfio_ap_reset_migration_state - Reset the vfio-ap migration state
 *
 * @matrix_mdev: pointer to the object maintaining the vfio-ap device state
 *
 * Called during VFIO_DEVICE_RESET to clean up any active migration
 * state and reset the device to RUNNING state as required by the VFIO
 * migration specification.
 */
void vfio_ap_reset_migration_state(struct ap_matrix_mdev *matrix_mdev)
{
	lockdep_assert_held(&matrix_dev->mdevs_lock);

	if (!matrix_mdev->mig_data)
		return;

	vfio_ap_release_mig_files(matrix_mdev);
	matrix_mdev->mig_data->mig_state = VFIO_DEVICE_STATE_RUNNING;
}
