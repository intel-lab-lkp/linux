/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NXP HMS (Heterogeneous Multi-SoC) DSA Switch Configuration
 *
 * Copyright 2025-2026 NXP
 */

#ifndef _HMS_CONFIG_H
#define _HMS_CONFIG_H

#include <linux/types.h>
#include <linux/if_ether.h>

#define HMS_RT1180_DEVICE_ID		0xe001
#define HMS_NUM_PORTS			5
#define HMS_MAX_NUM_PORTS		HMS_NUM_PORTS
#define HMS_NUM_TC			8

#define HMS_ETHTOOL_STATS_NUM_MAX	120

#define HMS_SPI_WORD_BITS		8
#define HMS_SPI_MSG_WORD_BYTES		4
#define HMS_SPI_MSG_HEADER_SIZE		20
#define HMS_SPI_MSG_PARAM_SIZE		16
#define HMS_SPI_MSG_MAXLEN		4096
#define HMS_SPI_MSG_RESPONSE_TIME	1000 /* us */

#define HMS_CMD_DIR_SHIFT		31
#define HMS_CMD_LEN_SHIFT		16

enum hms_spi_rw_mode {
	SPI_READ = 0,
	SPI_WRITE = 1,
};

struct hms_cmd_hdr {
	u32 cmd;
	u8 param[HMS_SPI_MSG_PARAM_SIZE];
};

/* Command */
enum hms_cmd {
	/* Port related commands */
	HMS_CMD_SYS_INFO_GET = 0x1,
	HMS_CMD_PORT_DSA_ADD,
	HMS_CMD_PORT_DSA_DEL,
	HMS_CMD_PORT_MTU_SET,
	HMS_CMD_PORT_MTU_GET,
	HMS_CMD_PORT_PHYLINK_MODE_SET,
	HMS_CMD_PORT_PHYLINK_STATUS_GET,
	HMS_CMD_PORT_ETHTOOL_STATS_GET,
	HMS_CMD_PORT_PVID_SET,
	HMS_CMD_PORT_LINK_SET,
	HMS_CMD_PORT_DROPUNTAG_SET,

	/* FDB/VLAN commands */
	HMS_CMD_FDB_ADD = 0x1000,
	HMS_CMD_FDB_DEL,
	HMS_CMD_FDB_DUMP,
	HMS_CMD_VLAN_ADD,
	HMS_CMD_VLAN_DEL,
	HMS_CMD_VLAN_DUMP,

	HMS_CMD_MAX_NUM,
};

struct hms_cmd_sysinfo {
	u16 device_id;
	u16 vendor_id;
	u8  version_major;
	u8  version_minor;
	u8  version_revision;
	u8  cpu_port;
};

/* command data for HMS_CMD_PORT_DSA_ADD */
struct hms_cmd_port_dsa_add {
	u8 cpu_port;
	u8 user_port;
	u8 mac_addr[ETH_ALEN];
};

/* command data for HMS_CMD_PORT_DSA_DEL */
struct hms_cmd_port_dsa_del {
	u8 user_port;
	u8 reserved[3];
};

/* command data for HMS_CMD_PORT_MTU_SET */
struct hms_cmd_port_mtu {
	u8 port;
	u8 reserved;
	u16 mtu;
};

/* command data for HMS_CMD_PORT_PHYLINK_MODE_SET */
struct hms_cmd_port_phylink_mode {
	u8 port;
	bool duplex;
	u16 speed;
};

/* command data for HMS_CMD_PORT_PVID_SET */
struct hms_cmd_port_pvid {
	u8 port;
	u8 reserved;
	u16 pvid;
};

/* command data for hms_cmd_port_link */
struct hms_cmd_port_link {
	u8 port;
	bool link;
	u8 reserved[2];
};

/* command data for hms_cmd_port_dropuntag */
struct hms_cmd_port_dropuntag {
	u8 port;
	u8 reserved;
	u16 drop;
};

/* command data for HMS_CMD_FDB_ADD */
struct hms_cmd_fdb {
	u8 mac_addr[ETH_ALEN];
	u16 vid;
	u8 port;
	u8 reserved[3];
};

/* command data for HMS_CMD_VLAN_ADD */
struct hms_cmd_vlan {
	u16 vid;
	u8 port;
	bool untagged;
};

/* data returned for HMS_CMD_PORT_PHYLINK_STATUS_GET */
struct hms_cmd_port_phylink_status {
	u8 port;
	bool link;
	u16 speed;
	bool duplex;
	u8 reserved[3];
};

/* command param */
struct hms_cmd_read_param {
	u32 id;
};

/* data returned for HMS_CMD_FDB_DUMP */
struct hms_cmd_fdb_dump {
	u8 mac_addr[ETH_ALEN];
	u16 vid;
	u32 port_map;
	bool dynamic;
	u8 reserved[3];
	u32 resume_entry_id;
};

/* data returned for HMS_CMD_VLAN_DUMP */
struct hms_cmd_vlan_dump {
	u16 vid;
	bool untagged;
	u8 reserved;
	u32 port_map;
	u32 resume_entry_id;
};

struct hms_cmd_port_ethtool_stats {
	u64 values[HMS_ETHTOOL_STATS_NUM_MAX];
};

struct hms_mac_config {
	u8 port;
	u16 speed;
	u16 vlanid;
	bool link;
	bool egress;
	bool ingress;
	bool duplex;
	bool drptag;
	bool drpuntag;
	bool retag;
};

struct hms_fdb_entry {
	u8 mac_addr[ETH_ALEN];
	u16 vid;
	u32 port_map;
	bool dynamic;
};

struct hms_vlan_entry {
	u16 vid;
	u16 port;
	u32 port_map;
	u32 tag_ports;
	u32 entry_id;
};

struct hms_config {
	u16 device_id;
	u16 vendor_id;
	u8  version_major;
	u8  version_minor;
	u8  version_revision;
	u8  cpu_port_mode;
	u16 tpid;
	u16 tpid2;
	struct hms_mac_config mac[HMS_MAX_NUM_PORTS];
	int cpu_port;
	int vlan_count;
	int vlan_max_count;
	struct hms_vlan_entry *vlan;
};

struct hms_private;

int hms_get_devinfo(struct hms_private *priv, struct hms_config *config);

int hms_port_phylink_mode_set(struct hms_private *priv,
			      struct hms_mac_config *mac);
int hms_port_phylink_status_get(struct hms_private *priv,
				struct hms_mac_config *mac);
int hms_port_pvid_set(struct hms_private *priv, int port, u16 pvid);
int hms_port_link_set(struct hms_private *priv, int port, bool up);
int hms_port_dropuntag_set(struct hms_private *priv, int port, bool drop);

int hms_port_mtu_set(struct hms_private *priv, int port, int mtu);
int hms_port_mtu_get(struct hms_private *priv, int port, int *mtu);

int hms_port_dsa_add(struct hms_private *priv, int cpu_port,
		     int user_port, const unsigned char *mac_addr);
int hms_port_dsa_del(struct hms_private *priv, int user_port);

int hms_fdb_entry_add(struct hms_private *priv,
		      const unsigned char *mac_addr,
		      u16 vid, int port);
int hms_fdb_entry_del(struct hms_private *priv,
		      const unsigned char *mac_addr,
		      u16 vid, int port);
int hms_fdb_entry_get(struct hms_private *priv,
		      struct hms_fdb_entry *fdb,
		      u32 entry_id, u32 *next_id);

int hms_vlan_entry_add(struct hms_private *priv,
		       u16 vid, int port, bool untagged);
int hms_vlan_entry_del(struct hms_private *priv, u16 vid, int port);
int hms_vlan_entry_read(struct hms_private *priv,
			struct hms_vlan_entry *vlan,
			u32 entry_id, u32 *next_id);

int hms_config_setup(struct hms_config *config);
void hms_config_free(struct hms_config *config);

#endif /* _HMS_CONFIG_H */
