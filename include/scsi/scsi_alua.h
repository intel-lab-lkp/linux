// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Generic SCSI-3 ALUA SCSI Device Handler
 *
 * Copyright (C) 2007-2010 Hannes Reinecke, SUSE Linux Products GmbH.
 * All rights reserved.
 */
#ifndef _SCSI_ALUA_H
#define _SCSI_ALUA_H

#include <linux/blk-mq.h>
#include <scsi/scsi.h>
#include <scsi/scsi_device.h>

#if IS_ENABLED(CONFIG_SCSI_ALUA)

struct alua_data {
	int			group_id;
	int			tpgs;
	int			state;
	int			pref;
	int			valid_states;
	bool			rtpg_ext_hdr_unsupp;
	unsigned char		transition_tmo;
	unsigned long		expiry;
	unsigned long		interval;
	struct scsi_device	*sdev;
	spinlock_t		lock;
};

int scsi_alua_sdev_init(struct scsi_device *sdev);
void scsi_alua_sdev_exit(struct scsi_device *sdev);

void scsi_alua_handle_state_transition(struct scsi_device *sdev);

int scsi_alua_check_tpgs(struct scsi_device *sdev);

int scsi_alua_rtpg_run(struct scsi_device *sdev);
int scsi_alua_stpg_run(struct scsi_device *sdev, bool optimize);

blk_status_t scsi_alua_prep_fn(struct scsi_device *sdev, struct request *req);

bool scsi_device_alua_implicit(struct scsi_device *sdev);

int scsi_alua_init(void);
void scsi_exit_alua(void);
#else //CONFIG_SCSI_ALUA

static inline void scsi_alua_handle_state_transition(struct scsi_device *sdev)
{
}
static inline int scsi_alua_check_tpgs(struct scsi_device *sdev)
{
	return 0;
}
static inline int scsi_alua_rtpg_run(struct scsi_device *sdev)
{
	return 0;
}
static inline int scsi_alua_stpg_run(struct scsi_device *sdev, bool optimize)
{
	return 0;
}
static inline
blk_status_t scsi_alua_prep_fn(struct scsi_device *sdev, struct request *req)
{
	return BLK_STS_OK;
}
static inline bool scsi_device_alua_implicit(struct scsi_device *sdev)
{
	return false;
}
static inline int scsi_alua_sdev_init(struct scsi_device *sdev)
{
	return 0;
}
static inline void scsi_alua_sdev_exit(struct scsi_device *sdev)
{

}
static inline int scsi_alua_init(void)
{
	return 0;
}
static inline void scsi_exit_alua(void)
{
}
#endif // CONFIG_SCSI_ALUA
#endif // _SCSI_ALUA_H
