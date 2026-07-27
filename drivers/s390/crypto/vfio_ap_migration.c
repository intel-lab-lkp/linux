// SPDX-License-Identifier: GPL-2.0
/*
 * Drives vfio_ap mdev migration.
 *
 * Copyright IBM Corp. 2025
 */
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include "vfio_ap_private.h"

/*
 * Masks the fields of the queue information returned from the PQAP(TAPQ)
 * command. In order to migrate a guest, it's AP configuration must be
 * compatible with AP configuration assigned to the target guest's mdev.
 * This mask is used to verify that the queue information for each source and
 * target queue is compatible.
 *
 * The following bits must match for the source device and the corresponding
 * destination device:
 * -------------------------------------------------------------------------
 * S bit 0: APSC  facility installed
 * M bit 1: APQKM facility installed
 * C bit 2: AP4KC facility installed
 * Mode bits 3-5:
 *     D bit 3: CCA-mode facility
 *     A bit 4: accelerator-mode facility
 *     X bit 5: XCP-mode facility
 * N  bit 6: APXA facility installed
 * SL bit 7: SLCF facility installed
 *
 * Either bit 8 or bit 9 will be set. If bit 8 is set for the source device,
 * then it must also be set for the corresponding destination device:
 * -------------------------------------------------------------------------
 * Classification (functional capabilities) bits 8-16
 *     bit 8: Native card function
 *     bit 9: Only stateless functions
 *
 * The BS bits must be set to 0 for both the source and corresponding
 * destination device:
 * -------------------------------------------------------------------------
 * BS bits 16-17:
 *
 * The AP type of the source device must be less than or equal to that of
 * the corresponding destination device:
 * -------------------------------------------------------------------------
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
	kvfree(mig_data->resuming_mig_file.ap_config);
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
	unsigned long *apm, *aqm, apid, apqi;
	struct vfio_ap_config *ap_configuration;
	const char *mdev_name;
	size_t ap_config_size;
	int num_queues;
	int ret;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	ap_config_size = vfio_ap_config_size(matrix_mdev, &num_queues);

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

static int validate_resuming_write_parms(struct file *filp,
					 size_t len, loff_t *pos)
{
	struct ap_matrix_mdev *matrix_mdev;
	loff_t total_len;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	if (!len || *pos < 0)
		return -EINVAL;

	if (check_add_overflow((loff_t)len, *pos, &total_len))
		return -ERANGE;

	matrix_mdev = filp->private_data;
	if (!matrix_mdev || !matrix_mdev->mig_data)
		return -ENODEV;

	if (filp != matrix_mdev->mig_data->resuming_mig_file.filp)
		return -ENXIO;

	/*
	 * If the ap_config has not yet been allocated and the file position
	 * indicates this is not the first write, or the ap_config has been allocated
	 * but the file position indicates this is the first write, then this is an
	 * error condition.
	 */
	if ((!matrix_mdev->mig_data->resuming_mig_file.ap_config && *pos != 0) ||
	    (matrix_mdev->mig_data->resuming_mig_file.ap_config && *pos == 0))
		return -EFAULT;

	/*
	 * The first write must cover at least num_queues (the first field of
	 * struct vfio_ap_config) so that allocate_ap_config() can derive the
	 * correct allocation size.  A shorter first write would cause
	 * cfg_sz to be set to len, the completion check
	 * (write_pos + len == cfg_sz) would fire immediately, and
	 * do_post_copy_validation() would read qinfo[] from a buffer that
	 * is too small to contain it.
	 */
	if (*pos == 0 && len < offsetofend(struct vfio_ap_config, num_queues))
		return -EINVAL;

	return 0;
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
 *		-EINVAL: len is 0, or num_queues exceeds the maximum (only checked
 *			 if @len covers the full vfio_ap_config header)
 *		-EIO: failed to copy data from @buf
 *		-ENOMEM: the allocation of storage failed
 */
static ssize_t allocate_ap_config(struct vfio_ap_config **ap_config,
				  const char __user *buf, size_t len)
{
	struct vfio_ap_config tmp_ap_config;
	ssize_t config_size;

	/*
	 * validate_resuming_write_parms() guarantees the first write covers at
	 * least num_queues, so we can always derive the final allocation size
	 * here.
	 */
	if (copy_from_user(&tmp_ap_config, buf, min(len, sizeof(tmp_ap_config))))
		return -EIO;

	config_size = calculate_ap_config_size(tmp_ap_config.num_queues);
	if (config_size < 0)
		return config_size;

	/*
	 * Use kvzalloc so that large configurations can fall back to vmalloc
	 * rather than failing a high-order contiguous physical allocation.
	 */
	*ap_config = kvzalloc(config_size, GFP_KERNEL_ACCOUNT);
	if (!*ap_config)
		return -ENOMEM;

	return config_size;
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
 * queues_available:
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
static bool queues_available(const char *mdev_name,
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

/**
 * control_domains_available
 *
 * Query whether each control domain specified in the source guest's AP
 * configuration is installed in the host system.
 *
 * @mdev_name:		The name of the mdev to use when logging messages
 * @source_config:	The object specifying the source guest's AP config
 *
 * Returns:	True if each control domain is installed; otherwise, logs an
 *		error message for each unavailable control domain and returns
 *		false.
 */
static bool control_domains_available(const char *mdev_name,
				      struct vfio_ap_config *source_config)
{
	unsigned long domain_num;
	bool available = true;

	for_each_set_bit_inv(domain_num, (unsigned long *)source_config->adm,
			     AP_DOMAINS) {
		if (!ap_test_config_ctrl_domain(domain_num)) {
			pr_err("vfio_ap_mdev: %s: Control domain %04lx not available on the destination host",
			       mdev_name, domain_num);
			available = false;
		}
	}

	return available;
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
	 * The BS field on both the source and destination must be 0, so if one
	 * of them is not, then report an error.
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
	if (src_bits == target_bits &&
	    (src_hwinfo->bs == 0 && target_hwinfo->bs == 0))
		return true;

	if (src_hwinfo->apsc  == target_hwinfo->apsc     &&
	    src_hwinfo->mex4k == target_hwinfo->mex4k    &&
	    src_hwinfo->crt4k == target_hwinfo->crt4k    &&
	    src_hwinfo->cca   == target_hwinfo->cca      &&
	    src_hwinfo->accel == target_hwinfo->accel    &&
	    src_hwinfo->ep11  == target_hwinfo->ep11     &&
	    src_hwinfo->slcf  == target_hwinfo->slcf     &&
	    src_hwinfo->apxa  == target_hwinfo->apxa     &&
	    src_hwinfo->at    <= target_hwinfo->at       &&
	    classes_compatible(src_hwinfo, target_hwinfo) &&
	    (src_hwinfo->bs == 0 && target_hwinfo->bs == 0))
		return true;

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
			ret = -EINVAL;
	}

	return ret;
}

static int do_post_copy_validation(const char *mdev_name,
				   struct vfio_ap_config *source_config)
{
	if (!queues_available(mdev_name, source_config))
		return -ENODEV;

	if (!control_domains_available(mdev_name, source_config))
		return -ENODEV;

	return verify_ap_configs_are_compatible(mdev_name, source_config);
}

/**
 * setup_ap_matrix_from_ap_config:
 *
 * Set the bits corresponding to the adapters, domains and control domains
 * from the source guest's AP configuration into an ap_matrix object to be
 * used to update the destination guest to run on this host.
 *
 * @ap_config:		The source guest's AP configuration
 * @guest_matrix:	The object to be used to update the destination guest's
 *			AP configuration
 */
static void setup_ap_matrix_from_ap_config(struct vfio_ap_config *ap_config,
					   struct ap_matrix *guest_matrix)
{
	struct ap_config_info host_config_info = { 0 };
	unsigned long apid, apqi, *guest_adm;
	struct vfio_ap_queue_info qinfo;

	ap_qci(&host_config_info);
	/*
	 * Zero the bitmaps before calling vfio_ap_matrix_init(), which only
	 * sets the apm_max/aqm_max/adm_max scalar fields and leaves the bitmap
	 * arrays untouched.  Without this, stack garbage in guest_matrix->apm,
	 * ->aqm, and ->adm would grant the destination guest access to
	 * arbitrary unassigned queues and control domains.
	 */
	memset(guest_matrix->apm, 0, sizeof(guest_matrix->apm));
	memset(guest_matrix->aqm, 0, sizeof(guest_matrix->aqm));
	memset(guest_matrix->adm, 0, sizeof(guest_matrix->adm));
	vfio_ap_matrix_init(&host_config_info, guest_matrix);

	for (int i = 0; i < ap_config->num_queues; i++) {
		qinfo = ap_config->qinfo[i];
		apid = AP_QID_CARD(qinfo.apqn);
		apqi = AP_QID_QUEUE(qinfo.apqn);

		if (!test_bit_inv(apid, guest_matrix->apm))
			set_bit_inv(apid, guest_matrix->apm);
		if (!test_bit_inv(apqi, guest_matrix->aqm))
			set_bit_inv(apqi, guest_matrix->aqm);
	}

	guest_adm = (unsigned long *)ap_config->adm;
	for_each_set_bit_inv(apqi, guest_adm, AP_DOMAINS) {
		if (!test_bit_inv(apqi, guest_matrix->adm))
			set_bit_inv(apqi, guest_matrix->adm);
	}
}

static ssize_t vfio_ap_resuming_write(struct file *filp, const char __user *buf,
				      size_t len, loff_t *pos)
{
	struct ap_matrix_mdev *matrix_mdev;
	struct vfio_ap_config *ap_config;
	struct ap_matrix guest_matrix;
	bool new_allocation = false;
	loff_t write_pos;
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
	if (ret) {
		mutex_unlock(&matrix_dev->mdevs_lock);
		return ret;
	}

	matrix_mdev = filp->private_data;
	mdev_name = dev_name(matrix_mdev->vdev.dev);

	/*
	 * If this is the first write operation, allocate storage for the AP
	 * configuration sized to fit the full payload (validate_resuming_write_parms
	 * guarantees num_queues is present in buf).  For subsequent writes the
	 * buffer is already correctly sized; just reuse it.
	 */
	if (*pos == 0) {
		ret = allocate_ap_config(&ap_config, buf, len);
		if (ret < 0) {
			mutex_unlock(&matrix_dev->mdevs_lock);
			return ret;
		}

		cfg_sz = ret;
		new_allocation = true;
	} else {
		ap_config = matrix_mdev->mig_data->resuming_mig_file.ap_config;
		cfg_sz = matrix_mdev->mig_data->resuming_mig_file.config_sz;
	}

	if (*pos + len > cfg_sz) {
		if (new_allocation)
			kvfree(ap_config);
		mutex_unlock(&matrix_dev->mdevs_lock);
		return -EIO;
	}

	/*
	 * Snapshot and advance *pos under the lock before dropping it for
	 * copy_from_user().  This prevents concurrent write()s on the same
	 * stream file from computing the same destination offset and clobbering
	 * each other's data or racing to reassign mig_data->resuming_mig_file.
	 */
	write_pos = *pos;
	*pos += len;

	mutex_unlock(&matrix_dev->mdevs_lock);

	if (copy_from_user((char *)ap_config + write_pos, buf, len)) {
		if (new_allocation)
			kvfree(ap_config);
		return -EIO;
	}

	/* Check if we've completed writing the entire configuration */
	if (write_pos + len == cfg_sz) {
		/*
		 * do_post_copy_validation() calls ap_tapq() which is a slow
		 * hardware instruction.  Run it before acquiring the update
		 * locks to avoid holding guests_lock, kvm->lock, and
		 * mdevs_lock across the hardware calls.
		 */
		ret = do_post_copy_validation(mdev_name, ap_config);
		if (ret < 0) {
			if (new_allocation)
				kvfree(ap_config);
			return ret;
		}

		setup_ap_matrix_from_ap_config(ap_config, &guest_matrix);

		mutex_lock(&ap_attr_mutex);
		get_update_locks_for_mdev(matrix_mdev);

		/*
		 * Verify the device wasn't closed while mdevs_lock was dropped
		 * for the copy_from_user and do_post_copy_validation above.
		 * get_update_locks_for_mdev() reacquires mdevs_lock.
		 */
		if (!matrix_mdev->mig_data) {
			release_update_locks_for_mdev(matrix_mdev);
			mutex_unlock(&ap_attr_mutex);
			if (new_allocation)
				kvfree(ap_config);
			return -ENODEV;
		}

		ret = vfio_ap_set_new_guest_config(matrix_mdev, &guest_matrix);

		release_update_locks_for_mdev(matrix_mdev);
		mutex_unlock(&ap_attr_mutex);

		if (ret) {
			if (new_allocation)
				kvfree(ap_config);
			return ret;
		}
	}

	mutex_lock(&matrix_dev->mdevs_lock);
	/*
	 * Re-read mig_data under the lock; the device could have been closed
	 * concurrently while the lock was dropped for copy_from_user().
	 */
	if (!matrix_mdev->mig_data) {
		mutex_unlock(&matrix_dev->mdevs_lock);
		if (new_allocation)
			kvfree(ap_config);
		return -ENODEV;
	}
	if (new_allocation)
		kvfree(matrix_mdev->mig_data->resuming_mig_file.ap_config);
	matrix_mdev->mig_data->resuming_mig_file.ap_config = ap_config;
	matrix_mdev->mig_data->resuming_mig_file.config_sz = cfg_sz;
	mutex_unlock(&matrix_dev->mdevs_lock);

	return len;
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
	 * has completed and the vfio-ap device is operating normally. Since the
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

	kvfree(mig_data->resuming_mig_file.ap_config);
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
