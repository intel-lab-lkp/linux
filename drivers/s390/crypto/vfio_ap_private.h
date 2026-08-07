/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Private data and functions for adjunct processor VFIO matrix driver.
 *
 * Author(s): Tony Krowiak <akrowiak@linux.ibm.com>
 *	      Halil Pasic <pasic@linux.ibm.com>
 *	      Pierre Morel <pmorel@linux.ibm.com>
 *
 * Copyright IBM Corp. 2018
 */

#ifndef _VFIO_AP_PRIVATE_H_
#define _VFIO_AP_PRIVATE_H_

#include <linux/types.h>
#include <linux/mdev.h>
#include <linux/delay.h>
#include <linux/eventfd.h>
#include <linux/mutex.h>
#include <linux/kvm_host.h>
#include <linux/vfio.h>
#include <linux/hashtable.h>

#include "ap_bus.h"

#define VFIO_AP_MODULE_NAME "vfio_ap"
#define VFIO_AP_DRV_NAME "vfio_ap"

/**
 * struct ap_matrix_dev - Contains the data for the matrix device.
 *
 * @device:	generic device structure associated with the AP matrix device
 * @info:	the struct containing the output from the PQAP(QCI) instruction
 * @mdev_list:	the list of mediated matrix devices created
 * @mdevs_lock: mutex for locking the AP matrix device. This lock will be
 *		taken every time we fiddle with state managed by the vfio_ap
 *		driver, be it using @mdev_list or writing the state of a
 *		single ap_matrix_mdev device. It's quite coarse but we don't
 *		expect much contention.
 * @vfio_ap_drv: the vfio_ap device driver
 * @guests_lock: mutex for controlling access to a guest that is using AP
 *		 devices passed through by the vfio_ap device driver. This lock
 *		 will be taken when the AP devices are plugged into or unplugged
 *		 from a guest, and when an ap_matrix_mdev device is added to or
 *		 removed from @mdev_list or the list is iterated.
 */
struct ap_matrix_dev {
	struct device device;
	struct ap_config_info info;
	struct list_head mdev_list;
	struct mutex mdevs_lock; /* serializes access to each ap_matrix_mdev */
	struct ap_driver  *vfio_ap_drv;
	struct mutex guests_lock; /* serializes access to each KVM guest */
	struct mdev_parent parent;
	struct mdev_type mdev_type;
	struct mdev_type *mdev_types;
};

extern struct ap_matrix_dev *matrix_dev;

/**
 * struct ap_matrix - matrix of adapters, domains and control domains
 *
 * @apm_max: max adapter number in @apm
 * @apm: identifies the AP adapters in the matrix
 * @aqm_max: max domain number in @aqm
 * @aqm: identifies the AP queues (domains) in the matrix
 * @adm_max: max domain number in @adm
 * @adm: identifies the AP control domains in the matrix
 *
 * The AP matrix is comprised of three bit masks identifying the adapters,
 * queues (domains) and control domains that belong to an AP matrix. The bits in
 * each mask, from left to right, correspond to IDs 0 to 255. When a bit is set
 * the corresponding ID belongs to the matrix.
 */
struct ap_matrix {
	unsigned long apm_max;
	DECLARE_BITMAP(apm, AP_DEVICES);
	unsigned long aqm_max;
	DECLARE_BITMAP(aqm, AP_DOMAINS);
	unsigned long adm_max;
	DECLARE_BITMAP(adm, AP_DOMAINS);
};

/**
 * struct ap_queue_table - a table of queue objects.
 *
 * @queues: a hashtable of queues (struct vfio_ap_queue).
 */
struct ap_queue_table {
	DECLARE_HASHTABLE(queues, 8);
};

/* Forward declaration for migration data structure */
struct vfio_ap_migration_data;

/**
 * struct ap_matrix_mdev - Contains the data associated with a matrix mediated
 *			   device.
 * @vdev:	the vfio device
 * @node:	allows the ap_matrix_mdev struct to be added to a list
 * @matrix:	the adapters, usage domains and control domains assigned to the
 *		mediated matrix device.
 * @shadow_apcb:    the shadow copy of the APCB field of the KVM guest's CRYCB
 * @kvm:	the struct holding guest's state
 * @pqap_hook:	the function pointer to the interception handler for the
 *		PQAP(AQIC) instruction.
 * @mdev:	the mediated device
 * @qtable:	table of queues (struct vfio_ap_queue) assigned to the mdev
 * @req_trigger eventfd ctx for signaling userspace to return a device
 * @cfg_chg_trigger eventfd ctx to signal AP config changed to userspace
 * @apm_add:	bitmap of APIDs added to the host's AP configuration
 * @aqm_add:	bitmap of APQIs added to the host's AP configuration
 * @adm_add:	bitmap of control domain numbers added to the host's AP
 *		configuration
 * @mig_data:  vfio device migration data
 */
struct ap_matrix_mdev {
	struct vfio_device vdev;
	struct list_head node;
	struct ap_matrix matrix;
	struct ap_matrix shadow_apcb;
	struct kvm *kvm;
	crypto_hook pqap_hook;
	struct mdev_device *mdev;
	struct ap_queue_table qtable;
	struct eventfd_ctx *req_trigger;
	struct eventfd_ctx *cfg_chg_trigger;
	DECLARE_BITMAP(apm_add, AP_DEVICES);
	DECLARE_BITMAP(aqm_add, AP_DOMAINS);
	DECLARE_BITMAP(adm_add, AP_DOMAINS);
	struct vfio_ap_migration_data *mig_data;
};

/**
 * struct vfio_ap_queue - contains the data associated with a queue bound to the
 *			  vfio_ap device driver
 * @matrix_mdev: the matrix mediated device
 * @saved_iova: the notification indicator byte (nib) address
 * @apqn: the APQN of the AP queue device
 * @saved_isc: the guest ISC registered with the GIB interface
 * @mdev_qnode: allows the vfio_ap_queue struct to be added to a hashtable
 * @reset_qnode: allows the vfio_ap_queue struct to be added to a list of queues
 *		 that need to be reset
 * @reset_status: the status from the last reset of the queue
 * @reset_work: work to wait for queue reset to complete
 */
struct vfio_ap_queue {
	struct ap_matrix_mdev *matrix_mdev;
	dma_addr_t saved_iova;
	int	apqn;
#define VFIO_AP_ISC_INVALID 0xff
	unsigned char saved_isc;
	struct hlist_node mdev_qnode;
	struct list_head reset_qnode;
	struct ap_queue_status reset_status;
	struct work_struct reset_work;
};

/**
 * get_update_locks_for_mdev: Acquire the locks required to dynamically update a
 *			      KVM guest's APCB in the proper order.
 *
 * @matrix_mdev: a pointer to a struct ap_matrix_mdev object containing the AP
 *		 configuration data to use to update a KVM guest's APCB.
 *
 * The proper locking order is:
 * 1. matrix_dev->guests_lock: required to use the KVM pointer to update a KVM
 *			       guest's APCB.
 * 2. matrix_mdev->kvm->lock:  required to update a guest's APCB
 * 3. matrix_dev->mdevs_lock:  required to access data stored in a matrix_mdev
 *
 * Note: If @matrix_mdev is NULL or is not attached to a KVM guest, the KVM
 *	 lock will not be taken.
 */
static inline void get_update_locks_for_mdev(struct ap_matrix_mdev *matrix_mdev)
{
	mutex_lock(&matrix_dev->guests_lock);
	if (matrix_mdev && matrix_mdev->kvm)
		mutex_lock(&matrix_mdev->kvm->lock);
	mutex_lock(&matrix_dev->mdevs_lock);
}

/**
 * release_update_locks_for_mdev: Release the locks used to dynamically update a
 *				  KVM guest's APCB in the proper order.
 *
 * @matrix_mdev: a pointer to a struct ap_matrix_mdev object containing the AP
 *		 configuration data to use to update a KVM guest's APCB.
 *
 * The proper unlocking order is:
 * 1. matrix_dev->mdevs_lock
 * 2. matrix_mdev->kvm->lock
 * 3. matrix_dev->guests_lock
 *
 * Note: If @matrix_mdev is NULL or is not attached to a KVM guest, the KVM
 *	 lock will not be released.
 */
static inline void release_update_locks_for_mdev(struct ap_matrix_mdev *matrix_mdev)
{
	mutex_unlock(&matrix_dev->mdevs_lock);
	if (matrix_mdev && matrix_mdev->kvm)
		mutex_unlock(&matrix_mdev->kvm->lock);
	mutex_unlock(&matrix_dev->guests_lock);
}

static inline void
assert_has_update_locks_for_mdev(struct ap_matrix_mdev *matrix_mdev)
{
	lockdep_assert_held(&matrix_dev->guests_lock);
	if (matrix_mdev && matrix_mdev->kvm)
		lockdep_assert_held(&matrix_mdev->kvm->lock);
	lockdep_assert_held(&matrix_dev->mdevs_lock);
}

int vfio_ap_mdev_get_num_queues(struct ap_matrix *ap_matrix);

int vfio_ap_mdev_register(void);
void vfio_ap_mdev_unregister(void);

int vfio_ap_mdev_probe_queue(struct ap_device *queue);
void vfio_ap_mdev_remove_queue(struct ap_device *queue);

int vfio_ap_mdev_resource_in_use(unsigned long *apm, unsigned long *aqm);

void vfio_ap_on_cfg_changed(struct ap_config_info *new_config_info,
			    struct ap_config_info *old_config_info);
void vfio_ap_on_scan_complete(struct ap_config_info *new_config_info,
			      struct ap_config_info *old_config_info);

void vfio_ap_matrix_init(struct ap_config_info *info, struct ap_matrix *matrix);

void vfio_ap_init_migration_capabilities(struct ap_matrix_mdev *matrix_mdev);
int vfio_ap_init_migration_data(struct ap_matrix_mdev *matrix_mdev);
void vfio_ap_release_migration_data(struct ap_matrix_mdev *matrix_mdev);
void vfio_ap_reset_migration_state(struct ap_matrix_mdev *matrix_mdev);

int vfio_ap_set_new_guest_config(struct ap_matrix_mdev *matrix_mdev,
				 struct ap_matrix *m_new);

#endif /* _VFIO_AP_PRIVATE_H_ */
