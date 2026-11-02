// SPDX-License-Identifier: GPL-2.0
/*
 * Drives vfio_ap mdev migration.
 *
 * Copyright IBM Corp. 2025
 */
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/slab.h>
#include "vfio_ap_private.h"

/* Magic number and version for the vfio_ap_config migration blob */
#define VFIO_AP_MIG_MAGIC			0x76666170U  /* "vfap" */
#define VFIO_AP_MIG_VERSION			1U

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
 * Classification (functional capabilities) bits 8-15
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
 * AP Type bits 32-39:
 */
#define QINFO_DATA_MASK		0xffffc000ff000000

/*
 * Masks the bit that indicates whether full native card function is available
 * from the 8 bits specifying the functional capabilities of a queue
 */
#define CLASSIFICATION_NATIVE_FCN_MASK		0x80

/* The maximum number of queues that can be installed in an s390 system */
#define MAX_AP_QUEUES				(AP_DEVICES * AP_DOMAINS)

/* The maximum size of a vfio_ap_config blob, used to pre-allocate the receive
 * buffer on the destination host during the RESUMING phase of migration.
 */
#define VFIO_AP_CONFIG_MAX_SIZE \
	(sizeof(struct vfio_ap_config) + \
	 MAX_AP_QUEUES * sizeof(struct vfio_ap_queue_info))

/**
 * struct vfio_ap_migration_file:
 *
 * This object is used for chunk processing of multiple reads and writes of
 * AP configuration information.
 *
 * @matrix_mdev:	object used for managing the state of the mdev
 * @lock:		mutex used for synchronizing read/write operations
 * @filp:		file stream used to read or write AP configuration data
 * @ap_config:		object used to store AP configuration data between read
 *			or write calls
 * @config_sz:		the size (in bytes) of @ap_config
 */
struct vfio_ap_migration_file {
	struct ap_matrix_mdev	*matrix_mdev;
	struct mutex		lock; /* protects read/write operations */
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
 * @mig_state:			the current migration state
 * @resuming_mig_data_complete:	indicates whether a complete, valid AP
 *				configuration blob was received (true) or not (false)
 *				during the RESUMING phase. This must be checked
 *				on RESUMING->STOP to enforce the vfio.h
 *				requirement that SET_STATE must fail if the data
 *				is incomplete or invalid.
 * @resuming_mig_file:		the object used to restore the state of the
 *				vfio-ap device on the destination host.
 * @stop_copy_mig_file:		the object used to store the AP configuration of
 *				the source guest for transfer to the destination
 *				host.
 */
struct vfio_ap_migration_data {
	enum vfio_device_mig_state	mig_state;
	bool				resuming_mig_data_complete;
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
 * Note: Since a guest's AP configuration could be comprised of a large number of
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

static void
vfio_ap_release_stop_copy_file(struct vfio_ap_migration_data *mig_data)
{
	kvfree(mig_data->stop_copy_mig_file.ap_config);
	mig_data->stop_copy_mig_file.ap_config = NULL;
	mig_data->stop_copy_mig_file.config_sz = 0;
	mig_data->stop_copy_mig_file.filp = NULL;
}

static void vfio_ap_release_resuming_file(struct vfio_ap_migration_data *mig_data)
{
	kvfree(mig_data->resuming_mig_file.ap_config);
	mig_data->resuming_mig_file.ap_config = NULL;
	mig_data->resuming_mig_file.config_sz = 0;
	mig_data->resuming_mig_file.filp = NULL;
	mig_data->resuming_mig_data_complete = false;
}

static void vfio_ap_release_mig_files(struct ap_matrix_mdev *matrix_mdev);

static int vfio_ap_release_mig_file(struct inode *file_inode, struct file *filp)
{
	struct vfio_ap_migration_file *mig_file = filp->private_data;

	/*
	 * The mig_file (and by extension mig_file->matrix_mdev) is tied to
	 * migration FD lifetime (see vfio_ap_open_file_stream() function) to
	 * guarantee that the matrix_mdev remains valid for every read/write
	 * operation. Let's drop it here since the migration FD is being
	 * released.
	 */
	vfio_device_put_registration(&mig_file->matrix_mdev->vdev);
	return 0;
}

/**
 * validate_stop_copy_read_parms: Validate the input parameters to the
 *                                vfio_ap_stop_copy_read function
 *
 * @mig_file:	Pointer to the object used for storing the vfio-ap device state
 * @filp:	File stream to which to write source guest's AP configuration
 * @len:	The length of the data to be read
 *
 * Verify the following:
 * - @filp is the instance opened when state transitioned from STOP to STOP_COPY
 * - @filp->f_pos + @len does not cause integer overflow
 * - @mig_file->matrix_mdev is not NULL
 * - @mig_file->matrix_mdev->mig_data is not NULL
 *
 * Returns: 0 if the parameters pass validation; otherwise returns an error
 */
static int validate_stop_copy_read_parms(struct vfio_ap_migration_file *mig_file,
					 struct file *filp,
					 size_t len)
{
	loff_t total_len;

	lockdep_assert_held(&mig_file->lock);
	lockdep_assert_held(&matrix_dev->mdevs_lock);

	if (filp != mig_file->filp)
		return -EINVAL;

	if (check_add_overflow((loff_t)len, filp->f_pos, &total_len))
		return -EIO;

	if (!mig_file->matrix_mdev || !mig_file->matrix_mdev->mig_data)
		return -ENODEV;

	return 0;
}

static size_t vfio_ap_config_size(struct ap_matrix_mdev *matrix_mdev,
				  unsigned int *num_queues)
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

	status = ap_test_queue(apqn, 1, hwinfo);

	switch (status.response_code) {
	case AP_RESPONSE_NORMAL:
	case AP_RESPONSE_RESET_IN_PROGRESS:
	case AP_RESPONSE_DECONFIGURED:
	case AP_RESPONSE_CHECKSTOPPED:
	case AP_RESPONSE_BUSY:
		/* For all these RCs the tapq info should be available */
		break;
	default:
		pr_err_ratelimited("vfio_ap_mdev %s: Failed to get hwinfo for queue %02lx.%04lx: TAPQ rc=%d\n",
				   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn),
				   status.response_code);
		return -EIO;
	}

	/* There should be at least one of the mode bits set */
	if (!hwinfo->value) {
		pr_err_ratelimited("vfio_ap_mdev %s: Empty hwinfo for queue %02lx.%04lx: TAPQ rc=%d\n",
				   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn),
				   status.response_code);
		return -EIO;
	}

	return 0;
}

/**
 * vfio_ap_store_queue_info:
 *
 * Stores the hardware information returned from the PQAP(TAPQ) command for each
 * queue device identified in the 'qinfo' field of the a vfio_ap_config
 * object. The APQNs in the 'qinfo' field must already have been snapshotted
 * prior to calling this function.
 *
 * @mdev_name:	The name (UUID) of the mediated device to use in log messages
 * @ap_config:	A reference to the vfio_ap_config instance in which to store
 *		the queue information. It is expected that each APQN identifying
 *		a queue device for which hardware information is to be retrieved
 *		shall be snapshotted prior to calling this function.
 *
 * Returns:	Zero (0) if the hardware information is retrieved for each queue
 *		device in the AP configuration; otherwise, returns an error.
 */
static int vfio_ap_store_queue_info(const char *mdev_name,
				    struct vfio_ap_config *ap_config)
{
	struct ap_tapq_hwinfo source_hwinfo;
	unsigned long num_queues;
	int ret;

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
	struct vfio_ap_config *ap_configuration;
	unsigned long *apm, *aqm, apid, apqi;
	unsigned int num_queues;
	const char *mdev_name;
	size_t ap_config_size;
	int ret, qindex;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	ap_config_size = vfio_ap_config_size(matrix_mdev, &num_queues);

	ap_configuration = kvzalloc(ap_config_size, GFP_KERNEL_ACCOUNT);
	if (!ap_configuration)
		return -ENOMEM;

	ap_configuration->magic   = VFIO_AP_MIG_MAGIC;
	ap_configuration->version = VFIO_AP_MIG_VERSION;

	/*
	 * num_queues must be set before writing qinfo[] elements; the
	 * __counted_by(num_queues) annotation on qinfo[] causes the compiler to
	 * insert bounds checks that evaluate against ap_configuration->num_queues.
	 * Writing through qinfo[i] with num_queues still 0 would trap.
	 */
	ap_configuration->num_queues = num_queues;

	apm = matrix_mdev->shadow_apcb.apm;
	aqm = matrix_mdev->shadow_apcb.aqm;
	qindex = 0;
	for_each_set_bit_inv(apid, apm, AP_DEVICES) {
		for_each_set_bit_inv(apqi, aqm, AP_DOMAINS) {
			ap_configuration->qinfo[qindex].apqn =
				AP_MKQID(apid, apqi);
			qindex += 1;
		}
	}
	memcpy(ap_configuration->adm, matrix_mdev->shadow_apcb.adm,
	       sizeof(ap_configuration->adm));
	mdev_name = dev_name(matrix_mdev->vdev.dev);

	ret = vfio_ap_store_queue_info(mdev_name, ap_configuration);
	if (ret) {
		kvfree(ap_configuration);
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
	void *bounce;
	ssize_t ret;

	/*
	 * This file was opened with stream_open(), so pos should be NULL for
	 * sequential read() calls; a non-NULL pointer will be passed only
	 * for positional pread() calls in which case we return an error
	 * indicating broken pipe/illegal seek on a non-seekable file
	 */
	if (pos)
		return -ESPIPE;

	/*
	 * mig_file is guaranteed live here: vfio_ap_open_file_stream() took
	 * a vfio_device registration reference that is held until
	 * vfio_ap_release_mig_file() runs, so the embedding mig_file cannot
	 * be freed while this file descriptor is open.
	 */
	mig_file = filp->private_data;
	mutex_lock(&mig_file->lock);
	mutex_lock(&matrix_dev->mdevs_lock);

	ret = validate_stop_copy_read_parms(mig_file, filp, len);
	if (ret) {
		mutex_unlock(&matrix_dev->mdevs_lock);
		mutex_unlock(&mig_file->lock);
		return ret;
	}

	pos = &filp->f_pos;

	/*
	 * The data to be returned to the caller - i.e., the source guest's
	 * AP configuration data - is generated on the first read() call. The
	 * result is cached in mig_file->ap_config for subsequent read() calls.
	 */
	if (!mig_file->ap_config) {
		ret = vfio_ap_get_config(mig_file->matrix_mdev);
		if (ret) {
			mutex_unlock(&matrix_dev->mdevs_lock);
			mutex_unlock(&mig_file->lock);
			return ret;
		}
	}

	/* Return EOF if all of the AP configuration data has been read */
	if (*pos >= mig_file->config_sz) {
		mutex_unlock(&matrix_dev->mdevs_lock);
		mutex_unlock(&mig_file->lock);
		return 0;
	}

	/*
	 * Don't read past the end of the AP configuration data. Limit len to
	 * the number of bytes remaining.
	 */
	len = min_t(size_t, mig_file->config_sz - *pos, len);

	/*
	 * Copy into a bounce buffer under mdevs_lock and advance *pos, then
	 * perform copy_to_user() outside the lock. This avoids stalling
	 * all host mdev operations if userspace passes a buffer that faults.
	 */
	bounce = kmalloc(len, GFP_KERNEL);
	if (!bounce) {
		mutex_unlock(&matrix_dev->mdevs_lock);
		mutex_unlock(&mig_file->lock);
		return -ENOMEM;
	}

	memcpy(bounce, (char *)mig_file->ap_config + *pos, len);
	mutex_unlock(&matrix_dev->mdevs_lock);

	if (copy_to_user(buf, bounce, len)) {
		kfree(bounce);
		mutex_unlock(&mig_file->lock);
		return -EFAULT;
	}

	*pos += len;
	mutex_unlock(&mig_file->lock);
	kfree(bounce);
	return len;
}

static const struct file_operations vfio_ap_stop_copy_fops = {
	.owner = THIS_MODULE,
	.read = vfio_ap_stop_copy_read,
	.release = vfio_ap_release_mig_file,
};

static struct file *vfio_ap_open_file_stream(struct vfio_ap_migration_file *mig_file,
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
	if (!vfio_device_try_get_registration(&mig_file->matrix_mdev->vdev))
		return ERR_PTR(-ENODEV);

	filp = anon_inode_getfile("vfio_ap_mig_file", fops, mig_file, flags);
	if (IS_ERR(filp)) {
		vfio_device_put_registration(&mig_file->matrix_mdev->vdev);
		return filp;
	}

	stream_open(filp->f_inode, filp);

	return filp;
}

static int validate_resuming_write_parms(struct vfio_ap_migration_file *mig_file,
					 struct file *filp, size_t len)
{
	loff_t total_len;

	lockdep_assert_held(&mig_file->lock);
	lockdep_assert_held(&matrix_dev->mdevs_lock);

	if (!len || filp->f_pos < 0)
		return -EINVAL;

	if (!mig_file->matrix_mdev->mig_data)
		return -ENODEV;

	/* Reject any write after the full configuration has been received */
	if (mig_file->matrix_mdev->mig_data->resuming_mig_data_complete)
		return -EPROTO;

	if (filp != mig_file->filp)
		return -ENXIO;

	if (check_add_overflow((loff_t)len, filp->f_pos, &total_len))
		return -ERANGE;

	/* Reject writes that would overflow the pre-allocated buffer */
	if (filp->f_pos + len > VFIO_AP_CONFIG_MAX_SIZE)
		return -EIO;

	return 0;
}

static void report_facilities_compatibility(const char *mdev_name,
					    unsigned long apqn,
					    struct ap_tapq_hwinfo *src_hwinfo,
					    struct ap_tapq_hwinfo *target_hwinfo)
{
	if (src_hwinfo->apsc != target_hwinfo->apsc) {
		if (src_hwinfo->apsc) {
			pr_err_ratelimited("vfio_ap_mdev %s: APSC facility installed in source queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err_ratelimited("vfio_ap_mdev %s: APSC facility not installed in target queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			pr_err_ratelimited("vfio_ap_mdev %s: APSC facility not installed in source queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err_ratelimited("vfio_ap_mdev %s: APSC facility installed in target queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}

	if (src_hwinfo->mex4k != target_hwinfo->mex4k) {
		if (src_hwinfo->mex4k) {
			pr_err_ratelimited("vfio_ap_mdev %s: mex4k facility installed in source queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err_ratelimited("vfio_ap_mdev %s: mex4k facility not installed in target queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			pr_err_ratelimited("vfio_ap_mdev %s: mex4k facility not installed in source queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err_ratelimited("vfio_ap_mdev %s: mex4k facility installed in target queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}

	if (src_hwinfo->crt4k != target_hwinfo->crt4k) {
		if (src_hwinfo->crt4k) {
			pr_err_ratelimited("vfio_ap_mdev %s: crt4k facility installed in source queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err_ratelimited("vfio_ap_mdev %s: crt4k facility not installed in target queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			pr_err_ratelimited("vfio_ap_mdev %s: crt4k facility not installed in source queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err_ratelimited("vfio_ap_mdev %s: crt4k facility installed in target queue %02lx.%04lx\n",
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
			pr_err_ratelimited("vfio_ap_mdev %s: Coprocessor-mode facility installed in source queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err_ratelimited("vfio_ap_mdev %s: Coprocessor-mode facility not installed in target queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			pr_err_ratelimited("vfio_ap_mdev %s: Coprocessor-mode facility not installed in source queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err_ratelimited("vfio_ap_mdev %s: Coprocessor-mode facility installed in target queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}

	if (src_hwinfo->accel != target_hwinfo->accel) {
		if (src_hwinfo->accel) {
			pr_err_ratelimited("vfio_ap_mdev %s: Accelerator-mode facility installed in source queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err_ratelimited("vfio_ap_mdev %s: Accelerator-mode facility not installed in target queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			pr_err_ratelimited("vfio_ap_mdev %s: Accelerator-mode facility not installed in source queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err_ratelimited("vfio_ap_mdev %s: Accelerator-mode facility installed in target queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}

	if (src_hwinfo->ep11 != target_hwinfo->ep11) {
		if (src_hwinfo->ep11) {
			pr_err_ratelimited("vfio_ap_mdev %s: XCP-mode facility installed in source queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err_ratelimited("vfio_ap_mdev %s: XCP-mode facility not installed in target queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			pr_err_ratelimited("vfio_ap_mdev %s: XCP-mode facility not installed in source queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err_ratelimited("vfio_ap_mdev %s: XCP-mode facility installed in target queue %02lx.%04lx\n",
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
			pr_err_ratelimited("vfio_ap_mdev %s: AP-extended-addressing (APXA) facility installed in source queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err_ratelimited("vfio_ap_mdev %s: AP-extended-addressing (APXA) facility not installed in target queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			pr_err_ratelimited("vfio_ap_mdev %s: AP-extended-addressing (APXA) facility not installed in source queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err_ratelimited("vfio_ap_mdev %s: AP-extended-addressing (APXA) facility installed in target queue %02lx.%04lx\n",
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
			pr_err_ratelimited("vfio_ap_mdev %s: Stateless-command-filtering (SLCF) available in source queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err_ratelimited("vfio_ap_mdev %s: Stateless-command-filtering (SLCF) not available in target queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			pr_err_ratelimited("vfio_ap_mdev %s: Stateless-command-filtering (SLCF) not available in source queue %02lx.%04lx\n",
					   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			pr_err_ratelimited("vfio_ap_mdev %s: Stateless-command-filtering (SLCF) available in target queue %02lx.%04lx\n",
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
		pr_err_ratelimited("vfio_ap_mdev %s: Bind/associate state for source (%01x) and target (%01x) queue %02lx.%04lx must be 0\n",
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
		pr_err_ratelimited("vfio_ap_mdev %s: AP type of source queue %02lx.%04lx (%02x) not compatible with target (%02x)\n",
				   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn),
				   src_hwinfo->at, target_hwinfo->at);
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
		pr_err_ratelimited("vfio_ap_mdev %s: Full native card function available on source queue %02lx.%04lx\n",
				   mdev_name, AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

		pr_err_ratelimited("vfio_ap_mdev %s: Only stateless functions available on target queue %02lx.%04lx\n",
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
	pr_err_ratelimited("vfio_ap_mdev %s: Migration failed: Source and target queue (%02lx.%04lx) not compatible\n",
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

/**
 * ap_config_is_subset_of_shadow_apcb:
 *
 * Verify that each APQN and control domain in the source guest's AP
 * configuration is present in the destination mdev's shadow_apcb. The
 * destination mdev must be pre-configured by the administrator with at least
 * the adapters, domains and control domains the source guest is actively
 * using. Since shadow_apcb is derived from the administrator's assignment
 * filtered to only installed and vfio_ap-bound queues, any APQN or control
 * domain present in shadow_apcb is guaranteed to be available.
 *
 * All missing APQNs and control domains are logged before returning so the
 * administrator sees all problems at once rather than one per retry.
 *
 * @mdev_name:     The mdev name to use in error messages
 * @matrix_mdev:   The destination mdev whose shadow_apcb to check against
 * @source_config: The source guest's AP configuration
 *
 * Returns: true if every APQN and control domain in @source_config is present
 *          in @matrix_mdev->shadow_apcb; otherwise returns false.
 */
static bool ap_config_is_subset_of_shadow_apcb(const char *mdev_name,
					       struct ap_matrix_mdev *matrix_mdev,
					       struct vfio_ap_config *source_config)
{
	unsigned long apid, apqi, apqn, domain_num;
	bool ret = true;

	for (int i = 0; i < source_config->num_queues; i++) {
		apqn = source_config->qinfo[i].apqn;
		apid = AP_QID_CARD(apqn);
		apqi = AP_QID_QUEUE(apqn);

		if (!test_bit_inv(apid, matrix_mdev->shadow_apcb.apm) ||
		    !test_bit_inv(apqi, matrix_mdev->shadow_apcb.aqm)) {
			pr_err_ratelimited("vfio_ap_mdev %s: Queue %02lx.%04lx not in destination mdev's AP configuration\n",
					   mdev_name, apid, apqi);
			ret = false;
		}
	}

	for_each_set_bit_inv(domain_num, (unsigned long *)source_config->adm,
			     AP_DOMAINS) {
		if (!test_bit_inv(domain_num, matrix_mdev->shadow_apcb.adm)) {
			pr_err_ratelimited("vfio_ap_mdev %s: Control domain %04lx not in destination mdev's AP configuration\n",
					   mdev_name, domain_num);
			ret = false;
		}
	}

	return ret;
}

static int do_post_copy_validation(struct ap_matrix_mdev *matrix_mdev,
				   struct vfio_ap_config *source_config)
{
	const char *mdev_name = dev_name(matrix_mdev->vdev.dev);

	lockdep_assert_held(&ap_attr_mutex);
	lockdep_assert_held(&matrix_dev->mdevs_lock);

	if (source_config->magic != VFIO_AP_MIG_MAGIC ||
	    source_config->version != VFIO_AP_MIG_VERSION)
		return -EINVAL;

	if (!ap_config_is_subset_of_shadow_apcb(mdev_name, matrix_mdev,
						source_config))
		return -EINVAL;

	return verify_ap_configs_are_compatible(mdev_name, source_config);
}

static ssize_t vfio_ap_resuming_write(struct file *filp, const char __user *buf,
				      size_t len, loff_t *pos)
{
	struct vfio_ap_migration_file *mig_file;
	struct vfio_ap_config *ap_config;
	ssize_t ret;
	void *kbuf;

	/*
	 * This file was opened with stream_open(), so pos should be NULL for
	 * sequential write() calls; a non-NULL pointer will be passed only
	 * for positional pwrite() calls in which case we return an error
	 * indicating broken pipe/illegal seek on a non-seekable file.
	 */
	if (pos)
		return -ESPIPE;

	/*
	 * Reject absurd lengths before allocating the bounce buffer below.
	 * This only caps the allocation: a write can never be larger than
	 * the receive buffer. The authoritative check against the space
	 * remaining is made by validate_resuming_write_parms() under the
	 * locks.
	 */
	if (len > VFIO_AP_CONFIG_MAX_SIZE)
		return -EIO;

	/*
	 * Copy the user data into a kernel buffer before taking any locks.
	 * copy_from_user() can fault and block for an unbounded time, which
	 * must not happen while holding ap_attr_mutex or mdevs_lock.
	 */
	kbuf = vmemdup_user(buf, len);
	if (IS_ERR(kbuf))
		return PTR_ERR(kbuf);

	/*
	 * mig_file->lock is taken first to serialize concurrent write()
	 * calls on the same migration fd, preventing two chunks from
	 * racing to update f_pos and ap_config simultaneously.
	 *
	 * ap_attr_mutex is held for the full duration of the write, just
	 * as (un)assign_adapter_store() and (un)assign_domain_store() do.
	 * This prevents the AP bus apmask/aqmask attributes from being
	 * changed while the guest's AP configuration is being validated.
	 *
	 * mdevs_lock is held to protect access to matrix_mdev->shadow_apcb,
	 * mig_data, and mig_data->resuming_mig_data_complete. guests_lock
	 * and kvm->lock are not needed because the pre-configured destination
	 * mdev model does not hot-plug the source guest's AP configuration
	 * into the running guest; the guest APCB is never modified here.
	 *
	 * Holding these locks for the full duration of a potentially chunked
	 * write could in theory affect performance, but this is acceptable
	 * for several reasons: migration is an infrequent operation;
	 * real-world guest AP configurations are small (each CEX8 card has
	 * 2 adapters and up to 16 domains, and the 85 cards in a system are
	 * shared across all LPARs and guests); and only a memcpy() and some
	 * integer checks are done for each chunk while the locks are held.
	 */
	mig_file = filp->private_data;
	mutex_lock(&mig_file->lock);
	mutex_lock(&ap_attr_mutex);
	mutex_lock(&matrix_dev->mdevs_lock);
	pos = &filp->f_pos;

	ret = validate_resuming_write_parms(mig_file, filp, len);
	if (ret)
		goto out_unlock;

	ap_config = mig_file->ap_config;
	memcpy((char *)ap_config + *pos, kbuf, len);
	*pos += len;

	/*
	 * Check whether we have received all the data.  We know the total
	 * expected size once num_queues has arrived, i.e. once *pos is past
	 * the fixed header.  Until then keep accumulating.
	 */
	if (*pos >= offsetofend(struct vfio_ap_config, num_queues)) {
		size_t expected_sz;

		if (ap_config->num_queues > MAX_AP_QUEUES) {
			ret = -EINVAL;
			goto out_unlock;
		}

		expected_sz = sizeof(struct vfio_ap_config) +
			      ap_config->num_queues *
			      sizeof(struct vfio_ap_queue_info);

		if (*pos > (loff_t)expected_sz) {
			ret = -EINVAL;
			goto out_unlock;
		}

		if (*pos == (loff_t)expected_sz) {
			ret = do_post_copy_validation(mig_file->matrix_mdev, ap_config);
			mig_file->matrix_mdev->mig_data->resuming_mig_data_complete = !ret;
		}
	}

out_unlock:
	mutex_unlock(&matrix_dev->mdevs_lock);
	mutex_unlock(&ap_attr_mutex);
	mutex_unlock(&mig_file->lock);
	kvfree(kbuf);
	return ret ? ret : (ssize_t)len;
}

static const struct file_operations vfio_ap_resume_fops = {
	.owner = THIS_MODULE,
	.write = vfio_ap_resuming_write,
	.release = vfio_ap_release_mig_file,
};

/**
 * vfio_ap_resuming_init
 *
 * Initializes the ap_config, config_sz and filp fields of a
 * struct vfio_ap_migration_file instance:
 *
 * 1. Pre-allocates the maximum possible receive buffer using kvzalloc so that
 *    vfio_ap_resuming_write() can copy directly into it without needing
 *    to know num_queues up front.  kvzalloc falls back to vmalloc for
 *    large allocations rather than failing a high-order kmalloc.
 * 2. Sets num_queues to MAX_AP_QUEUES immediately after allocation so
 *    that the __counted_by(num_queues) annotation on qinfo[] covers
 *    the full pre-allocated buffer. Without this, fortified memcpy()
 *    computes object size from num_queues == 0 and will fortify_panic()
 *    on the first write chunk that reaches into qinfo[].  num_queues
 *    will be overwritten with the correct value when the source guest's
 *    AP configuration data is received via vfio_ap_resuming_write().
 *
 * @mig_file:	pointer to object that maintains the state of the migration
 *		data stream.
 */
static struct file *vfio_ap_resuming_init(struct vfio_ap_migration_file *mig_file)
{
	struct vfio_ap_config *ap_config;
	struct file *filp;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	ap_config = kvzalloc(VFIO_AP_CONFIG_MAX_SIZE, GFP_KERNEL_ACCOUNT);
	if (!ap_config)
		return ERR_PTR(-ENOMEM);

	ap_config->num_queues = MAX_AP_QUEUES;

	filp = vfio_ap_open_file_stream(mig_file, &vfio_ap_resume_fops, O_WRONLY);

	if (IS_ERR(filp)) {
		kvfree(ap_config);
		return filp;
	}

	mig_file->ap_config = ap_config;
	mig_file->config_sz = VFIO_AP_CONFIG_MAX_SIZE;
	mig_file->filp = filp;

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
		struct vfio_ap_migration_file *mig_file = &mig_data->stop_copy_mig_file;
		struct file *filp = vfio_ap_open_file_stream(mig_file,
							     &vfio_ap_stop_copy_fops,
							     O_RDONLY);
		if (IS_ERR(filp))
			return ERR_CAST(filp);

		mig_file->filp = filp;

		return filp;
	}

	/*
	 * Begins the process of restoring the vfio device state by creating and
	 * returning a streaming data_fd to be used to read in the internal
	 * state of the vfio-ap device on the destination host.
	 */
	if (cur_state == VFIO_DEVICE_STATE_STOP &&
	    new_state == VFIO_DEVICE_STATE_RESUMING) {
		struct file *filp = vfio_ap_resuming_init(&mig_data->resuming_mig_file);

		if (IS_ERR(filp))
			return ERR_CAST(filp);

		return filp;
	}

	/*
	 * Terminates the data transfer session of the vfio-ap device state
	 * to the destination guest.
	 */
	if (cur_state == VFIO_DEVICE_STATE_RESUMING &&
	    new_state == VFIO_DEVICE_STATE_STOP) {
		if (!mig_data->resuming_mig_data_complete)
			return ERR_PTR(-EINVAL);

		/* Set resuming_mig_data_complete to false for clean reuse */
		matrix_mdev->mig_data->resuming_mig_data_complete = false;
		vfio_ap_release_resuming_file(matrix_mdev->mig_data);
		return NULL;
	}

	/*
	 * Terminates the transfer session of the vfio-ap device state from
	 * the source guest.
	 */
	if (cur_state == VFIO_DEVICE_STATE_STOP_COPY &&
	    new_state == VFIO_DEVICE_STATE_STOP) {
		vfio_ap_release_stop_copy_file(matrix_mdev->mig_data);
		return NULL;
	}

	/*
	 * These states indicate migration has either not been initiated or
	 * has completed and the vfio-ap device is operating normally. Since the
	 * vfio-ap device does not virtualize a DMA device, there is no internal
	 * device state to incorporate into the vfio-ap device on the target.
	 */
	if (cur_state == VFIO_DEVICE_STATE_STOP &&
	    new_state == VFIO_DEVICE_STATE_RUNNING) {
		vfio_ap_release_mig_files(matrix_mdev);
		return NULL;
	}

	if (cur_state == VFIO_DEVICE_STATE_RUNNING &&
	    new_state == VFIO_DEVICE_STATE_STOP) {
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

	mutex_lock(&matrix_dev->mdevs_lock);

	matrix_mdev = container_of(vdev, struct ap_matrix_mdev, vdev);
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

	if (mig_data->mig_state == VFIO_DEVICE_STATE_ERROR)
		filp = ERR_PTR(-EIO);

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

	/*
	 * Live guest migration is not supported for SE guests, so there is no
	 * need to initialize the migration data.
	 */
	if (ap_is_se_guest())
		return 0;

	mig_data = kzalloc_obj(struct vfio_ap_migration_data, GFP_KERNEL);
	if (!mig_data)
		return -ENOMEM;

	mig_data->mig_state = VFIO_DEVICE_STATE_RUNNING;
	mig_data->resuming_mig_data_complete = false;
	mig_data->resuming_mig_file.matrix_mdev = matrix_mdev;
	mutex_init(&mig_data->resuming_mig_file.lock);
	mig_data->stop_copy_mig_file.matrix_mdev = matrix_mdev;
	mutex_init(&mig_data->stop_copy_mig_file.lock);
	matrix_mdev->mig_data = mig_data;

	return 0;
}

/**
 * vfio_ap_release_mig_files:
 *
 * Free the ap_config buffers for any open migration FDs. Although a
 * migration FD may still be held open by userspace, it is safe to free
 * mig_data here because:
 *
 *   1. matrix_mdev remains valid for the lifetime of any open migration
 *      FD via the vfio_device registration reference taken in
 *      vfio_ap_open_file_stream() and dropped in
 *      vfio_ap_release_mig_file().
 *
 *   2. mig_data is only accessed by the migration file ops
 *      (vfio_ap_stop_copy_read, vfio_ap_resuming_write) under
 *      mdevs_lock. Once mig_data is set to NULL by the caller, those
 *      paths will see NULL and return -ENODEV before dereferencing it.
 *
 * @matrix_mdev: The object used to maintain the state for a mediated device
 */
static void vfio_ap_release_mig_files(struct ap_matrix_mdev *matrix_mdev)
{
	struct vfio_ap_migration_data *mig_data;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	mig_data = matrix_mdev->mig_data;
	vfio_ap_release_stop_copy_file(mig_data);
	vfio_ap_release_resuming_file(mig_data);
}

/**
 * vfio_ap_release_migration_data: reclaim private migration data
 *
 * @matrix_mdev: pointer to the object containing the state of the mdev
 */
void vfio_ap_release_migration_data(struct ap_matrix_mdev *matrix_mdev)
{
	lockdep_assert_held(&matrix_dev->mdevs_lock);

	if (!matrix_mdev->mig_data)
		return;

	vfio_ap_release_mig_files(matrix_mdev);
	mutex_destroy(&matrix_mdev->mig_data->stop_copy_mig_file.lock);
	mutex_destroy(&matrix_mdev->mig_data->resuming_mig_file.lock);
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
