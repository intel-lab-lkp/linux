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

/*
 * Masks the fields of the queue information returned from the PQAP(TAPQ)
 * command. In order to migrate a guest, it's AP configuration must be
 * compatible with AP configuration assigned to the target guest's mdev.
 * This mask is used to verify that the queue information for each source and
 * target queue is compatible (i.e., the masked fields are equivalent).
 *
 * The relevant fields covered by this mask are:
 * S bit 0: APSC  facility installed
 * M bit 1: APQKM facility installed
 * C bit 2: AP4KC facility installed
 * Mode bits 3-5:
 *     D bit 3: CCA-mode facility
 *     A bit 4: accelerator-mode facility
 *     X bit 5: XCP-mode facility
 * N  bit 6: APXA facility installed
 * SL bit 7: SLCF facility installed
 * Classification (functional capabilities) bits 8-16
 *     bit 8: Native card function
 *     bit 9: Only stateless functions
 * BS bits 16-17:
 * AP Type bits 32-40:
 */
#define QINFO_DATA_MASK		0xffffc000ff000000

/*
 * Masks the bit that indicates whether full native card function is available
 * from the 8 bits specifying the functional capabilities of a queue
 */
#define CLASSIFICATION_NATIVE_FCN_MASK		0x80

/* The maximum number of queues that can be installed in an s390 system */
#define MAX_AP_QUEUES				(AP_DEVICES * AP_DOMAINS)

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

static void
vfio_ap_release_resuming_file(struct vfio_ap_migration_data *mig_data)
{
	if (mig_data->resuming_mig_state.filp)
		mig_data->resuming_mig_state.filp = NULL;

	kfree(mig_data->resuming_mig_state.ap_config);
	mig_data->resuming_mig_state.ap_config = NULL;
	mig_data->resuming_mig_state.config_sz = 0;
}

static int vfio_ap_release_mig_file(struct inode *file_inode, struct file *filp)
{
	struct ap_matrix_mdev *matrix_mdev = filp->private_data;

	if (!matrix_mdev || !matrix_mdev->mig_data)
		return -ENODEV;

	if (filp == matrix_mdev->mig_data->stop_copy_mig_file)
		vfio_ap_release_stop_copy_file(matrix_mdev->mig_data);
	else if (filp == matrix_mdev->mig_data->resuming_mig_state.filp)
		vfio_ap_release_resuming_file(matrix_mdev->mig_data);
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
		pr_err("vfio_ap_mdev %s: migration failed: Failed to get hwinfo for queue %02lx.%04lx on target host: TAPQ rc=%d",
		       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn), status.response_code);

		return -ENODEV;
	default:
		/* On a pending async error the tapq info should be available */
		if (!status.async)
			return 0;

		pr_err("vfio_ap_mdev %s: Failed to get hwinfo for queue %02lx.%04lx: TAPQ rc=%d",
		       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn), status.response_code);

		return -EIO;
	}
}

static int vfio_ap_store_queue_info(struct ap_matrix_mdev *matrix_mdev,
				    struct vfio_ap_config *ap_config)
{
	unsigned long *apm, *aqm, num_queues, apid, apqi, apqn;
	struct ap_tapq_hwinfo source_hwinfo;
	const char *mdev_name;
	int ret;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	mdev_name = dev_name(matrix_mdev->vdev.dev);
	apm = matrix_mdev->shadow_apcb.apm;
	aqm = matrix_mdev->shadow_apcb.aqm;
	num_queues = 0;

	for_each_set_bit_inv(apid, apm, AP_DEVICES) {
		for_each_set_bit_inv(apqi, aqm, AP_DOMAINS) {
			apqn = AP_MKQID(apid, apqi);

			ret = get_hardware_info_for_queue(mdev_name,
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

static int validate_resuming_write_parms(struct file *filp,
					 size_t len, loff_t *pos)
{
	struct ap_matrix_mdev *matrix_mdev;
	loff_t total_len;
	int ret;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	matrix_mdev = filp->private_data;
	if (!matrix_mdev || !matrix_mdev->mig_data) {
		ret = -ENODEV;
		goto done;
	}

	if (filp != matrix_mdev->mig_data->resuming_mig_state.filp) {
		ret = -ENXIO;
		goto done;
	}

	if (*pos < 0) {
		ret = -EINVAL;
		goto done;
	}

	if (check_add_overflow((loff_t)len, *pos, &total_len)) {
		ret = -ERANGE;
		goto done;
	}

	/*
	 * If the ap_config has not yet been allocated and the file position
	 * indicates this is not the first write, or the ap_config has been allocated
	 * but the file position indicates this is the first write, then this is an
	 * error condition.
	 */
	if ((!matrix_mdev->mig_data->resuming_mig_state.ap_config && *pos != 0) ||
	    (matrix_mdev->mig_data->resuming_mig_state.ap_config && *pos == 0)) {
		ret = -EFAULT;

		goto done;
	}

	ret = 0;

done:
	return ret;
}

static ssize_t calculate_ap_config_size(unsigned int num_queues)
{
	size_t qinfo_size;

	if (num_queues > MAX_AP_QUEUES)
		return -EINVAL;

	qinfo_size = num_queues * sizeof(struct vfio_ap_queue_info);
	return qinfo_size + sizeof(struct vfio_ap_config);
}

/**
 * allocate_ap_config:
 *
 * Allocate storage for the source guest's AP configuration data sent from
 * userspace.
 *
 * @ap_config:	The location in which to store the pointer to the storage
 *		allocated for the AP configuration data.
 * @buf:	The userspace buffer containing some or all of the source
 *		guest's AP configuration data
 * @len:	The number of bytes of data to copy from @buf
 *
 * Returns:	The number of bytes of storage allocated for the config data or
 *		an error:
 *
 *
 *		-EIO: failed to copy data from @buf
 *		-EINVAL: the number of queues specified exceeds the max allowed
 *		-ENOMEM: the allocation of storage failed
 */
static ssize_t allocate_ap_config(struct vfio_ap_config **ap_config,
				  const char __user *buf, size_t len)
{
	struct vfio_ap_config tmp_ap_config;
	ssize_t config_size;
	size_t copy_size;

	/*
	 * If the length of the data sent exceeds the size of the vfio_ap_config
	 * structure, then we will copy enough data from userspace to get the
	 * number of queues which we can use to allocate enough space all of
	 * the queue information.
	 */
	copy_size = min(len, sizeof(tmp_ap_config));

	if (copy_from_user(&tmp_ap_config, buf, copy_size))
		return -EIO;

	/*
	 * If the length of data sent includes the number of queues
	 * in the AP configuration, then calculate its size; otherwise
	 * set config_size to the length of data sent.
	 */
	if (len >= sizeof(struct vfio_ap_config)) {
		config_size = calculate_ap_config_size(tmp_ap_config.num_queues);

		/* If the calculation returned an error */
		if (config_size < 0)
			return config_size;
	} else {
		config_size = len;
	}

	*ap_config = kzalloc(config_size, GFP_KERNEL_ACCOUNT);
	if (!*ap_config)
		return -ENOMEM;

	return config_size;
}

/**
 * reallocate_ap_config:
 *
 * Reallocate the storage buffer so it is large enough to store the source
 * guest's AP configuration data sent from userspace.
 *
 * @mig_ap_config: The location in which to store the pointer to the storage
 *		   reallocated for the AP configuration data.
 * len:		   The length of the data to be stored
 *
 * Returns:	   The size of the memory allocated for the source guest's
 *		   AP configuration data or an error:
 *
 *		   -ENOMEM: The call to krealloc failed
 *		   -EINVAL: The guest's AP configuration size changed between
 *		   calls to the vfio_ap_resuming_write function.
 *
 */
static ssize_t reallocate_ap_config(struct vfio_ap_config **mig_ap_config,
				    size_t len)
{
	struct vfio_ap_config *ap_config = *mig_ap_config;
	struct vfio_ap_config *new_ap_config;
	size_t new_cfg_sz, cur_cfg_sz;
	unsigned int num_queues;

	cur_cfg_sz = ap_config->config_sz;
	num_queues = ap_config->num_queues;

	/*
	 * If the current configuration size is greater than the
	 * size of a vfio_ap_config structure (i.e., contains the num_queues
	 * field), then there should already be enough storage allocated
	 * to store the source guest's AP configuration. Let's verify that the
	 * amount of storage allocated is what we expect based on the number of
	 * vfio_ap_queue_info objects that must be stored.
	 */
	if (cur_cfg_sz >= sizeof(struct vfio_ap_config)) {
		new_cfg_sz = calculate_ap_config_size(num_queues);
		if (cur_cfg_sz != new_cfg_sz)
			return -EINVAL;
	} else {
		new_cfg_sz = cur_cfg_sz + len;
	}

	new_ap_config = krealloc(ap_config, new_cfg_sz, GFP_KERNEL_ACCOUNT);
	if (!new_ap_config)
		return -ENOMEM;

	*mig_ap_config = new_ap_config;

	return new_cfg_sz;
}

/**
 * qdev_is_bound_to_vfio_ap:
 *
 * Query to determine whether a queue with the specified APQN is available on
 * the host system and bound to the vfio_ap device driver.
 *
 * @apqn: The APQN of the queue device being queried
 *
 * Returns: True if there is a queue device with the specified @apqn installed
 *	    in the system and is bound to the vfio_ap device driver; otherwise,
 *	    returns false.
 */
static bool qdev_is_bound_to_vfio_ap(unsigned int apqn)
{
	struct ap_queue *queue;
	bool is_bound = true;

	queue = ap_get_qdev(apqn);
	if (!queue)
		return false;

	if (queue->ap_dev.device.driver != &matrix_dev->vfio_ap_drv->driver)
		is_bound = false;

	put_device(&queue->ap_dev.device);

	return is_bound;
}

/**
 * queues_available_on_target_system:
 *
 * Query whether each queue from the source guest's AP configuration is
 * available and bound to the vfio_ap device driver; if not, log an error
 * message.
 *
 * @mdev_name:	   The mdev name to use in error messages
 * @source_config: The object specifying the source guest's AP configuration
 *
 * Returns: true if each queue identified in @source_config is available and
 *	    bound to the vfio_ap device driver; otherwise, returns false.
 */
static bool
queues_available_on_target_system(const char *mdev_name,
				  struct vfio_ap_config *source_config)
{
	unsigned long apqn;
	bool ret = true;

	for (int i = 0; i < source_config->num_queues; i++) {
		apqn = source_config->qinfo[i].apqn;

		/*
		 * Find the queue device bound to the vfio_ap device driver. If it is
		 * not found, log an error and continue so users see all problems
		 * at once, not one-at-a-time through retries of the migration.
		 */
		if (!qdev_is_bound_to_vfio_ap(apqn)) {
			pr_err("vfio_ap_mdev %s: Queue %02lx.%04lx not available to vfio_ap driver on target host\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
			ret = false;
		}
	}

	return ret;
}

static void report_facilities_compatibility(const char *mdev_name,
					    unsigned long apqn,
					    struct ap_tapq_hwinfo *src_hwinfo,
					    struct ap_tapq_hwinfo *target_hwinfo)
{
	if (src_hwinfo->apsc != target_hwinfo->apsc) {
		if (src_hwinfo->apsc) {
			pr_err("vfio_ap_mdev %s: APSC facility installed in source queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err("vfio_ap_mdev %s: APSC facility not installed in target queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			pr_err("vfio_ap_mdev %s: APSC facility not installed in source queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err("vfio_ap_mdev %s APSC facility installed in target queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}

	if (src_hwinfo->mex4k != target_hwinfo->mex4k) {
		if (src_hwinfo->mex4k) {
			pr_err("vfio_ap_mdev %s: mex4k facility installed in source queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err("vfio_ap_mdev %s: mex4k facility not installed in target queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			pr_err("vfio_ap_mdev %s: mex4k facility not installed in source queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err("vfio_ap_mdev %s: mex4k facility installed in target queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}

	if (src_hwinfo->crt4k != target_hwinfo->crt4k) {
		if (src_hwinfo->crt4k) {
			pr_err("vfio_ap_mdev %s: crt4k facility installed in source queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err("vfio_ap_mdev %s: crt4k facility not installed in target queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			pr_err("vfio_ap_mdev %s: crt4k facility not installed in source queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err("vfio_ap_mdev %s: crt4k facility installed in target queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}
}

static void report_mode_compatibility(const char *mdev_name,
				      unsigned long apqn,
				      struct ap_tapq_hwinfo *src_hwinfo,
				      struct ap_tapq_hwinfo *target_hwinfo)
{
	if (src_hwinfo->cca != target_hwinfo->cca) {
		if (src_hwinfo->cca) {
			pr_err("vfio_ap_mdev %s: Coprocessor-mode facility installed in source queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err("vfio_ap_mdev %s: Coprocessor-mode  facility not installed target queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			pr_err("vfio_ap_mdev %s: Coprocessor-mode facility not installed in source queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err("vfio_ap_mdev %s: Coprocessor-mode  facility installed target queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}

	if (src_hwinfo->accel != target_hwinfo->accel) {
		if (src_hwinfo->accel) {
			pr_err("vfio_ap_mdev %s: Accelerator-mode facility installed source queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err("vfio_ap_mdev %s: Accelerator-mode facility not installed target queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			pr_err("vfio_ap_mdev %s: Accelerator-mode facility not installed source queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err("vfio_ap_mdev %s: Accelerator-mode facility installed target queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}

	if (src_hwinfo->ep11 != target_hwinfo->ep11) {
		if (src_hwinfo->ep11) {
			pr_err("vfio_ap_mdev %s: XCP-mode facility installed source queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err("vfio_ap_mdev %s: XCP-mode facility not installed target queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			pr_err("vfio_ap_mdev %s: XCP-mode facility not installed source queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err("vfio_ap_mdev %s: XCP-mode facility installed target queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}
}

static void report_apxa_compatibility(const char *mdev_name,
				      unsigned long apqn,
				      struct ap_tapq_hwinfo *src_hwinfo,
				      struct ap_tapq_hwinfo *target_hwinfo)
{
	if (src_hwinfo->apxa != target_hwinfo->apxa) {
		if (src_hwinfo->apxa) {
			pr_err("vfio_ap_mdev %s: AP-extended-addressing (APXA) facility installed in source queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err("vfio_ap_mdev %s: AP-extended-addressing (APXA) facility not installed in target queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			pr_err("vfio_ap_mdev %s: AP-extended-addressing (APXA) facility not installed in source queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err("vfio_ap_mdev %s: AP-extended-addressing (APXA) facility installed in target queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}
}

static void report_slcf_compatibility(const char *mdev_name,
				      unsigned long apqn,
				      struct ap_tapq_hwinfo *src_hwinfo,
				      struct ap_tapq_hwinfo *target_hwinfo)
{
	if (src_hwinfo->slcf != target_hwinfo->slcf) {
		if (src_hwinfo->slcf) {
			pr_err("vfio_ap_mdev %s: Stateless-command-filtering (SLCF) available in source queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err("vfio_ap_mdev %s: Stateless-command-filtering (SLCF) not available in target queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			pr_err("vfio_ap_mdev %s: Stateless-command-filtering (SLCF) not available in source queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err("vfio_ap_mdev %s: Stateless-command-filtering (SLCF) available in target queue %02lx.%04lx\n",
			       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}
}

static void report_bs_compatibility(const char *mdev_name,
				    unsigned long apqn,
				    struct ap_tapq_hwinfo *src_hwinfo,
				    struct ap_tapq_hwinfo *target_hwinfo)
{
	/*
	 * The BS field on both the source and destination must be 0, so if one of
	 * them is not, then report an error.
	 */
	if (src_hwinfo->bs || target_hwinfo->bs) {
		pr_err("vfio_ap_mdev %s: Bind/associate state for source (%01x) and target (%01x) queue %02lx.%04lx must be 0\n",
		       mdev_name, src_hwinfo->bs, target_hwinfo->bs,
		       AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
	}
}

static void report_aptype_compatibility(const char *mdev_name,
					unsigned long apqn,
					struct ap_tapq_hwinfo *src_hwinfo,
					struct ap_tapq_hwinfo *target_hwinfo)
{
	if (src_hwinfo->at > target_hwinfo->at) {
		pr_err("vfio_ap_mdev %s: AP type of source (%02x) not compatible with target (%02x)\n",
		       mdev_name, src_hwinfo->at, target_hwinfo->at);
	}
}

static bool classes_compatible(struct ap_tapq_hwinfo *src_hwinfo,
			       struct ap_tapq_hwinfo *target_hwinfo)
{
	unsigned long src_native, target_native;

	src_native = src_hwinfo->class & CLASSIFICATION_NATIVE_FCN_MASK;
	target_native = target_hwinfo->class & CLASSIFICATION_NATIVE_FCN_MASK;

	/*
	 * If the source queue has full native card function and the
	 * target queue has only stateless functions available, then
	 * there may be instructions that will not execute on the
	 * target queue. This shall be reported as an error.
	 *
	 * If the source queue has only stateless card functions and the
	 * target queue has full native card function available, then
	 * we are okay because the target queue can run all stateless card
	 * functions.
	 */
	return (src_native != target_native) ? !src_native : true;
}

static void report_class_compatibility(const char *mdev_name,
				       unsigned long apqn,
				       struct ap_tapq_hwinfo *src_hwinfo,
				       struct ap_tapq_hwinfo *target_hwinfo)
{
	if (!classes_compatible(src_hwinfo, target_hwinfo)) {
		pr_err("vfio_ap_mdev %s: Full native card function available on source queue %02lx.%04lx\n",
		       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

		pr_err("vfio_ap_mdev %s: Only stateless functions available on target queue %02lx.%04lx\n",
		       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
	}
}

/*
 * Log a device error reporting that migration failed due to queue
 * incompatibilities followed by a device error for each incompatible feature.
 */
static void report_qinfo_incompatibilities(const char *mdev_name,
					   unsigned long apqn,
					   struct ap_tapq_hwinfo *src_hwinfo,
					   struct ap_tapq_hwinfo *target_hwinfo)
{
	pr_err("vfio_ap_mdev %s: Migration failed: Source and target queue (%02lx.%04lx) not compatible\n",
	       mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

	report_facilities_compatibility(mdev_name, apqn, src_hwinfo, target_hwinfo);
	report_mode_compatibility(mdev_name, apqn, src_hwinfo, target_hwinfo);
	report_apxa_compatibility(mdev_name, apqn, src_hwinfo, target_hwinfo);
	report_slcf_compatibility(mdev_name, apqn, src_hwinfo, target_hwinfo);
	report_aptype_compatibility(mdev_name, apqn, src_hwinfo, target_hwinfo);
	report_bs_compatibility(mdev_name, apqn, src_hwinfo, target_hwinfo);
	report_class_compatibility(mdev_name, apqn, src_hwinfo, target_hwinfo);
}

/**
 * queue_hardware_info_is_compatible:
 *
 * Verify whether the hardware information for a source queue is compatible with
 * the hardware info for the corresponding queue on this system.
 *
 * In order to be compatible, the hardware information for each queue must
 * meet the following requirements:
 *
 * 1. The hardware facilities bits much match
 * 2. The AP type of the source queue must be the same as or older than that
 *    of the target queue (target is backwards compatible)
 * 3. The classification bits must indicate:
 *    - Both queues have full native card function or both have stateless
 *      functions available
 *    - If the classification bits don't match, then the only acceptable
 *      configuration is stateless functions for the source queue and
 *      full native function for the target queue
 * 4. The BS bits for both queues must be 0 (Queue usable for all messages
 *    supported by the adapter)
 *
 * @mdev_name:	The mdev name to use in error messages
 * @apqn:	The APQN for the queues
 * @src_hwinfo: The hardware info for the source queue
 * @target_hwinfo: The hardware info for the corresponding queue on this system
 *
 * Returns: true if the hardware info for the two queues is compatible;
 *          otherwise, returns false.
 */
static bool queue_hardware_info_is_compatible(const char *mdev_name,
					      unsigned long apqn,
					      struct ap_tapq_hwinfo *src_hwinfo,
					      struct ap_tapq_hwinfo *target_hwinfo)
{
	unsigned long src_bits, target_bits;

	src_bits = src_hwinfo->value & QINFO_DATA_MASK;
	target_bits = target_hwinfo->value & QINFO_DATA_MASK;

	/* If all bits match the queues are compatible */
	if (src_bits == target_bits)
		return true;

	if (src_hwinfo->fac == target_hwinfo->fac &&
	    src_hwinfo->at <= target_hwinfo->at &&
	    classes_compatible(src_hwinfo, target_hwinfo) &&
	    (src_hwinfo->bs == 0 && target_hwinfo->bs == 0)) {
		return true;
	}

	report_qinfo_incompatibilities(mdev_name, apqn, src_hwinfo, target_hwinfo);

	return false;
}

/**
 * verify_ap_configs_are_compatible:
 *
 * Verifies that the queues in the source guest's AP configuration are
 * compatible with the corresponding queues on this system.
 *
 * @mdev_name:	   The mdev name to use in error messages
 * @source_config: The object specifying the source guest's AP configuration
 *
 * Returns: an error indicating either a failure to retrieve a queue's
 *			hardware information or one or more source queues are not
 *			compatible with the corresponding queue on this system; otherwise,
 *			returns zero to indicate compatibility.
 */
static int verify_ap_configs_are_compatible(const char *mdev_name,
					    struct vfio_ap_config *source_config)
{
	struct ap_tapq_hwinfo src_hwinfo, dest_hwinfo;
	unsigned long apqn;
	int ret = 0, rc;

	for (int i = 0; i < source_config->num_queues; i++) {
		apqn = source_config->qinfo[i].apqn;

		/*
		 * If we can't get the hardware info for a particular queue, then let's
		 * capture the function return code and continue so we can log all
		 * errors to aid in debugging of migration.
		 */
		rc = get_hardware_info_for_queue(mdev_name, &dest_hwinfo, apqn);
		if (rc) {
			ret = rc;
			continue;
		}

		src_hwinfo.value =  source_config->qinfo[i].data;

		if (!queue_hardware_info_is_compatible(mdev_name, apqn,
						       &src_hwinfo,
						       &dest_hwinfo))
			ret = -EFAULT;
	}

	return ret;
}

static int do_post_copy_validation(const char *mdev_name,
				   struct vfio_ap_config *source_config)
{
	if (!queues_available_on_target_system(mdev_name, source_config))
		return -ENODEV;

	return verify_ap_configs_are_compatible(mdev_name, source_config);
}

/**
 * setup_ap_matrix_from_ap_config:
 *
 * Set the bits corresponding to the adapters, domains and control domains
 * in the source guest's AP configuration into an ap_matrix object to be
 * used to update the target guest to run on this host. An error message will
 * be logged for each adapter, domain or control domain that is not available
 * on this host. Returning an error after the each error may result in needing
 * to initiate multiple migrations in order to find and fix each of them.
 *
 * Returns: zero (0) if each adapter, domain and control domain from the
 *          source guest's ap configuration is available on this host;
 *          otherwise, returns -ENODEV.
 */
static void setup_ap_matrix_from_ap_config(const char *mdev_name,
					   struct vfio_ap_config *ap_config,
					   struct ap_matrix *guest_matrix)
{
	struct vfio_ap_queue_info qinfo;
	unsigned long apid, apqi;

	for (int i = 0; i < ap_config->num_queues; i++) {
		qinfo = ap_config->qinfo[i];
		apid = AP_QID_CARD(qinfo.apqn);
		apqi = AP_QID_QUEUE(qinfo.apqn);

		if (!test_bit_inv(apid, guest_matrix->apm))
			set_bit_inv(apid, guest_matrix->apm);
		if (!test_bit_inv(apqi, guest_matrix->aqm))
			set_bit_inv(apqi, guest_matrix->aqm);
	}
}

static ssize_t vfio_ap_resuming_write(struct file *filp, const char __user *buf,
				      size_t len, loff_t *pos)
{
	struct vfio_ap_migration_data *mig_data;
	struct ap_matrix_mdev *matrix_mdev;
	struct vfio_ap_config *ap_config;
	struct ap_matrix guest_matrix;
	ssize_t ret = 0, cfg_sz;
	const char *mdev_name;

	/*
	 * When userspace calls write() with an explicit offset (pwrite), pos is
	 * non-NULL and the function rejects it with -ESPIPE (illegal seek). For
	 * normal write() calls, pos is NULL, so we'll use the file's internal
	 * position filp->f_pos
	 */
	if (pos)
		return -ESPIPE;

	mutex_lock(&matrix_dev->mdevs_lock);
	pos = &filp->f_pos;

	ret = validate_resuming_write_parms(filp, len, pos);
	if (ret)
		goto done;

	matrix_mdev = filp->private_data;
	mig_data = matrix_mdev->mig_data;
	mdev_name = dev_name(matrix_mdev->vdev.dev);

	/*
	 * If this is the first write operation, then allocate storage for the
	 * AP configuration information; otherwise, reallocate the
	 * struct vfio_ap_config object used to store the AP configuration data
	 * sent from userspace.
	 */
	if (*pos == 0) {
		ret = allocate_ap_config(&ap_config, buf, len);

		/* If the allocation failed, we'll return the error */
		if (ret < 0)
			goto done;

		cfg_sz = ret;
	} else {
		ap_config = mig_data->resuming_mig_state.ap_config;

		ret = reallocate_ap_config(&ap_config, len);
		if (ret < 0)
			goto cleanup;

		cfg_sz = ret;
	}

	if (*pos + len > cfg_sz) {
		ret = -EIO;
		goto cleanup;
	}

	/*
	 * We don't want to lock all mdevs while copying data from userspace so
	 * we don't block all other mdevs in case the I/O takes a long time.
	 * From here on out we don't need this lock because we are not
	 * accessing the matrix_mdev until we need to get the update
	 * locks to set the new destination guest's AP configuration in which
	 * case this lock will be taken then.
	 */
	mutex_unlock(&matrix_dev->mdevs_lock);

	if (copy_from_user((char *)ap_config + *pos, buf, len)) {
		ret = -EIO;
		goto cleanup;
	}

	/* Check if we've completed writing the entire configuration */
	if (*pos + len == cfg_sz) {
		ret = do_post_copy_validation(mdev_name, ap_config);
		if (ret < 0)
			goto cleanup;

		setup_ap_matrix_from_ap_config(mdev_name, ap_config,
					       &guest_matrix);

		/* Acquire locks required to update the guest's AP config */
		mutex_lock(&ap_attr_mutex);
		get_update_locks_for_mdev(matrix_mdev);

		ret = vfio_ap_set_new_guest_config(matrix_mdev, &guest_matrix,
						   false);
		if (!ret) {
			mig_data->resuming_mig_state.ap_config = ap_config;
			mig_data->resuming_mig_state.config_sz = cfg_sz;
		}

		release_update_locks_for_mdev(matrix_mdev);
		mutex_unlock(&ap_attr_mutex);

		if (ret)
			goto cleanup;
	}

	ret = len;
	*pos += len;
	goto done;

cleanup:
	kfree(ap_config);
done:
	if (mutex_is_locked(&matrix_dev->mdevs_lock))
		mutex_unlock(&matrix_dev->mdevs_lock);

	return ret;
}

static const struct file_operations vfio_ap_resume_fops = {
	.owner = THIS_MODULE,
	.write = vfio_ap_resuming_write,
	.release = vfio_ap_release_mig_file,
};

static struct file *vfio_ap_resuming_init(struct ap_matrix_mdev *matrix_mdev)
{
	struct vfio_ap_migration_data *mig_data;
	struct file *filp;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	mig_data = matrix_mdev->mig_data;
	filp = vfio_ap_open_file_stream(matrix_mdev, &vfio_ap_resume_fops, O_WRONLY);

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

		mig_data->resuming_mig_state.filp = filp;

		return filp;
	}

	/*
	 * Terminates the data transfer session of the vfio-ap device state
	 * between the source and target hosts. Since the vfio-ap device does
	 * not virtualize a DMA device, there is no internal device state to
	 * incorporate into the vfio-ap device on the target.
	 */
	if ((cur_state == VFIO_DEVICE_STATE_RESUMING &&
	     new_state == VFIO_DEVICE_STATE_STOP) ||
	    (cur_state == VFIO_DEVICE_STATE_STOP_COPY &&
	     new_state == VFIO_DEVICE_STATE_STOP)) {
		return NULL;
	}

	/*
	 * These states indicate migration has either not been initiated or
	 * has completed and the vfio-ap device is operating normally.Since the
	 * vfio-ap device does not virtualize a DMA device, there is no internal
	 * device state to incorporate into the vfio-ap device on the target.
	 */
	if ((cur_state == VFIO_DEVICE_STATE_STOP &&
	     new_state == VFIO_DEVICE_STATE_RUNNING) ||
	    (cur_state == VFIO_DEVICE_STATE_RUNNING &&
	     new_state == VFIO_DEVICE_STATE_STOP)) {
		return NULL;
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
