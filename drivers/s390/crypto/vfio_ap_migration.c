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
 * AP Type bits 32-40:
 */
#define QINFO_DATA_MASK		0xffffc000ff000000

/*
 * Masks the bit that indicates whether full native card function is available
 * from the 8 bits specifying the functional capabilities of a queue
 */
#define CLASSIFICATION_NATIVE_FCN_MASK		0x80

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

static ssize_t validate_save_read_parms(struct vfio_ap_migration_file *migf,
					loff_t *pos, size_t len)
{
	lockdep_assert_held(&matrix_dev->mdevs_lock);

	if (migf->disabled) {
		dev_err(migf->matrix_mdev->vdev.dev,
			"%s (%d): migration file is disabled\n",
			__func__, __LINE__);
		return -ENODEV;
	}

	if (*pos > migf->config_sz) {
		dev_err(migf->matrix_mdev->vdev.dev,
			"%s (%d): file pos (%llu) exceeds migf->config size (%zu)\n",
			__func__, __LINE__, *pos, migf->config_sz);
		return -EINVAL;
	}

	return 0;
}

static ssize_t vfio_ap_save_read(struct file *filp, char __user *buf,
				 size_t len, loff_t *pos)
{
	struct vfio_ap_migration_file *migf;
	ssize_t ret = 0;

	if (pos)
		return -ESPIPE;

	mutex_lock(&matrix_dev->mdevs_lock);

	pos = &filp->f_pos;
	migf = filp->private_data;

	ret = validate_save_read_parms(migf, pos, len);
	if (ret)
		goto out_unlock;

	len = min_t(size_t, migf->config_sz - *pos, len);
	if (len) {
		if (copy_to_user(buf, (void *)migf->ap_config + *pos, len)) {
			ret = -EFAULT;
			dev_err(migf->matrix_mdev->vdev.dev,
				"%s (%d): failed to copy config data to user\n",
				__func__, __LINE__);
			goto out_unlock;
		}

		*pos += len;
		ret = len;
	}

	dev_dbg(migf->matrix_mdev->vdev.dev,
		"%s (%d): copied %zu bytes of AP config data to user\n",
		__func__, __LINE__, len);

out_unlock:
	mutex_unlock(&matrix_dev->mdevs_lock);

	return ret;
}

static void vfio_ap_deallocate_migf(struct vfio_ap_migration_file *migf);

static int vfio_ap_release_migf(struct inode *inode, struct file *filp)
{
	struct vfio_ap_migration_file *migf;

	mutex_lock(&matrix_dev->mdevs_lock);
	migf = filp->private_data;
	vfio_ap_deallocate_migf(migf);
	mutex_unlock(&matrix_dev->mdevs_lock);

	return 0;
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

static int validate_resume_write_parms(struct vfio_ap_migration_file *migf,
				       size_t len, loff_t *pos)
{
	loff_t total_len;
	int ret = -EIO;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	if (!migf->matrix_mdev) {
		pr_err("migration failed: matrix_mdev object not linked to migration file");
		goto done;
	}

	if (*pos < 0) {
		dev_err(migf->matrix_mdev->vdev.dev,
			"migration failed: invalid migration file position  (%lli) for write\n",
			*pos);
		goto done;
	}

	if (check_add_overflow((loff_t)len, *pos, &total_len)) {
		dev_err(migf->matrix_mdev->vdev.dev,
			"migration failed: pos (%llu) plus len (%zu) operation overflowed loff_t precision\n",
			*pos, len);
		goto done;
	}

	if (total_len > migf->config_sz) {
		dev_err(migf->matrix_mdev->vdev.dev,
			"migration failed: source guest's AP config size (%llu) larger than target's (%lu)",
			total_len, migf->config_sz);
		goto done;
	}

	if (migf->disabled) {
		dev_err(migf->matrix_mdev->vdev.dev,
			"migration failed: migration file is disabled");
		goto done;
	}

	dev_dbg(migf->matrix_mdev->vdev.dev, "resume write parameters validated\n");
	ret = 0;

done:
	return ret;
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

static void report_facilities_compatibility(struct ap_matrix_mdev *matrix_mdev,
					    unsigned long apqn,
					    struct ap_tapq_hwinfo *src_hwinfo,
					    struct ap_tapq_hwinfo *target_hwinfo)
{
	if (src_hwinfo->apsc != target_hwinfo->apsc) {
		if (src_hwinfo->apsc) {
			dev_err(matrix_mdev->vdev.dev,
				"APSC facility installed in source queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			dev_err(matrix_mdev->vdev.dev,
				"APSC facility not installed in target queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			dev_err(matrix_mdev->vdev.dev,
				"APSC facility not installed in source queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			dev_err(matrix_mdev->vdev.dev,
				"APSC facility installed in target queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}

	if (src_hwinfo->mex4k != target_hwinfo->mex4k) {
		if (src_hwinfo->mex4k) {
			dev_err(matrix_mdev->vdev.dev,
				"mex4k facility installed in source queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			dev_err(matrix_mdev->vdev.dev,
				"mex4k facility not installed in target queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			dev_err(matrix_mdev->vdev.dev,
				"mex4k facility not installed in source queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			dev_err(matrix_mdev->vdev.dev,
				"mex4k facility installed in target queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}

	if (src_hwinfo->crt4k != target_hwinfo->crt4k) {
		if (src_hwinfo->crt4k) {
			dev_err(matrix_mdev->vdev.dev,
				"crt4k facility installed in source queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			dev_err(matrix_mdev->vdev.dev,
				"crt4k facility not installed in target queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			dev_err(matrix_mdev->vdev.dev,
				"crt4k facility not installed in source queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			dev_err(matrix_mdev->vdev.dev,
				"crt4k facility installed in target queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}
}

static void report_mode_compatibility(struct ap_matrix_mdev *matrix_mdev,
				      unsigned long apqn,
				      struct ap_tapq_hwinfo *src_hwinfo,
				      struct ap_tapq_hwinfo *target_hwinfo)
{
	if (src_hwinfo->cca != target_hwinfo->cca) {
		if (src_hwinfo->cca) {
			dev_err(matrix_mdev->vdev.dev,
				"Coprocessor-mode facility installed in source queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			dev_err(matrix_mdev->vdev.dev,
				"Coprocessor-mode  facility not installed target queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			dev_err(matrix_mdev->vdev.dev,
				"Coprocessor-mode facility not installed in source queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			dev_err(matrix_mdev->vdev.dev,
				"Coprocessor-mode  facility installed target queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}

	if (src_hwinfo->accel != target_hwinfo->accel) {
		if (src_hwinfo->accel) {
			dev_err(matrix_mdev->vdev.dev,
				"Accelerator-mode facility installed source queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			dev_err(matrix_mdev->vdev.dev,
				"Accelerator-mode facility not installed target queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			dev_err(matrix_mdev->vdev.dev,
				"Accelerator-mode facility not installed source queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			dev_err(matrix_mdev->vdev.dev,
				"Accelerator-mode facility installed target queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}

	if (src_hwinfo->ep11 != target_hwinfo->ep11) {
		if (src_hwinfo->ep11) {
			dev_err(matrix_mdev->vdev.dev,
				"XCP-mode facility installed source queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			dev_err(matrix_mdev->vdev.dev,
				"XCP-mode facility not installed target queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			dev_err(matrix_mdev->vdev.dev,
				"XCP-mode facility not installed source queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			dev_err(matrix_mdev->vdev.dev,
				"XCP-mode facility installed target queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}
}

static void report_apxa_compatibility(struct ap_matrix_mdev *matrix_mdev,
				      unsigned long apqn,
				      struct ap_tapq_hwinfo *src_hwinfo,
				      struct ap_tapq_hwinfo *target_hwinfo)
{
	if (src_hwinfo->apxa != target_hwinfo->apxa) {
		if (src_hwinfo->apxa) {
			dev_err(matrix_mdev->vdev.dev,
				"AP-extended-addressing (APXA) facility installed in source queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			dev_err(matrix_mdev->vdev.dev,
				"AP-extended-addressing (APXA) facility not installed in target queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			dev_err(matrix_mdev->vdev.dev,
				"AP-extended-addressing (APXA) facility not installed in source queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			dev_err(matrix_mdev->vdev.dev,
				"AP-extended-addressing (APXA) facility installed in target queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}
}

static void report_slcf_compatibility(struct ap_matrix_mdev *matrix_mdev,
				      unsigned long apqn,
				      struct ap_tapq_hwinfo *src_hwinfo,
				      struct ap_tapq_hwinfo *target_hwinfo)
{
	if (src_hwinfo->slcf != target_hwinfo->slcf) {
		if (src_hwinfo->slcf) {
			dev_err(matrix_mdev->vdev.dev,
				"Stateless-command-filtering (SLCF) available in source queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			dev_err(matrix_mdev->vdev.dev,
				"Stateless-command-filtering (SLCF) not available in target queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		} else {
			dev_err(matrix_mdev->vdev.dev,
				"Stateless-command-filtering (SLCF) not available in source queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			dev_err(matrix_mdev->vdev.dev,
				"Stateless-command-filtering (SLCF) available in target queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}
}

static void report_class_compatibility(struct ap_matrix_mdev *matrix_mdev,
				       unsigned long apqn,
				       struct ap_tapq_hwinfo *src_hwinfo,
				       struct ap_tapq_hwinfo *target_hwinfo)
{
	unsigned long src_native, target_native;

	src_native = src_hwinfo->class & CLASSIFICATION_NATIVE_FCN_MASK;
	target_native = target_hwinfo->class & CLASSIFICATION_NATIVE_FCN_MASK;

	if (src_native != target_native) {
		/*
		 * If the source queue has full native card function and the
		 * target queue has only stateless functions available, then
		 * there may be instructions that will not execute on the
		 * target queue.
		 *
		 * If the source queue has only stateless card functions and the
		 * target queue has full native card function available, then
		 * we are okay because the target queue can run all card
		 * functions.
		 */
		if (src_native) {
			dev_err(matrix_mdev->vdev.dev,
				"Full native card function available on source queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

			dev_err(matrix_mdev->vdev.dev,
				"Only stateless functions available on target queue %02lx.%04lx\n",
				AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
		}
	}
}

static void report_bs_compatibility(struct ap_matrix_mdev *matrix_mdev,
				    unsigned long apqn,
				    struct ap_tapq_hwinfo *src_hwinfo,
				    struct ap_tapq_hwinfo *target_hwinfo)
{
	if (src_hwinfo->bs || target_hwinfo->bs) {
		dev_err(matrix_mdev->vdev.dev,
			"Bind/associate state for source (%01x) and target (%01x) queue %02lx.%04lx are not compatible\n",
			src_hwinfo->bs, target_hwinfo->bs,
			AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));
	}
}

static void report_aptype_compatibility(struct ap_matrix_mdev *matrix_mdev,
					unsigned long apqn,
					struct ap_tapq_hwinfo *src_hwinfo,
					struct ap_tapq_hwinfo *target_hwinfo)
{
	dev_err(matrix_mdev->vdev.dev,
		"AP type of source (%02x) not compatible with target (%02x)\n",
		src_hwinfo->at, target_hwinfo->at);
}

/*
 * Log a device error reporting that migration failed due to queue
 * incompatibilities followed by a device error for each incompatible feature.
 */
static void report_qinfo_incompatibilities(struct ap_matrix_mdev *matrix_mdev,
					   unsigned long apqn,
					   struct ap_tapq_hwinfo *src_hwinfo,
					   struct ap_tapq_hwinfo *target_hwinfo)
{
	dev_err(matrix_mdev->vdev.dev,
		"Migration failed: Source and target queue (%02lx.%04lx) not compatible\n",
		AP_QID_CARD(apqn), AP_QID_QUEUE(apqn));

	report_facilities_compatibility(matrix_mdev, apqn, src_hwinfo, target_hwinfo);
	report_mode_compatibility(matrix_mdev, apqn, src_hwinfo, target_hwinfo);
	report_apxa_compatibility(matrix_mdev, apqn, src_hwinfo, target_hwinfo);
	report_slcf_compatibility(matrix_mdev, apqn, src_hwinfo, target_hwinfo);
	report_class_compatibility(matrix_mdev, apqn, src_hwinfo, target_hwinfo);
	report_aptype_compatibility(matrix_mdev, apqn, src_hwinfo, target_hwinfo);
	report_bs_compatibility(matrix_mdev, apqn, src_hwinfo, target_hwinfo);
}

static bool qinfo_compatible(struct ap_matrix_mdev *matrix_mdev,
			     unsigned long apqn,
			     struct ap_tapq_hwinfo *src_hwinfo,
			     struct ap_tapq_hwinfo *target_hwinfo)
{
	unsigned long src_bits, target_bits;

	src_bits = src_hwinfo->value & QINFO_DATA_MASK;
	target_bits = target_hwinfo->value & QINFO_DATA_MASK;

	/*
	 * If all relevant bits are the same, or only the AP type of the source
	 * and target queue differ but the source type is older than the target
	 * type, then no incompatibilities will be reported. The AP types are
	 * considered compatible even if they differ as long as the source type
	 * is older than the target type since AP devices are backwards
	 * compatible.
	 */
	if (src_bits == target_bits ||
	    (src_hwinfo->fac == target_hwinfo->fac &&
	     src_hwinfo->at <= target_hwinfo->at)) {
		return true;
	}

	report_qinfo_incompatibilities(matrix_mdev, apqn, src_hwinfo,
				       target_hwinfo);

	return false;
}

static int matrixes_compatible(struct vfio_ap_migration_file *migf)
{
	struct ap_matrix_mdev *matrix_mdev;
	struct ap_tapq_hwinfo src_hwinfo;
	struct vfio_ap_queue *q;
	unsigned long apqn;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	matrix_mdev = migf->matrix_mdev;

	for (int i = 0; i < migf->ap_config->num_queues; i++) {
		apqn = migf->ap_config->qinfo[i].apqn;
		q = vfio_ap_mdev_get_queue(matrix_mdev, apqn);
		memcpy(&src_hwinfo, &migf->ap_config->qinfo[i].data,
		       sizeof(src_hwinfo));

		if (!qinfo_compatible(matrix_mdev, apqn, &src_hwinfo, &q->hwinfo))
			return -EFAULT;
	}

	return 0;
}

static bool apqns_match(struct vfio_ap_migration_file *migf)
{
	struct ap_matrix_mdev *matrix_mdev;
	unsigned long apid, apqi, apqn;
	bool ret = true;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	matrix_mdev = migf->matrix_mdev;

	for (int i = 0; i < migf->ap_config->num_queues; i++) {
		apqn = migf->ap_config->qinfo[i].apqn;
		apid = AP_QID_CARD(apqn);
		apqi = AP_QID_QUEUE(apqn);

		if (!test_bit_inv(apid, matrix_mdev->shadow_apcb.apm) ||
		    !test_bit_inv(apqi, matrix_mdev->shadow_apcb.aqm)) {
			dev_err(matrix_mdev->vdev.dev,
				"migration failed: queue %02lx.%04lx not assigned to guest matrix\n",
				apid, apqi);
			ret = false;
		}
	}

	return ret;
}

static int vfio_ap_validate_num_queues(struct vfio_ap_migration_file *migf)
{
	int num_migf_queues, num_mdev_queues;
	struct ap_matrix_mdev *matrix_mdev;

	lockdep_assert_held(&matrix_dev->mdevs_lock);
	matrix_mdev = migf->matrix_mdev;
	num_mdev_queues = vfio_ap_mdev_get_num_queues(&matrix_mdev->shadow_apcb);
	num_migf_queues = migf->ap_config->num_queues;

	if (num_mdev_queues != num_migf_queues) {
		dev_err(matrix_mdev->vdev.dev,
			"migration failed: number of queues on source (%d) and target (%d) guests differ\n",
			num_migf_queues, num_mdev_queues);
		return (num_mdev_queues > num_migf_queues) ? -ENODEV : -E2BIG;
	}

	return 0;
}

static int do_post_copy_validation(struct vfio_ap_migration_file *migf, loff_t pos)
{
	unsigned long nqueues_offset;
	int ret;

	nqueues_offset = offsetofend(struct vfio_ap_config, num_queues);
	if (pos >= nqueues_offset) {
		ret = vfio_ap_validate_num_queues(migf);
		if (ret)
			return ret;

		if (pos == migf->config_sz) {
			if (!apqns_match(migf))
				return -ENODEV;
			ret = matrixes_compatible(migf);
			if (ret)
				return ret;
		}
	}

	return 0;
}

/**
 * vfio_ap_resume_write - store the AP configuration information sent from the
 *			  source guest into the migration file.
 * @filp: the file used to send the AP configuration information from the source
 *	  guest.
 * @buf:  buffer containing the AP configuration information sent from the
 *	  source guest
 * @len:  the length of the AP configuration information contained in @buf
 * *pos:  a pointer to store the file position after retrieving the AP config
 *	  information from @buf
 */
static ssize_t vfio_ap_resume_write(struct file *filp, const char __user *buf,
				    size_t len, loff_t *pos)
{
	struct vfio_ap_migration_data *mig_data;
	struct vfio_ap_migration_file *migf;
	ssize_t ret = 0;

	if (pos)
		return -ESPIPE;

	mutex_lock(&matrix_dev->mdevs_lock);

	pos = &filp->f_pos;
	migf = filp->private_data;
	mig_data = migf->matrix_mdev->mig_data;

	ret = validate_resume_write_parms(migf, len, pos);
	if (ret)
		goto out_unlock;

	if (copy_from_user((void *)migf->ap_config + *pos, buf, len)) {
		dev_err(migf->matrix_mdev->vdev.dev,
			"%s (%d): failed to copy queue information from userspace",
			__func__, __LINE__);
		ret = -EFAULT;
		goto out_unlock;
	}

	*pos += len;

	ret = do_post_copy_validation(migf, *pos);
	if (ret)
		goto out_unlock;

	ret = len;

	dev_dbg(migf->matrix_mdev->vdev.dev,
		"%s (%d): %zu bytes of queue information stored in the migration file",
		__func__, __LINE__, len);

out_unlock:
	mutex_unlock(&matrix_dev->mdevs_lock);

	return ret;
}

static const struct file_operations vfio_ap_resume_fops = {
	.owner = THIS_MODULE,
	.write = vfio_ap_resume_write,
	.release = vfio_ap_release_migf,
};

static struct vfio_ap_migration_file *
vfio_ap_resume_mdev_state(struct ap_matrix_mdev *matrix_mdev)
{
	struct vfio_ap_migration_data *mig_data;
	struct vfio_ap_migration_file *migf;
	struct file *filp;

	lockdep_assert_held(&matrix_dev->mdevs_lock);
	mig_data = matrix_mdev->mig_data;

	migf = vfio_ap_allocate_migf(matrix_mdev);
	if (IS_ERR(migf))
		return ERR_CAST(migf);

	filp = vfio_ap_open_file_stream(migf, &vfio_ap_resume_fops, O_WRONLY);
	if (IS_ERR(filp)) {
		vfio_ap_deallocate_migf(migf);
		return ERR_CAST(filp);
	}

	migf->matrix_mdev = matrix_mdev;
	migf->filp = filp;
	mig_data->resuming_migf = migf;

	return migf;
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

	/*
	 * Starts the process of restoring the state of the vfio-ap device
	 * on the target host by creating a filestream to be used to transfer
	 * the internal state of the vfio-ap device on the source host that
	 * was saved during the STOP_COPY phase of the migration.
	 */
	if (cur_state == VFIO_DEVICE_STATE_STOP &&
	    new_state == VFIO_DEVICE_STATE_RESUMING) {
		migf = vfio_ap_resume_mdev_state(matrix_mdev);
		if (IS_ERR(migf))
			return ERR_CAST(migf);

		get_file(migf->filp);

		return migf->filp;
	}

	/*
	 * Terminates the data transfer session of the vfio-ap device state
	 * between the source and target hosts. Since the vfio-ap device does
	 * not virtualize a DMA device, there is no internal device state to
	 * incorporate into the vfio-ap device on the target; so, the only
	 * thing left to do is release the migration files used to process
	 * the vfio device migration. Note that this state transition is for
	 * the vfio-ap device on the target host.
	 */
	if (cur_state == VFIO_DEVICE_STATE_RESUMING &&
	    new_state == VFIO_DEVICE_STATE_STOP) {
		vfio_ap_release_mig_files(matrix_mdev);

		return NULL;
	}

	/*
	 * Stop the operation of the vfio-ap device. Since the vfio-ap device
	 * does not virtualize a DMA device, there is no physical device to
	 * stop; so, the only thing left to do is release the migration files
	 * used to process the vfio device migration. Note that this state
	 * transition is for the vfio-ap device on the source host.
	 */
	if (cur_state == VFIO_DEVICE_STATE_STOP_COPY &&
	    new_state == VFIO_DEVICE_STATE_STOP) {
		vfio_ap_release_mig_files(matrix_mdev);

		return NULL;
	}

	/*
	 * These states indicates migration has either not been initiated or
	 * has completed and the vfio-ap device is operating normally; so
	 * just set the new migration state. Just in case, release the
	 * migration files used to facilitate migration if any are hanging
	 * around.
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
