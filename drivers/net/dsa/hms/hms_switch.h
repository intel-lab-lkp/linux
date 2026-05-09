/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NXP HMS (Heterogeneous Multi-SoC) DSA Switch Driver
 *
 * Copyright 2025-2026 NXP
 */

#ifndef _HMS_SWITCH_H
#define _HMS_SWITCH_H

#include <linux/dsa/8021q.h>
#include <linux/dsa/tag_hms.h>
#include <linux/mutex.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/timecounter.h>
#include <net/dsa.h>

#include "hms_config.h"

#define HMS_DEFAULT_VLAN	1

struct hms_private;

struct hms_info {
	const char *name;
	int device_id;
	int num_ports;
	enum dsa_tag_protocol tag_proto;
};

struct hms_private {
	const struct hms_info *info;
	struct hms_config config;
	phy_interface_t phy_mode[HMS_MAX_NUM_PORTS];
	bool fixed_link[HMS_MAX_NUM_PORTS];

	size_t max_xfer_len;
	struct spi_device *spidev;
	struct dsa_switch *ds;
	u16 bridge_pvid[HMS_MAX_NUM_PORTS];
	u16 tag_8021q_pvid[HMS_MAX_NUM_PORTS];

	/* Protects SPI command transactions */
	struct mutex spi_lock;
	/* Serializes accesses to the FDB */
	struct mutex fdb_lock;
};

int hms_is_vlan_configured(struct hms_private *priv, u16 vid);

int hms_vlan_filtering(struct dsa_switch *ds, int port, bool enabled,
		       struct netlink_ext_ack *extack);

/* From hms_spi.c */
int hms_xfer_cmd(struct hms_private *priv,
		 enum hms_spi_rw_mode rw, enum hms_cmd cmd,
		 void *param, size_t param_len,
		 void *resp, size_t resp_len,
		 struct ptp_system_timestamp *ptp_sts);
int hms_xfer_set_cmd(struct hms_private *priv,
		     enum hms_cmd cmd,
		     void *param, size_t param_len);
int hms_xfer_get_cmd(struct hms_private *priv,
		     enum hms_cmd cmd, u32 id,
		     void *resp, size_t resp_len);

#endif /* _HMS_SWITCH_H */
