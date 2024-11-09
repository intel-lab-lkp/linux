/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SCSI_SCSI_MULTIPATH_H
#define _SCSI_SCSI_MULTIPATH_H

#include <linux/list.h>
#include <linux/types.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/blk-mq.h>
#include <scsi/scsi.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>

struct scsi_device;

enum scsi_mpath_iopolicy {
	SCSI_MPATH_IOPOLICY_NUMA,
	SCSI_MPATH_IOPOLICY_RR,
};

enum scsi_mpath_access_state {
	SCSI_MPATH_OPTIMAL	= SCSI_ACCESS_STATE_OPTIMAL,
	SCSI_MPATH_ACTIVE	= SCSI_ACCESS_STATE_ACTIVE,
	SCSI_MPATH_STANDBY	= SCSI_ACCESS_STATE_STANDBY,
	SCSI_MPATH_UNAVAILABLE	= SCSI_ACCESS_STATE_UNAVAILABLE,
	SCSI_MPATH_LBA		= SCSI_ACCESS_STATE_LBA,
	SCSI_MPATH_OFFLINE	= SCSI_ACCESS_STATE_OFFLINE,
	SCSI_MPATH_TRANSITIONING = SCSI_ACCESS_STATE_TRANSITIONING,
	SCSI_MPATH_INVALID	= 0xFF
};

struct scsi_mpath_dh_data {
	const char	*hndlr_name; /* device Handler name */
	int	group_id;		/* Group ID reported from RTPG cmd */
	int	tpgs;			/* Target Port Groups reported from RTPG cmd */
	int	state;			/* Target Port Group State */
	char	*device_id_str;		/* Multipath Device String */
	int	device_id_len;		/* Device ID Length */
	int	valid_states;		/* states from RTPG cmd */
	int	prefrence;		/* Path prefrence for Port Group from RTPG cmd */
	int	is_active;		/* Current Sdev is active */
};

struct scsi_mpath {
	struct srcu_struct 	srcu;
	struct Scsi_Host	*shost;	/*Scsi_Host where this mpath belong */
	struct list_head        mpath_list;  /* list of multipath scsi_device   */
	struct	bio_list	mpath_requeue_list; /* list for requeing bio */
	spinlock_t		mpath_requeue_lock;
	struct work_struct	mpath_requeue_work; /* work struct for requeue */
	struct mutex            mpath_lock;
	unsigned long		mpath_start_time;
	struct delayed_work	activate_mpath; /* Path Activation work */
	struct scsi_device __rcu *current_path[]; /* scsi_device of current path */
};

extern void scsi_mpath_default_iopolicy(struct scsi_device *);
extern void scsi_mpath_unfreeze(struct Scsi_Host *);
extern void scsi_mpath_wait_freeze(struct Scsi_Host *);
extern void scsi_mpath_start_freeze(struct Scsi_Host *);
extern void scsi_mpath_failover_req(struct request *);
extern void scsi_mpath_start_request(struct request *);
extern void scsi_mpath_end_request(struct request *);
extern void scsi_kick_requeue_lists(struct Scsi_Host *);
extern bool scsi_mpath_clear_current_path(struct scsi_device *);
int scsi_multipath_init(struct scsi_device *);
extern int scsi_mpath_failover_disposition(struct scsi_cmnd *);
int scsi_mpath_alloc_disk(struct scsi_device *);
extern void scsi_mpath_remove_disk(struct scsi_device *);
extern void scsi_mpath_shutdown_disk(struct scsi_device *);
void scsi_put_mpath_sdev(struct scsi_device *);
void scsi_mpath_requeue_work(struct work_struct *);
extern void scsi_mpath_dev_release(struct scsi_device *);
void scsi_mpath_kick_requeue_lists(struct Scsi_Host *);
int scsi_mpath_update_state(struct scsi_device *);
extern int scsi_mpath_add_disk(struct scsi_device *);
void scsi_mpath_set_live(struct scsi_device *);
void scsi_activate_path(struct scsi_device *);
void scsi_multipath_iopolicy_update(struct scsi_device *, int);
void scsi_mpath_clear_paths(struct Scsi_Host *);
int scsi_mpath_unique_lun_id(struct scsi_device *);

extern void scsi_mpath_revalidate_path(struct gendisk *, sector_t);
extern int scsi_mpath_unique_id(struct scsi_device *sdev, u8 id[16], enum blk_unique_id type);
#endif /* _SCSI_SCSI_MULTIPATH_H */
