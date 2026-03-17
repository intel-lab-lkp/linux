// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Generic SCSI-3 ALUA SCSI Device Handler
 *
 * Copyright (C) 2007-2010 Hannes Reinecke, SUSE Linux Products GmbH.
 * All rights reserved.
 */
#ifndef _SCSI_ALUA_H
#define _SCSI_ALUA_H

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

int scsi_alua_rtpg_run(struct scsi_device *sdev);

int scsi_alua_init(void);
void scsi_exit_alua(void);
#else //CONFIG_SCSI_ALUA

static inline int scsi_alua_rtpg_run(struct scsi_device *sdev)
{
	return 0;
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
