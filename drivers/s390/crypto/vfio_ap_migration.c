// SPDX-License-Identifier: GPL-2.0
/*
 * Drives vfio_ap mdev migration.
 *
 * Copyright IBM Corp. 2025
 */
#include <linux/file.h>
#include "vfio_ap_private.h"

/* Magic number and version for the vfio_ap_config migration blob */
#define VFIO_AP_MIG_MAGIC			0x76666170U  /* "vfap" */
#define VFIO_AP_MIG_VERSION			1U

/**
 * struct vfio_ap_migration_file:
 *
 * This object is used for chunk processing of multiple reads and writes of
 * AP configuration information.
 *
 * @matrix_mdev:	object used for managing the state of the mdev
 * @lock:		mutex used for synchronizing read/write operations
 * @filp:		file stream used to read or write AP configuration data
 * @ap_config:		buffer used to store AP configuration data between read
 *			or write calls
 * @config_sz:		the size (in bytes) of @ap_config
 */
struct vfio_ap_migration_file {
	struct ap_matrix_mdev	*matrix_mdev;
	struct mutex		lock; /* protects read/write operations */
	struct file		*filp;
	void			*ap_config;
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

/**
 * vfio_ap_init_migration_data - initialize migration data and functions
 *
 * @matrix_mdev: pointer to object containing the mdev state
 *
 * This is called each time the vfio device is opened, and a vfio device can
 * be opened more than once. The migration data is not freed when the device
 * is closed; it is freed only when the vfio device is released. If it
 * already exists because the device was opened before, it is reused as is.
 *
 * Return: zero if initialization is successful; otherwise, returns a error.
 */
int vfio_ap_init_migration_data(struct ap_matrix_mdev *matrix_mdev)
{
	struct vfio_ap_migration_data *mig_data;

	lockdep_assert_held(&matrix_dev->mdevs_lock);

	if (matrix_mdev->mig_data)
		return 0;

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
 * vfio_ap_release_mig_files - reset the stop-copy and resuming migration files
 *
 * @matrix_mdev: The object used to maintain the state for a mediated device
 *
 * For each migration file, free the ap_config buffer, zero its size and set
 * its filp to NULL so any migration FD still open from a previous use of the
 * file is rejected when it is used. For the resuming file, also clear
 * resuming_mig_data_complete.
 *
 * The vfio_ap_migration_file objects themselves are not freed. They are
 * embedded in mig_data, which is freed by vfio_ap_release_migration_data().
 * That function is called from the vfio device's release callback, which
 * does not run until every registration reference on the vfio device has
 * been dropped. Since a reference is held for as long as a migration FD is
 * open, filp->private_data remains valid for the lifetime of the FD.
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
 *
 * This must only be called from the vfio device's release callback. It must
 * not be called when the vfio device is closed because a migration FD may
 * still be open and reference the migration data.
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
