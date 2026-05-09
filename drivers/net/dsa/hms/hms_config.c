// SPDX-License-Identifier: GPL-2.0
/*
 * NXP HMS (Heterogeneous Multi-SoC) DSA Switch Configuration
 *
 * Copyright 2025-2026 NXP
 */

#include <linux/slab.h>
#include "hms_switch.h"

int hms_get_devinfo(struct hms_private *priv, struct hms_config *config)
{
	struct hms_cmd_sysinfo sysinfo;
	int rc;

	rc = hms_xfer_get_cmd(priv, HMS_CMD_SYS_INFO_GET, 0,
			      &sysinfo, sizeof(sysinfo));
	if (rc)
		return rc;

	config->device_id = sysinfo.device_id;
	config->vendor_id = sysinfo.vendor_id;
	config->version_major = sysinfo.version_major;
	config->version_minor = sysinfo.version_minor;
	config->version_revision = sysinfo.version_revision;
	config->cpu_port = sysinfo.cpu_port;

	return 0;
}

int hms_port_mtu_set(struct hms_private *priv, int port, int mtu)
{
	struct hms_cmd_port_mtu cmd;

	cmd.port = port;
	cmd.mtu = mtu;

	return hms_xfer_set_cmd(priv, HMS_CMD_PORT_MTU_SET,
				&cmd, sizeof(cmd));
}

int hms_port_mtu_get(struct hms_private *priv, int port, int *mtu)
{
	struct hms_cmd_port_mtu cmd;
	int rc;

	rc = hms_xfer_get_cmd(priv, HMS_CMD_PORT_MTU_GET, port,
			      &cmd, sizeof(cmd));
	if (rc)
		return rc;

	*mtu = cmd.mtu;

	return 0;
}

int hms_port_phylink_mode_set(struct hms_private *priv,
			      struct hms_mac_config *mac)
{
	struct hms_cmd_port_phylink_mode cmd;

	cmd.port = mac->port;
	cmd.duplex = mac->duplex;
	cmd.speed = mac->speed;

	return hms_xfer_set_cmd(priv, HMS_CMD_PORT_PHYLINK_MODE_SET,
				&cmd, sizeof(cmd));
}

int hms_port_phylink_status_get(struct hms_private *priv,
				struct hms_mac_config *mac)
{
	struct hms_cmd_port_phylink_status status;
	int rc;

	rc = hms_xfer_get_cmd(priv, HMS_CMD_PORT_PHYLINK_STATUS_GET, mac->port,
			      &status, sizeof(status));
	if (rc)
		return rc;

	mac->link = status.link;
	mac->speed = status.speed;
	mac->duplex = status.duplex;

	return 0;
}

int hms_port_pvid_set(struct hms_private *priv, int port, u16 pvid)
{
	struct hms_cmd_port_pvid cmd;

	cmd.port = port;
	cmd.pvid = pvid;

	return hms_xfer_set_cmd(priv, HMS_CMD_PORT_PVID_SET,
				&cmd, sizeof(cmd));
}

int hms_port_link_set(struct hms_private *priv, int port, bool up)
{
	struct hms_cmd_port_link cmd;

	cmd.port = port;
	cmd.link = up;

	return hms_xfer_set_cmd(priv, HMS_CMD_PORT_LINK_SET,
				&cmd, sizeof(cmd));
}

int hms_port_dropuntag_set(struct hms_private *priv, int port, bool drop)
{
	struct hms_cmd_port_dropuntag cmd;

	cmd.port = port;
	cmd.drop = drop;

	return hms_xfer_set_cmd(priv, HMS_CMD_PORT_DROPUNTAG_SET,
				&cmd, sizeof(cmd));
}

int hms_port_dsa_add(struct hms_private *priv, int cpu_port,
		     int user_port, const unsigned char *mac_addr)
{
	struct hms_cmd_port_dsa_add cmd;

	cmd.cpu_port = cpu_port;
	cmd.user_port = user_port;
	memcpy(cmd.mac_addr, mac_addr, ETH_ALEN);

	return hms_xfer_set_cmd(priv, HMS_CMD_PORT_DSA_ADD,
				&cmd, sizeof(cmd));
}

int hms_port_dsa_del(struct hms_private *priv, int user_port)
{
	struct hms_cmd_port_dsa_del cmd;

	cmd.user_port = user_port;

	return hms_xfer_set_cmd(priv, HMS_CMD_PORT_DSA_DEL,
				&cmd, sizeof(cmd));
}

int hms_vlan_entry_add(struct hms_private *priv,
		       u16 vid, int port, bool untagged)
{
	struct hms_cmd_vlan cmd;

	cmd.vid = vid;
	cmd.port = port;
	cmd.untagged = untagged;

	return hms_xfer_set_cmd(priv, HMS_CMD_VLAN_ADD,
				&cmd, sizeof(cmd));
}

int hms_vlan_entry_del(struct hms_private *priv, u16 vid, int port)
{
	struct hms_cmd_vlan cmd;

	cmd.vid = vid;
	cmd.port = port;

	return hms_xfer_set_cmd(priv, HMS_CMD_VLAN_DEL,
				&cmd, sizeof(cmd));
}

int hms_vlan_entry_read(struct hms_private *priv,
			struct hms_vlan_entry *vlan,
			u32 entry_id, u32 *next_id)
{
	struct hms_cmd_vlan_dump resp;
	int rc;

	rc = hms_xfer_get_cmd(priv, HMS_CMD_VLAN_DUMP, entry_id,
			      &resp, sizeof(resp));
	if (rc)
		return rc;

	vlan->vid = resp.vid;
	vlan->port_map = resp.port_map;
	*next_id = resp.resume_entry_id;

	return 0;
}

int hms_fdb_entry_add(struct hms_private *priv,
		      const unsigned char *mac_addr,
		      u16 vid, int port)
{
	struct hms_cmd_fdb cmd;

	memcpy(cmd.mac_addr, mac_addr, ETH_ALEN);
	cmd.vid = vid;
	cmd.port = port;

	return hms_xfer_set_cmd(priv, HMS_CMD_FDB_ADD,
				&cmd, sizeof(cmd));
}

int hms_fdb_entry_del(struct hms_private *priv,
		      const unsigned char *mac_addr,
		      u16 vid, int port)
{
	struct hms_cmd_fdb cmd;

	memcpy(cmd.mac_addr, mac_addr, ETH_ALEN);
	cmd.vid = vid;
	cmd.port = port;

	return hms_xfer_set_cmd(priv, HMS_CMD_FDB_DEL,
				&cmd, sizeof(cmd));
}

int hms_fdb_entry_get(struct hms_private *priv, struct hms_fdb_entry *fdb,
		      u32 entry_id, u32 *next_id)
{
	struct hms_cmd_fdb_dump resp;
	int rc;

	rc = hms_xfer_get_cmd(priv, HMS_CMD_FDB_DUMP, entry_id,
			      &resp, sizeof(resp));
	if (rc)
		return rc;

	memcpy(fdb->mac_addr, resp.mac_addr, ETH_ALEN);
	fdb->vid = resp.vid;
	fdb->port_map = resp.port_map;
	fdb->dynamic = resp.dynamic;
	*next_id = resp.resume_entry_id;

	return 0;
}

int hms_config_setup(struct hms_config *config)
{
	config->vlan_max_count = 256;
	config->vlan = kcalloc(config->vlan_max_count,
			       sizeof(struct hms_vlan_entry),
			       GFP_KERNEL);
	if (!config->vlan)
		return -ENOMEM;

	config->vlan_count = 0;

	return 0;
}

void hms_config_free(struct hms_config *config)
{
	kfree(config->vlan);
	config->vlan = NULL;
}
