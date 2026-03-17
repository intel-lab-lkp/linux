// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic SCSI-3 ALUA SCSI driver
 *
 * Copyright (C) 2007-2010 Hannes Reinecke, SUSE Linux Products GmbH.
 * All rights reserved.
 */

#include <linux/unaligned.h>

#include <scsi/scsi.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_alua.h>

#define DRV_NAME "alua"

static struct workqueue_struct *kalua_wq;

#define TPGS_SUPPORT_NONE		0x00
#define TPGS_SUPPORT_OPTIMIZED		0x01
#define TPGS_SUPPORT_NONOPTIMIZED	0x02
#define TPGS_SUPPORT_STANDBY		0x04
#define TPGS_SUPPORT_UNAVAILABLE	0x08
#define TPGS_SUPPORT_LBA_DEPENDENT	0x10
#define TPGS_SUPPORT_OFFLINE		0x40
#define TPGS_SUPPORT_TRANSITION		0x80
#define TPGS_SUPPORT_ALL		0xdf

#define RTPG_FMT_MASK			0x70
#define RTPG_FMT_EXT_HDR		0x10
#define TPGS_MODE_NONE			0x0
#define TPGS_MODE_IMPLICIT		0x1
#define TPGS_MODE_EXPLICIT		0x2

#define ALUA_RTPG_SIZE			128
#define ALUA_FAILOVER_TIMEOUT		60
#define ALUA_FAILOVER_RETRIES		5
#define ALUA_RTPG_DELAY_MSECS		5
#define ALUA_RTPG_RETRY_DELAY		2

/*
 * alua_tur - Send a TEST UNIT READY
 * @sdev: device to which the TEST UNIT READY command should be send
 *
 * Send a TEST UNIT READY to @sdev to figure out the device state
 * Returns SCSI_DH_RETRY if the sense code is NOT READY/ALUA TRANSITIONING,
 * SCSI_DH_OK if no error occurred, and SCSI_DH_IO otherwise.
 */
static int scsi_alua_tur(struct scsi_device *sdev)
{
	struct scsi_sense_hdr sense_hdr;
	int retval;

	retval = scsi_test_unit_ready(sdev, ALUA_FAILOVER_TIMEOUT * HZ,
				      ALUA_FAILOVER_RETRIES, &sense_hdr);
	if ((sense_hdr.sense_key == NOT_READY ||
	     sense_hdr.sense_key == UNIT_ATTENTION) &&
	    sense_hdr.asc == 0x04 && sense_hdr.ascq == 0x0a)
		return -EAGAIN;//SCSI_DH_RETRY;
	else if (retval)
		return -EIO;//SCSI_DH_IO;
	else
		return 0;//SCSI_DH_OK;
}

/*
 * submit_rtpg - Issue a REPORT TARGET GROUP STATES command
 * @sdev: sdev the command should be sent to
 */
static int submit_rtpg(struct scsi_device *sdev, unsigned char *buff,
		       int bufflen, struct scsi_sense_hdr *sshdr)
{
	u8 cdb[MAX_COMMAND_SIZE];
	blk_opf_t opf = REQ_OP_DRV_IN | REQ_FAILFAST_DEV |
				REQ_FAILFAST_TRANSPORT | REQ_FAILFAST_DRIVER;
	const struct scsi_exec_args exec_args = {
		.sshdr = sshdr,
	};

	/* Prepare the command. */
	memset(cdb, 0x0, MAX_COMMAND_SIZE);
	cdb[0] = MAINTENANCE_IN;
	if (!sdev->alua->rtpg_ext_hdr_unsupp)
		cdb[1] = MI_REPORT_TARGET_PGS | MI_EXT_HDR_PARAM_FMT;
	else
		cdb[1] = MI_REPORT_TARGET_PGS;
	put_unaligned_be32(bufflen, &cdb[6]);

	return scsi_execute_cmd(sdev, cdb, opf, buff, bufflen,
				ALUA_FAILOVER_TIMEOUT * HZ,
				ALUA_FAILOVER_RETRIES, &exec_args);
}

/*
 * submit_stpg - Issue a SET TARGET PORT GROUP command
 *
 * Currently we're only setting the current target port group state
 * to 'active/optimized' and let the array firmware figure out
 * the states of the remaining groups.
 */
static int submit_stpg(struct scsi_device *sdev,
				struct scsi_sense_hdr *sshdr)
{
	u8 cdb[MAX_COMMAND_SIZE];
	unsigned char stpg_data[8];
	int stpg_len = 8;
	blk_opf_t opf = REQ_OP_DRV_OUT | REQ_FAILFAST_DEV |
				REQ_FAILFAST_TRANSPORT | REQ_FAILFAST_DRIVER;
	const struct scsi_exec_args exec_args = {
		.sshdr = sshdr,
	};

	/* Prepare the data buffer */
	memset(stpg_data, 0, stpg_len);
	stpg_data[4] = SCSI_ACCESS_STATE_OPTIMAL;
	put_unaligned_be16(sdev->alua->group_id, &stpg_data[6]);

	/* Prepare the command. */
	memset(cdb, 0x0, MAX_COMMAND_SIZE);
	cdb[0] = MAINTENANCE_OUT;
	cdb[1] = MO_SET_TARGET_PGS;
	put_unaligned_be32(stpg_len, &cdb[6]);

	return scsi_execute_cmd(sdev, cdb, opf, stpg_data,
				stpg_len, ALUA_FAILOVER_TIMEOUT * HZ,
				ALUA_FAILOVER_RETRIES, &exec_args);
}

/*
 * scsi_alua_check_tpgs - Evaluate TPGS setting
 * @sdev: device to be checked
 *
 * Examine the TPGS setting of the sdev to find out if ALUA
 * is supported.
 */
int scsi_alua_check_tpgs(struct scsi_device *sdev)
{
	int tpgs = TPGS_MODE_NONE;

	/*
	 * ALUA support for non-disk devices is fraught with
	 * difficulties, so disable it for now.
	 */
	if (sdev->type != TYPE_DISK) {
		sdev_printk(KERN_INFO, sdev,
			    "%s: disable for non-disk devices\n",
			    DRV_NAME);
		return tpgs;
	}

	tpgs = scsi_device_tpgs(sdev);
	switch (tpgs) {
	case TPGS_MODE_EXPLICIT|TPGS_MODE_IMPLICIT:
		sdev_printk(KERN_INFO, sdev,
			    "%s: supports implicit and explicit TPGS\n",
			    DRV_NAME);
		break;
	case TPGS_MODE_EXPLICIT:
		sdev_printk(KERN_INFO, sdev, "%s: supports explicit TPGS\n",
			    DRV_NAME);
		break;
	case TPGS_MODE_IMPLICIT:
		sdev_printk(KERN_INFO, sdev, "%s: supports implicit TPGS\n",
			    DRV_NAME);
		break;
	case TPGS_MODE_NONE:
		sdev_printk(KERN_INFO, sdev, "%s: not supported\n",
			    DRV_NAME);
		break;
	default:
		sdev_printk(KERN_INFO, sdev,
			    "%s: unsupported TPGS setting %d\n",
			    DRV_NAME, tpgs);
		tpgs = TPGS_MODE_NONE;
		break;
	}

	return tpgs;
}
EXPORT_SYMBOL_GPL(scsi_alua_check_tpgs);

static char print_alua_state(unsigned char state)
{
	switch (state) {
	case SCSI_ACCESS_STATE_OPTIMAL:
		return 'A';
	case SCSI_ACCESS_STATE_ACTIVE:
		return 'N';
	case SCSI_ACCESS_STATE_STANDBY:
		return 'S';
	case SCSI_ACCESS_STATE_UNAVAILABLE:
		return 'U';
	case SCSI_ACCESS_STATE_LBA:
		return 'L';
	case SCSI_ACCESS_STATE_OFFLINE:
		return 'O';
	case SCSI_ACCESS_STATE_TRANSITIONING:
		return 'T';
	default:
		return 'X';
	}
}

/*
 * scsi_alua_rtpg - Evaluate REPORT TARGET GROUP STATES
 * @sdev: the device to be evaluated.
 *
 * Evaluate the Target Port Group State.
 * Returns -ENODEV if the path is
 * found to be unusable.
 */
static int scsi_alua_rtpg(struct scsi_device *sdev)
{
	struct alua_data *alua = sdev->alua;
	struct scsi_sense_hdr sense_hdr;
	int len, k, off, bufflen = ALUA_RTPG_SIZE;
	int group_id_old, state_old, pref_old, valid_states_old;
	unsigned char *desc, *buff;
	unsigned err;
	int retval;
	unsigned int tpg_desc_tbl_off;
	unsigned char orig_transition_tmo;
	unsigned long flags;
	bool transitioning_sense = false;
	int rel_port, group_id = scsi_vpd_tpg_id(sdev, &rel_port);

	if (group_id < 0) {
		/*
		 * Internal error; TPGS supported but required
		 * VPD identification descriptors not present.
		 * Disable ALUA support
		 */
		sdev_printk(KERN_INFO, sdev,
			    "%s: No target port descriptors found\n",
			    DRV_NAME);
		return -EOPNOTSUPP; //SCSI_DH_DEV_UNSUPP;
	}

	group_id_old = alua->group_id;
	state_old = alua->state;
	pref_old = alua->pref;
	valid_states_old = alua->valid_states;

	if (!alua->expiry) {
		unsigned long transition_tmo = ALUA_FAILOVER_TIMEOUT * HZ;

		if (alua->transition_tmo)
			transition_tmo = alua->transition_tmo * HZ;

		alua->expiry = round_jiffies_up(jiffies + transition_tmo);
	}

	buff = kzalloc(bufflen, GFP_KERNEL);
	if (!buff)
		return -ENOMEM;

 retry:
	err = 0;
	retval = submit_rtpg(sdev, buff, bufflen, &sense_hdr);

	if (retval) {
		/*
		 * Some (broken) implementations have a habit of returning
		 * an error during things like firmware update etc.
		 * But if the target only supports active/optimized there's
		 * not much we can do; it's not that we can switch paths
		 * or anything.
		 * So ignore any errors to avoid spurious failures during
		 * path failover.
		 */
		if ((alua->valid_states & ~TPGS_SUPPORT_OPTIMIZED) == 0) {
			sdev_printk(KERN_INFO, sdev,
				    "%s: ignoring rtpg result %d\n",
				    DRV_NAME, retval);
			kfree(buff);
			return 0;//SCSI_DH_OK
		}
		if (retval < 0 || !scsi_sense_valid(&sense_hdr)) {
			sdev_printk(KERN_INFO, sdev,
				    "%s: rtpg failed, result %d\n",
				    DRV_NAME, retval);
			kfree(buff);
			if (retval < 0)
				return -EBUSY;//SCSI_DH_DEV_TEMP_BUSY;
			if (host_byte(retval) == DID_NO_CONNECT)
				return -ENOENT;//SCSI_DH_RES_TEMP_UNAVAIL;
			return -EIO;//SCSI_DH_IO
		}

		/*
		 * submit_rtpg() has failed on existing arrays
		 * when requesting extended header info, and
		 * the array doesn't support extended headers,
		 * even though it shouldn't according to T10.
		 * The retry without rtpg_ext_hdr_req set
		 * handles this.
		 * Note:  some arrays return a sense key of ILLEGAL_REQUEST
		 * with ASC 00h if they don't support the extended header.
		 */
		if (!alua->rtpg_ext_hdr_unsupp &&
		    sense_hdr.sense_key == ILLEGAL_REQUEST) {
			alua->rtpg_ext_hdr_unsupp = true;
			goto retry;
		}
		/*
		 * If the array returns with 'ALUA state transition'
		 * sense code here it cannot return RTPG data during
		 * transition. So set the state to 'transitioning' directly.
		 */
		if (sense_hdr.sense_key == NOT_READY &&
		    sense_hdr.asc == 0x04 && sense_hdr.ascq == 0x0a) {
			transitioning_sense = true;
			goto skip_rtpg;
		}
		/*
		 * Retry on any other UNIT ATTENTION occurred.
		 */
		if (sense_hdr.sense_key == UNIT_ATTENTION)
			err = -EAGAIN;//SCSI_DH_RETRY
		if (err == -EAGAIN &&
		    alua->expiry != 0 && time_before(jiffies, alua->expiry)) {
			sdev_printk(KERN_ERR, sdev, "%s: rtpg retry\n",
				    DRV_NAME);
			scsi_print_sense_hdr(sdev, DRV_NAME, &sense_hdr);
			kfree(buff);
			return err;
		}
		sdev_printk(KERN_ERR, sdev, "%s: rtpg failed\n",
			    DRV_NAME);
		scsi_print_sense_hdr(sdev, DRV_NAME, &sense_hdr);
		kfree(buff);
		alua->expiry = 0;
		return -EIO;//SCSI_DH_IO
	}

	len = get_unaligned_be32(&buff[0]) + 4;

	if (len > bufflen) {
		/* Resubmit with the correct length */
		kfree(buff);
		bufflen = len;
		buff = kmalloc(bufflen, GFP_KERNEL);
		if (!buff) {
			sdev_printk(KERN_WARNING, sdev,
				    "%s: kmalloc buffer failed\n",__func__);
			/* Temporary failure, bypass */
			alua->expiry = 0;
			return -EBUSY;//SCSI_DH_DEV_TEMP_BUSY;
		}
		goto retry;
	}

	orig_transition_tmo = alua->transition_tmo;
	if ((buff[4] & RTPG_FMT_MASK) == RTPG_FMT_EXT_HDR && buff[5] != 0)
		alua->transition_tmo = buff[5];
	else
		alua->transition_tmo = ALUA_FAILOVER_TIMEOUT;

	if (orig_transition_tmo != alua->transition_tmo) {
		sdev_printk(KERN_INFO, sdev,
			    "%s: transition timeout set to %d seconds\n",
			    DRV_NAME, alua->transition_tmo);
		alua->expiry = jiffies + alua->transition_tmo * HZ;
	}

	if ((buff[4] & RTPG_FMT_MASK) == RTPG_FMT_EXT_HDR)
		tpg_desc_tbl_off = 8;
	else
		tpg_desc_tbl_off = 4;

	for (k = tpg_desc_tbl_off, desc = buff + tpg_desc_tbl_off;
	     k < len;
	     k += off, desc += off) {
		u16 group_id_desc = get_unaligned_be16(&desc[2]);

		spin_lock_irqsave(&alua->lock, flags);
		if (group_id_desc == group_id) {
			alua->group_id = group_id;
			WRITE_ONCE(alua->state, desc[0] & 0x0f);
			alua->pref = desc[0] >> 7;
			WRITE_ONCE(sdev->access_state, desc[0]);
			alua->valid_states = desc[1];
		}
		spin_unlock_irqrestore(&alua->lock, flags);
		off = 8 + (desc[7] * 4);
	}

 skip_rtpg:
	spin_lock_irqsave(&alua->lock, flags);
	if (transitioning_sense)
		alua->state = SCSI_ACCESS_STATE_TRANSITIONING;

	if (group_id_old != alua->group_id || state_old != alua->state ||
		pref_old != alua->pref || valid_states_old != alua->valid_states)
		sdev_printk(KERN_INFO, sdev,
			"%s: port group %02x state %c %s supports %c%c%c%c%c%c%c\n",
			DRV_NAME, alua->group_id, print_alua_state(alua->state),
			alua->pref ? "preferred" : "non-preferred",
			alua->valid_states&TPGS_SUPPORT_TRANSITION?'T':'t',
			alua->valid_states&TPGS_SUPPORT_OFFLINE?'O':'o',
			alua->valid_states&TPGS_SUPPORT_LBA_DEPENDENT?'L':'l',
			alua->valid_states&TPGS_SUPPORT_UNAVAILABLE?'U':'u',
			alua->valid_states&TPGS_SUPPORT_STANDBY?'S':'s',
			alua->valid_states&TPGS_SUPPORT_NONOPTIMIZED?'N':'n',
			alua->valid_states&TPGS_SUPPORT_OPTIMIZED?'A':'a');

	switch (alua->state) {
	case SCSI_ACCESS_STATE_TRANSITIONING:
		if (time_before(jiffies, alua->expiry)) {
			/* State transition, retry */
			alua->interval = ALUA_RTPG_RETRY_DELAY;
			err = -EAGAIN;//SCSI_DH_RETRY
		} else {
			unsigned char access_state;

			/* Transitioning time exceeded, set port to standby */
			err = -EIO;//SCSI_DH_IO;
			alua->state = SCSI_ACCESS_STATE_STANDBY;
			alua->expiry = 0;
			access_state = alua->state & SCSI_ACCESS_STATE_MASK;
			if (alua->pref)
				access_state |= SCSI_ACCESS_STATE_PREFERRED;
			WRITE_ONCE(sdev->access_state, access_state);
		}
		break;
	case SCSI_ACCESS_STATE_OFFLINE:
		/* Path unusable */
		err = -ENODEV;//SCSI_DH_DEV_OFFLINED;
		alua->expiry = 0;
		break;
	default:
		/* Useable path if active */
		err = 0;//SCSI_DH_OK
		alua->expiry = 0;
		break;
	}
	spin_unlock_irqrestore(&alua->lock, flags);
	kfree(buff);
	return err;
}

int scsi_alua_rtpg_run(struct scsi_device *sdev)
{
	struct alua_data *alua = sdev->alua;
	unsigned long flags;
	int state, err;

	spin_lock_irqsave(&alua->lock, flags);
	state = alua->state;
	spin_unlock_irqrestore(&alua->lock, flags);

	if (state == SCSI_ACCESS_STATE_TRANSITIONING) {
		if (scsi_alua_tur(sdev) == -EAGAIN) {
			spin_lock_irqsave(&alua->lock, flags);
			alua->interval = ALUA_RTPG_RETRY_DELAY;
			spin_unlock_irqrestore(&alua->lock, flags);
			return -EAGAIN;
		}
		/* Send RTPG on failure or if TUR indicates SUCCESS */
	}

	err = scsi_alua_rtpg(sdev);
	spin_lock_irqsave(&alua->lock, flags);
	if (err == -EAGAIN) {
		alua->interval = ALUA_RTPG_RETRY_DELAY;
		spin_unlock_irqrestore(&alua->lock, flags);
		return -EAGAIN;
	}
	spin_unlock_irqrestore(&alua->lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(scsi_alua_rtpg_run);

/*
 * scsi_alua_stpg - Issue a SET TARGET PORT GROUP command
 *
 * Issue a SET TARGET PORT GROUP command and evaluate the
 * response. Returns SCSI_DH_RETRY per default to trigger
 * a re-evaluation of the target group state or SCSI_DH_OK
 * if no further action needs to be taken.
 */
static int scsi_alua_stpg(struct scsi_device *sdev, bool optimize)
{
	struct alua_data *alua = sdev->alua;
	int retval;
	struct scsi_sense_hdr sense_hdr;

	if (!(alua->tpgs & TPGS_MODE_EXPLICIT)) {
		/* Only implicit ALUA supported, retry */
		return -EAGAIN;//SCSI_DH_RETRY;
	}
	switch (alua->state) {
	case SCSI_ACCESS_STATE_OPTIMAL:
		return 0;//SCSI_DH_OK;
	case SCSI_ACCESS_STATE_ACTIVE:
		if (optimize &&
		    !alua->pref &&
		    (alua->tpgs & TPGS_MODE_IMPLICIT))
			return 0;//SCSI_DH_OK;
		break;
	case SCSI_ACCESS_STATE_STANDBY:
	case SCSI_ACCESS_STATE_UNAVAILABLE:
		break;
	case SCSI_ACCESS_STATE_OFFLINE:
		return -EIO;//SCSI_DH_IO;
	case SCSI_ACCESS_STATE_TRANSITIONING:
		break;
	default:
		sdev_printk(KERN_INFO, sdev,
			    "%s: stpg failed, unhandled TPGS state %d",
			    DRV_NAME, alua->state);
		return -ENOSYS ;//SCSI_DH_NOSYS;
	}
	retval = submit_stpg(sdev, &sense_hdr);

	if (retval) {
		if (retval < 0 || !scsi_sense_valid(&sense_hdr)) {
			sdev_printk(KERN_INFO, sdev,
				    "%s: stpg failed, result %d",
				    DRV_NAME, retval);
			if (retval < 0)
				return -EBUSY;//SCSI_DH_DEV_TEMP_BUSY;
		} else {
			sdev_printk(KERN_INFO, sdev, "%s: stpg failed\n",
				    DRV_NAME);
			scsi_print_sense_hdr(sdev, DRV_NAME, &sense_hdr);
		}
	}
	/* Retry RTPG */
	return -EAGAIN;//SCSI_DH_RETRY;
}

int scsi_alua_stpg_run(struct scsi_device *sdev, bool optimize)
{
	struct alua_data *alua = sdev->alua;
	unsigned long flags;
	int err;

	err = scsi_alua_stpg(sdev, optimize);
	spin_lock_irqsave(&alua->lock, flags);
	if (err == EAGAIN) {
		alua->interval = 0;
		spin_unlock_irqrestore(&alua->lock, flags);
		return -EAGAIN;
	}
	spin_unlock_irqrestore(&alua->lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(scsi_alua_stpg_run);

int scsi_alua_sdev_init(struct scsi_device *sdev)
{
	int rel_port, ret, tpgs;

	tpgs = scsi_device_tpgs(sdev);
	if (!tpgs)
		return 0;

	sdev->alua = kzalloc(sizeof(*sdev->alua), GFP_KERNEL);
	if (!sdev->alua)
		return -ENOMEM;

	sdev->alua->group_id = scsi_vpd_tpg_id(sdev, &rel_port);
	sdev_printk(KERN_INFO, sdev,
			    "%s: group_id=%d\n",
			    DRV_NAME, sdev->alua->group_id);
	if (sdev->alua->group_id < 0) {
		/*
		 * Internal error; TPGS supported but required
		 * VPD identification descriptors not present.
		 * Disable ALUA support.
		 */
		sdev_printk(KERN_INFO, sdev,
			    "%s: No target port descriptors found\n",
			    __func__);
		ret = -EIO;
		goto out_free_data;
	}

	sdev->alua->sdev = sdev;
	sdev->alua->tpgs = tpgs;
	spin_lock_init(&sdev->alua->lock);

	return 0;
out_free_data:
	kfree(sdev->alua);
	sdev->alua = NULL;
	return ret;
}

void scsi_alua_sdev_exit(struct scsi_device *sdev)
{
	kfree(sdev->alua);
	sdev->alua = NULL;
}

int scsi_alua_init(void)
{
	kalua_wq = alloc_workqueue("kalua", WQ_MEM_RECLAIM | WQ_PERCPU, 0);
	if (!kalua_wq)
		return -ENOMEM;
	return 0;
}

void scsi_exit_alua(void)
{
	destroy_workqueue(kalua_wq);
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("scsi_alua");
