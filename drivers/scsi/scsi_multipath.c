// SPDX-License-Indentifier: GPL-2.0
/*
 * Copyright (c) 2024 Himanshu Madhani
 *
 * SCSI Multipath support using ALUA (Asymmetric Logical Unit Access)
 * capable devices.
 */

#include <linux/bio.h>
#include <linux/moduleparam.h>
#include <linux/topology.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_dh.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_multipath.h>

bool scsi_multipath = true;
module_param(scsi_multipath, bool, 0444);
MODULE_PARM_DESC(scsi_multipath,
    "turn on native support for multiple scsi devices \n"
    "set this value to false to disable multipath, \n");

static const char *scsi_iopolicy_names[] = {
	[SCSI_MPATH_IOPOLICY_NUMA]	= "numa",
	[SCSI_MPATH_IOPOLICY_RR]	= "round-robin",
};

static int iopolicy = SCSI_MPATH_IOPOLICY_NUMA;

/*
 * SCSI multipath will only allow 'NUMA' or 'round-robin' policy for IO.
 * In Future, if more apropriate IO-policy is introduced will be added
 * based on community feedback.
 */
static int scsi_set_iopolicy(const char *val, const struct kernel_param *kp)
{
	if (!val)
		return -EINVAL;
	if (!strncmp(val, "numa", 4))
		iopolicy = SCSI_MPATH_IOPOLICY_NUMA;
	else if (!strncmp(val, "round-robin", 11))
		iopolicy = SCSI_MPATH_IOPOLICY_RR;
	else
		return -EINVAL;

	return 0;
}

static int scsi_get_iopolicy(char *buf, const struct kernel_param *kp)
{
	return sprintf(buf, "%s\n", scsi_iopolicy_names[iopolicy]);
}

module_param_call(iopolicy, scsi_set_iopolicy, scsi_get_iopolicy,
    &iopolicy, 0644);
MODULE_PARM_DESC(iopolicy,
    "Default multipath I/O policy; 'numa' (default) or 'round-robin'");

void scsi_mpath_default_iopolicy(struct scsi_device *sdev)
{
	sdev->mpath_iopolicy = iopolicy;
}

void scsi_multipath_iopolicy_update(struct scsi_device *sdev, int iopolicy)
{
	struct Scsi_Host *shost =  sdev->host;
	struct scsi_mpath *mpath_dev = shost->mpath_dev;
	int old_iopolicy = READ_ONCE(sdev->mpath_iopolicy);

	if (old_iopolicy == iopolicy)
		return;

	WRITE_ONCE(sdev->mpath_iopolicy, iopolicy);

	/* iopoliocy changes clear the multipath */
	mutex_lock(&mpath_dev->mpath_lock);
	list_for_each_entry_rcu(sdev, &shost->mpath_sdev, mpath_entry)
		scsi_mpath_clear_paths(shost);
	mutex_unlock(&mpath_dev->mpath_lock);

	sdev_printk(KERN_NOTICE, sdev, "Multipath iopolocy changed from %s to %s\n",
	    scsi_iopolicy_names[old_iopolicy], scsi_iopolicy_names[iopolicy]);
}

bool scsi_mpath_clear_current_path(struct scsi_device *sdev)
{
	struct Scsi_Host *shost = sdev->host;
	struct scsi_mpath *mpath_dev = shost->mpath_dev;
	bool changed = false;
	int node;

	if (!sdev)
		return changed;

	for_each_node(node) {
		if (sdev == rcu_access_pointer(mpath_dev->current_path[node])) {
			rcu_assign_pointer(mpath_dev->current_path[node], NULL);
			changed = true;
		}
	}

	return changed;
}
EXPORT_SYMBOL_GPL(scsi_mpath_clear_current_path);

void scsi_mpath_clear_paths(struct Scsi_Host *shost)
{
	struct scsi_device *sdev;
	int srcu_idx;

	srcu_idx = srcu_read_lock(&shost->mpath_dev->srcu);
	list_for_each_entry_rcu(sdev, &shost->mpath_sdev, mpath_entry) {
		scsi_mpath_clear_current_path(sdev);
		kblockd_schedule_work(&shost->mpath_dev->mpath_requeue_work);
	}
	srcu_read_unlock(&shost->mpath_dev->srcu, srcu_idx);

}

static inline bool scsi_mpath_state_is_live(enum scsi_mpath_access_state state)
{
	if (state == SCSI_MPATH_OPTIMAL ||
	    state == SCSI_MPATH_ACTIVE)
		return true;

	return false;
}

/* Check for path error */
static inline bool scsi_is_mpath_error(struct scsi_cmnd *scmd)
{
	struct request *req = scsi_cmd_to_rq(scmd);
	struct scsi_device *sdev = req->q->queuedata;

	if (sdev->handler && sdev->handler->prep_fn) {
		blk_status_t ret = sdev->handler->prep_fn(sdev, req);

		if (ret != BLK_STS_OK)
			return true;
	}

	return false;
}

static bool scsi_mpath_is_disabled(struct scsi_device *sdev)
{
	enum scsi_device_state sdev_state = sdev->sdev_state;

	/*
	 * if device multipath state is not set to LIVE
	 * then return true
	 */
	if (!scsi_mpath_state_is_live(sdev->mpath_state))
		return true;

	/*
	 * Do not treat DELETING as a disabled path as I/O should
	 * still be able to complete assuming that scsi_device is
	 * within timeout limit.
	 * Otherwise I/O will fail immeadiately and return to
	 * requeue list
	 */
	if (sdev_state != SDEV_RUNNING && sdev_state != SDEV_CANCEL)
		return true;

	return false;
}

/* handle failover request for path */
void scsi_mpath_failover_req(struct request *req)
{
	struct scsi_cmnd *scmd = blk_mq_rq_to_pdu(req);
	struct scsi_device *sdev = scmd->device;
	struct Scsi_Host *shost = scmd->device->host;
	struct scsi_mpath *mpath_dev = shost->mpath_dev;
	unsigned long flags;
	struct bio *bio;

	if (!scsi_device_online(sdev) || sdev->was_reset || sdev->locked)
		return;

	scsi_mpath_clear_current_path(sdev);

	/*
	 * if we got device handler error, we know that device is alive but not
	 * ready to process command. kick off a requeue of scsi command and try
	 * other available path
	 */
	if (scsi_is_mpath_error(scmd)) {
		/*
		 * Set flag as pending and requeue bio for retry on
		 * another path
		 */
		set_bit(SCSI_MPATH_DISK_IO_PENDING, &sdev->mpath_flags);
		queue_work(shost->work_q, &mpath_dev->mpath_requeue_work);
	}

	/*
	 * following logic tries to steal bio, check if the bio has polled
	 * operation, if yes, then clear polled reqeust and reqeue bio
	 */
	spin_lock_irqsave(&mpath_dev->mpath_requeue_lock, flags);
	for (bio = req->bio; bio; bio = bio->bi_next) {
		bio_set_dev(bio, req->q->disk->part0);
		if (bio->bi_opf & REQ_POLLED) {
			bio->bi_opf &= ~REQ_POLLED;
			bio->bi_cookie = BLK_QC_T_NONE;
		}
	}
	blk_steal_bios(&mpath_dev->mpath_requeue_list, req);
	spin_unlock_irqrestore(&mpath_dev->mpath_requeue_lock, flags);

	scmd->result = 0;

	blk_mq_end_request(req, 0);

	kblockd_schedule_work(&mpath_dev->mpath_requeue_work);
}
EXPORT_SYMBOL_GPL(scsi_mpath_failover_req);

static inline bool scsi_mpath_is_optimized(struct scsi_device *sdev)
{
	return (!scsi_device_online(sdev) &&
	    ((sdev->mpath_state == SCSI_MPATH_OPTIMAL) ||
	     (sdev->mpath_state == SCSI_MPATH_ACTIVE)));
}

static struct scsi_device *scsi_next_mpath_sdev(struct Scsi_Host *shost,
			struct scsi_device *sdev)
{
	sdev = list_next_or_null_rcu(&shost->mpath_sdev, &sdev->siblings,
	    struct scsi_device, siblings);

	if (sdev)
		return sdev;

	return list_first_or_null_rcu(&shost->mpath_sdev, struct scsi_device,
	    siblings);
}

static struct scsi_device *scsi_mpath_round_robin_path(struct Scsi_Host *shost,
	int node, struct scsi_device *old_sdev)
{
	struct scsi_device *sdev, *found = NULL;
	struct scsi_mpath *mpath_dev = shost->mpath_dev;

	if (list_is_singular(&shost->mpath_sdev)) {
		if(scsi_mpath_is_disabled(old_sdev))
			return NULL;
		return old_sdev;
	}

	for (sdev = scsi_next_mpath_sdev(shost, old_sdev);
	    sdev && sdev != old_sdev;
	    sdev = scsi_next_mpath_sdev(shost, sdev)) {
		if (scsi_mpath_is_disabled(sdev))
			continue;
		if (sdev->mpath_state == SCSI_MPATH_OPTIMAL) {
			found = sdev;
			goto out;
		}
		if (sdev->mpath_state == SCSI_MPATH_ACTIVE)
			found = sdev;
	}

	if (!scsi_mpath_is_disabled(old_sdev) &&
	    (old_sdev->mpath_state == SCSI_MPATH_OPTIMAL ||
	    (!found && old_sdev->mpath_state == SCSI_MPATH_ACTIVE)))
		return old_sdev;

	if (!found)
		return NULL;
out:
	rcu_assign_pointer(mpath_dev->current_path[node], found);

	return found;
}

/*
 * Search path based on iopolicy and numa node affinity
 * and return the scsi_device for that path
 */
inline struct scsi_device *__scsi_find_path(struct Scsi_Host *shost, int node)
{
	struct scsi_mpath *mpath_dev = shost->mpath_dev;
	int found_distance = INT_MAX, fallback_distance = INT_MAX, distance;
	struct scsi_device *sdev_found = NULL, *sdev_fallback = NULL, *sdev;

	list_for_each_entry_rcu(sdev, &shost->mpath_sdev, mpath_entry) {
		if (scsi_mpath_is_disabled(sdev))
			continue;

		if (sdev->mpath_numa_node != NUMA_NO_NODE &&
		    (READ_ONCE(sdev->mpath_iopolicy) == SCSI_MPATH_IOPOLICY_NUMA))
			distance = node_distance(node, sdev->mpath_numa_node);
		else
			distance = LOCAL_DISTANCE;

		switch(sdev->mpath_state) {
		case SCSI_MPATH_OPTIMAL:
		    if (distance < found_distance) {
			    found_distance = distance;
			    sdev_found = sdev;
		    }
		    break;
		case SCSI_MPATH_ACTIVE:
		    if (distance < fallback_distance) {
			    fallback_distance = distance;
			    sdev_fallback = sdev;
		    }
		    break;
		default:
		    break;
		}
	}

	if (!sdev_found)
		sdev_found = sdev_fallback;

	if (sdev_found)
		rcu_assign_pointer(mpath_dev->current_path[node], sdev_found);

	return sdev_found;
}

inline struct scsi_device *scsi_find_path(struct Scsi_Host *shost)
{
	int node = numa_node_id();
	struct scsi_device *sdev;

	sdev = srcu_dereference(shost->mpath_dev->current_path[node],
	    &shost->mpath_dev->srcu);

	if (unlikely(!sdev))
		sdev = __scsi_find_path(shost, node);

	if (READ_ONCE(sdev->mpath_iopolicy) == SCSI_MPATH_IOPOLICY_RR)
		return scsi_mpath_round_robin_path(shost, node, sdev);

	if (unlikely(!scsi_mpath_is_optimized(sdev)))
		return __scsi_find_path(shost, node);

	return sdev;
}

void scsi_mpath_requeue_work(struct work_struct *work)
{
	struct scsi_mpath *mpath_dev =
	    container_of(work, struct scsi_mpath, mpath_requeue_work);
	struct bio *bio, *next;

	spin_lock_irq(&mpath_dev->mpath_requeue_lock);
	next = bio_list_get(&mpath_dev->mpath_requeue_list);
	spin_unlock(&mpath_dev->mpath_requeue_lock);

	while ((bio = next) != NULL) {
		next = bio->bi_next;
		bio->bi_next = NULL;
		submit_bio_noacct(bio);
	}
}

void scsi_mpath_set_live(struct scsi_device *sdev)
{
	struct Scsi_Host *shost = sdev->host;
	struct scsi_mpath *mpath_dev = shost->mpath_dev;
	int ret;

	if (!sdev->mpath_disk)
		return;

	if (!test_and_set_bit(SCSI_MPATH_DISK_LIVE, &sdev->mpath_flags)) {
		ret = device_add_disk(&sdev->sdev_dev, sdev->mpath_disk, NULL);
		if (ret) {
			clear_bit(SCSI_MPATH_DISK_LIVE, &sdev->mpath_flags);
			return;
		}
	}

	pr_info("Attached SCSI %s disk\n", sdev->mpath_disk->disk_name);

	mutex_lock(&mpath_dev->mpath_lock);
	if (scsi_mpath_is_optimized(sdev)) {
		int node, srcu_idx;

		srcu_idx = srcu_read_lock(&mpath_dev->srcu);
		for_each_online_node(node)
			__scsi_find_path(shost, node);
		srcu_read_unlock(&mpath_dev->srcu, srcu_idx);
	}
	mutex_unlock(&mpath_dev->mpath_lock);

	synchronize_srcu(&mpath_dev->srcu);
	kblockd_schedule_work(&mpath_dev->mpath_requeue_work);
}

/**
 * Callback function for activating multipath devices
 */
static void activate_mpath(void *data, int err)
{
	struct scsi_device *sdev = data;
	struct scsi_mpath_dh_data *mpath_h = sdev->mpath_pg_data;
	bool retry = false;

	if (!mpath_h)
		return;

	switch (err) {
	case SCSI_DH_OK:
		break;
	case SCSI_DH_NOSYS:
		sdev_printk(KERN_ERR, sdev,
			"Could not failover the device scsi_dh_%s, Error %d\n",
			sdev->handler->name, err);
		scsi_mpath_clear_current_path(sdev);
		break;
	case SCSI_DH_DEV_TEMP_BUSY:
		sdev_printk(KERN_ERR, sdev,
			"Device Handler Path Busy\n");
		break;
	case SCSI_DH_RETRY:
		sdev_printk(KERN_ERR, sdev,
			"Device Handler Path Retry \n");
		retry = true;
		fallthrough;
	case SCSI_DH_IMM_RETRY:
	case SCSI_DH_RES_TEMP_UNAVAIL:
		sdev_printk(KERN_ERR, sdev,
			"Device Handler Path Unavailable, Clear current path \n");
		if ((mpath_h->state == SCSI_ACCESS_STATE_OFFLINE) ||
		    (mpath_h->state == SCSI_ACCESS_STATE_UNAVAILABLE))
			scsi_mpath_clear_current_path(sdev);
		err = 0;
		break;
	case SCSI_DH_DEV_OFFLINED:
	default:
		sdev_printk(KERN_ERR, sdev, "Device Handler Path offlined \n");
		scsi_mpath_clear_current_path(sdev);
		break;
	}

	if (retry)
		set_bit(SCSI_MPATH_DISK_IO_PENDING, &sdev->mpath_flags);

        if (scsi_mpath_state_is_live(sdev->mpath_state))
		scsi_mpath_set_live(sdev);
}

void scsi_activate_path(struct scsi_device *sdev)
{
	struct request_queue *q = sdev->mpath_disk->queue;
	struct scsi_mpath_dh_data *mpath_dh = sdev->mpath_pg_data;

	if (!mpath_dh)
		return;

        if (!(scsi_mpath_state_is_live(sdev->mpath_state))) {
		sdev_printk(KERN_INFO, sdev, "Path state is not live \n");
                return;
	}

	if (!blk_queue_dying(q))
		scsi_dh_activate(q, activate_mpath, sdev);
	else
		activate_mpath(sdev, SCSI_DH_OK);
}

static void scsi_activate_mpath_work(struct work_struct *work)
{
        struct scsi_device *sdev = container_of(work,
            struct scsi_device, activate_mpath);

	if (!sdev)
		return;

	scsi_activate_path(sdev);
}

int scsi_mpath_add_disk(struct scsi_device *sdev)
{
	if (!sdev->mpath_pg_data) {
		/* Re initialize ALUA */
		sdev->handler->rescan(sdev);
	} else {
		sdev->mpath_state = SCSI_MPATH_OPTIMAL;
		scsi_mpath_set_live(sdev);
	}

	return (test_bit(SCSI_MPATH_DISK_LIVE, &sdev->mpath_flags));
}
EXPORT_SYMBOL_GPL(scsi_mpath_add_disk);

int scsi_multipath_init(struct scsi_device *sdev)
{
	struct Scsi_Host *shost = sdev->host;
	struct scsi_mpath_dh_data *h;
	struct scsi_mpath *mpath_dev;
	int ret = -ENOMEM;

	mpath_dev = kzalloc(sizeof(struct scsi_mpath), GFP_KERNEL);
	if (!mpath_dev)
		return ret;

	h = kzalloc(sizeof(struct scsi_mpath_dh_data), GFP_KERNEL);
	if (!h)
		goto out_mpath_dev;

	sdev->mpath_pg_data = h;

	ret = init_srcu_struct(&mpath_dev->srcu);
	if (ret) {
		cleanup_srcu_struct(&mpath_dev->srcu);
		goto out_handler;
	}

	shost->mpath_dev = mpath_dev;

	mutex_init(&mpath_dev->mpath_lock);
	bio_list_init(&mpath_dev->mpath_requeue_list);
	spin_lock_init(&mpath_dev->mpath_requeue_lock);
	INIT_WORK(&mpath_dev->mpath_requeue_work, scsi_mpath_requeue_work);
	INIT_LIST_HEAD(&mpath_dev->mpath_list);
	INIT_WORK(&sdev->activate_mpath, scsi_activate_mpath_work);
	INIT_LIST_HEAD(&sdev->mpath_entry);
	sdev->mpath_numa_node = NUMA_NO_NODE;
	sdev->is_shared = 1;

	return 0;

out_handler:
	kfree(h);
out_mpath_dev:
	if (mpath_dev)
		kfree(mpath_dev);

	return ret;
}
EXPORT_SYMBOL_GPL(scsi_multipath_init);

static bool scsi_available_mpath(struct Scsi_Host *shost)
{
	struct scsi_device *sdev;

	list_for_each_entry_rcu(sdev, &shost->mpath_sdev, mpath_entry) {
		if (scsi_device_online(sdev))
			return true;
	}
	return false;
}

/*  called when shost is being freed */
void scsi_mpath_dev_release(struct scsi_device *sdev)
{
	struct Scsi_Host *shost = sdev->host;
	struct scsi_mpath *mpath_dev;

	if (!shost->mpath_dev)
		return;

	mpath_dev = shost->mpath_dev;
	cancel_work_sync(&mpath_dev->mpath_requeue_work);
	cleanup_srcu_struct(&mpath_dev->srcu);

	if (sdev->mpath_pg_data)
                kfree(sdev->mpath_pg_data);
}
EXPORT_SYMBOL_GPL(scsi_mpath_dev_release);

void scsi_put_mpath_sdev(struct scsi_device *sdev)
{
	scsi_device_put(sdev);
}

void scsi_mpath_revalidate_path(struct gendisk *mpath_disk, sector_t capacity)
{
	struct Scsi_Host *shost = mpath_disk->private_data;
	struct scsi_mpath *mpath_dev = shost->mpath_dev;
	struct scsi_device *sdev;
	int srcu_idx;
	int node;

	if (!shost->mpath_dev)
		return;

	srcu_idx = srcu_read_lock(&mpath_dev->srcu);
	list_for_each_entry_rcu(sdev, &shost->mpath_sdev, mpath_entry) {
		if (capacity != get_capacity(sdev->mpath_disk))
			clear_bit(SCSI_MPATH_DISK_LIVE, &sdev->mpath_flags);
	}
	srcu_read_unlock(&mpath_dev->srcu, srcu_idx);

	for_each_node(node)
		rcu_assign_pointer(mpath_dev->current_path[node], NULL);
	kblockd_schedule_work(&mpath_dev->mpath_requeue_work);
}
EXPORT_SYMBOL_GPL(scsi_mpath_revalidate_path);

static int scsi_mpath_open(struct gendisk *disk, blk_mode_t mode)
{
	if (!scsi_get_device(disk->private_data))
		return -ENXIO;

	return 0;
}

static void scsi_mpath_release(struct gendisk *disk)
{
	struct Scsi_Host *shost = disk->private_data;
	struct scsi_device *sdev;
	int srcu_idx;

	srcu_idx = srcu_read_lock(&shost->mpath_dev->srcu);
	sdev = scsi_find_path(shost);
	srcu_read_unlock(&shost->mpath_dev->srcu, srcu_idx);
}

int scsi_mpath_failover_disposition(struct scsi_cmnd *scmd)
{
	struct request *req = scsi_cmd_to_rq(scmd);

	if (req->cmd_flags & REQ_SCSI_MPATH) {
		if (scsi_is_mpath_error(scmd) ||
		    blk_queue_dying(req->q)) {
			return NEEDS_RETRY;
		}
	} else {
		if (blk_queue_dying(req->q))
			return SUCCESS;
	}

	return SUCCESS;
}
EXPORT_SYMBOL_GPL(scsi_mpath_failover_disposition);

static void scsi_multipath_submit_bio(struct bio *bio)
{
	struct Scsi_Host *shost = bio->bi_bdev->bd_disk->private_data;
	struct scsi_mpath *mpath_dev = shost->mpath_dev;
	struct scsi_device *sdev;
	int srcu_idx;

	/*
	 * The scsi device might be going away and the bio might be
	 * moved to a difference queue via blk_steal_bios(), so we
	 * need to use bio_split pool from the original queue to
	 * allocate the bvecs from.
	 */
	bio = bio_split_to_limits(bio);
	if (!bio)
		return;

	srcu_idx = srcu_read_lock(&mpath_dev->srcu);
	sdev = scsi_find_path(shost);
	if (likely(sdev)) {
		bio_set_dev(bio, bio->bi_bdev->bd_disk->part0);
		bio->bi_opf |= REQ_SCSI_MPATH;
		submit_bio_noacct(bio);
	} else if (scsi_available_mpath(shost)) {
		sdev_printk(KERN_NOTICE, NULL,
		    "No Usable Path - Requeing I/O \n");

		spin_lock_irq(&mpath_dev->mpath_requeue_lock);
		bio_list_add(&mpath_dev->mpath_requeue_list, bio);
		spin_unlock_irq(&mpath_dev->mpath_requeue_lock);
	} else {
		sdev_printk(KERN_NOTICE, NULL,
		    "No available path = Failing I/O \n");

		bio_io_error(bio);
	}
	srcu_read_unlock(&mpath_dev->srcu, srcu_idx);
}

static int scsi_mpath_get_unique_id(struct gendisk *disk, u8 id[16],
    enum blk_unique_id type)
{
	struct Scsi_Host *shost = disk->private_data;
	struct scsi_device *sdev;
	int srcu_idx, ret = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&shost->mpath_dev->srcu);
	sdev = scsi_find_path(shost);
	if (sdev)
		ret = scsi_mpath_unique_id(sdev, id, type);
	srcu_read_unlock(&shost->mpath_dev->srcu, srcu_idx);

	return ret;
}

const struct block_device_operations scsi_mpath_ops = {
	.owner          = THIS_MODULE,
	.submit_bio	= scsi_multipath_submit_bio,
	.open		= scsi_mpath_open,
	.release	= scsi_mpath_release,
	.get_unique_id	= scsi_mpath_get_unique_id,
};

int scsi_mpath_unique_id(struct scsi_device *sdev, u8 id[16],
		enum blk_unique_id type)
{
	struct scsi_mpath_dh_data *dh_data = sdev->mpath_pg_data;

	if (type != BLK_UID_NAA)
		return -EINVAL;

	if (strncmp(dh_data->device_id_str, id, 16) == 0)
		return dh_data->device_id_len;

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(scsi_mpath_unique_id);

int scsi_mpath_unique_lun_id(struct scsi_device *sdev)
{
	struct scsi_mpath_dh_data *dh_data = sdev->mpath_pg_data;
	char device_id_str[20];
	int ret = -EINVAL;

	ret = scsi_vpd_lun_id(sdev, device_id_str, dh_data->device_id_len);
	if (ret < 0)
		return ret;

	if (strncmp(dh_data->device_id_str, device_id_str,
	    dh_data->device_id_len) == 0)
		return -EINVAL;

	return 0;
}

/*
 * Allocate Disk for Multipath Device
 */
int scsi_mpath_alloc_disk(struct scsi_device *sdev)
{
	struct Scsi_Host *shost = sdev->host;
	struct queue_limits lim;

	/*
	 * Don't allocate mpath disk if ALUA handler is not attached
	 */
	if (!sdev->handler || strncmp(sdev->handler->name, "alua", 4) != 0) {
		sdev_printk(KERN_NOTICE, sdev,
		    "No Handler or correct handler attached for multipath \n");
		return 0;
	}

	/*
	 * Add multipath disk only if scsi host supports multipath modparam
	 */
	if (!scsi_multipath) {
		sdev_printk(KERN_NOTICE, sdev,
		    "%s Handler attached but modparam scsi_multipath is set to false \n",
		    sdev->handler->name);
		return 0;
	}

	if (scsi_mpath_unique_lun_id(sdev) == 0) {
		sdev_printk(KERN_NOTICE, sdev,
		    "existing sdev with path, return\n");
		return 0;
	}

	blk_set_stacking_limits(&lim);

	lim.features |= BLK_FEAT_IO_STAT | BLK_FEAT_NOWAIT | BLK_FEAT_POLL;
	lim.max_zone_append_sectors = 0;
	lim.dma_alignment = 3;

	sdev->mpath_disk = blk_alloc_disk(&lim, sdev->mpath_numa_node);
	if (IS_ERR(sdev->mpath_disk))
		return PTR_ERR(sdev->mpath_disk);

	sdev->mpath_disk->private_data = shost;
	sdev->mpath_disk->fops = &scsi_mpath_ops;

	list_add_tail(&shost->mpath_sdev, &sdev->mpath_entry);

	return 0;
}
EXPORT_SYMBOL_GPL(scsi_mpath_alloc_disk);

void scsi_mpath_start_request(struct request *req)
{
	struct scsi_cmnd *cmd = blk_mq_rq_to_pdu(req);
	struct scsi_device *sdev = cmd->device;
	struct Scsi_Host *shost = sdev->host;
	struct scsi_mpath *mpath_dev = shost->mpath_dev;

	if (!blk_queue_io_stat(sdev->mpath_disk->queue) ||
	    blk_rq_is_passthrough(req))
		return;

	req->rq_flags |= SCSI_MPATH_IO_STATS;
	mpath_dev->mpath_start_time = bdev_start_io_acct(sdev->mpath_disk->part0,
	    req_op(req), jiffies);
}

void scsi_mpath_end_request(struct request *req)
{
	struct scsi_cmnd *cmd = blk_mq_rq_to_pdu(req);
	struct scsi_device *sdev = cmd->device;
	struct Scsi_Host *shost = sdev->host;
	struct scsi_mpath *mpath_dev = shost->mpath_dev;

	if (!(req->rq_flags & SCSI_MPATH_IO_STATS))
		return;

	bdev_end_io_acct(sdev->mpath_disk->part0, req_op(req),
	    blk_rq_bytes(req) >> SECTOR_SHIFT,
	    mpath_dev->mpath_start_time);
}

void scsi_mpath_kick_requeue_lists(struct Scsi_Host *shost)
{
	struct scsi_mpath *mpath_dev = shost->mpath_dev;
	struct scsi_device *sdev;
	int srcu_idx;

	srcu_idx = srcu_read_lock(&mpath_dev->srcu);
	list_for_each_entry_rcu(sdev, &shost->mpath_sdev, mpath_entry) {
		if (sdev->is_shared)
			continue;

		kblockd_schedule_work(&mpath_dev->mpath_requeue_work);
		if (sdev->sdev_state == SDEV_RUNNING)
			disk_uevent(sdev->mpath_disk, KOBJ_CHANGE);
	}
	srcu_read_unlock(&mpath_dev->srcu, srcu_idx);
}

void scsi_mpath_shutdown_disk(struct scsi_device *sdev)
{
	struct Scsi_Host *shost = sdev->host;

	if (!sdev->mpath_disk)
		return;

	if (test_and_clear_bit(SCSI_MPATH_DISK_LIVE, &sdev->mpath_flags)) {
		synchronize_srcu(&shost->mpath_dev->srcu);
		kblockd_schedule_work(&shost->mpath_dev->mpath_requeue_work);
		del_gendisk(sdev->mpath_disk);
	}
}
EXPORT_SYMBOL_GPL(scsi_mpath_shutdown_disk);

void scsi_mpath_remove_disk(struct scsi_device *sdev)
{
	struct Scsi_Host *shost = sdev->host;

	if (!sdev->mpath_disk)
		return;

	if (!sdev->is_shared)
		return;

	/* Make sure All pending bio's are cleaned up */
	kblockd_schedule_work(&shost->mpath_dev->mpath_requeue_work);
	flush_work(&shost->mpath_dev->mpath_requeue_work);
	put_disk(sdev->mpath_disk);
}
EXPORT_SYMBOL_GPL(scsi_mpath_remove_disk);

int scsi_mpath_update_state(struct scsi_device *sdev)
{
        struct scsi_mpath_dh_data *mpath_h;

        mpath_h = sdev->mpath_pg_data;
        if (!mpath_h)
		return SCSI_MPATH_UNAVAILABLE;

	switch(mpath_h->state) {
		case SCSI_ACCESS_STATE_OPTIMAL:
			sdev->mpath_state = SCSI_MPATH_OPTIMAL;
			break;
		case SCSI_ACCESS_STATE_ACTIVE:
			sdev->mpath_state = SCSI_MPATH_ACTIVE;
			break;
		case SCSI_ACCESS_STATE_STANDBY:
			sdev->mpath_state = SCSI_MPATH_STANDBY;
			break;
		case SCSI_ACCESS_STATE_UNAVAILABLE:
			sdev->mpath_state = SCSI_MPATH_UNAVAILABLE;
			break;
		case SCSI_ACCESS_STATE_TRANSITIONING:
			sdev->mpath_state = SCSI_MPATH_TRANSITIONING;
			break;
		case SCSI_ACCESS_STATE_OFFLINE:
		default:
                    sdev->mpath_state = SCSI_MPATH_OFFLINE;
		    break;
	}

	return sdev->mpath_state;
}
