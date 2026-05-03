// SPDX-License-Identifier: GPL-2.0
/* Forwarding and multicast database interface for the rtl8365mb switch family
 *
 * Copyright (C) 2022 Alvin Šipraga <alsi@bang-olufsen.dk>
 */

#include <linux/etherdevice.h>

#include "rtl8365mb_l2.h"
#include "rtl8365mb_table.h"
#include <linux/regmap.h>

#define RTL8365MB_L2_ENTRY_SIZE			6

#define RTL8365MB_L2_UC_D0_MAC5_MASK		GENMASK(7, 0)
#define RTL8365MB_L2_UC_D0_MAC4_MASK		GENMASK(15, 8)
#define RTL8365MB_L2_UC_D1_MAC3_MASK		GENMASK(7, 0)
#define RTL8365MB_L2_UC_D1_MAC2_MASK		GENMASK(15, 8)
#define RTL8365MB_L2_UC_D2_MAC1_MASK		GENMASK(7, 0)
#define RTL8365MB_L2_UC_D2_MAC0_MASK		GENMASK(15, 8)
#define RTL8365MB_L2_UC_D3_VID_MASK		GENMASK(11, 0)
#define RTL8365MB_L2_UC_D3_IVL_MASK		GENMASK(13, 13)
#define RTL8365MB_L2_UC_D3_PORT_EXT_MASK	GENMASK(15, 15)
#define RTL8365MB_L2_UC_D4_EFID_MASK		GENMASK(2, 0)
#define RTL8365MB_L2_UC_D4_FID_MASK		GENMASK(6, 3)
#define RTL8365MB_L2_UC_D4_SA_PRI_MASK		GENMASK(7, 7)
#define RTL8365MB_L2_UC_D4_PORT_MASK		GENMASK(10, 8)
#define RTL8365MB_L2_UC_D4_AGE_MASK		GENMASK(13, 11)
#define RTL8365MB_L2_UC_D4_AUTH_MASK		GENMASK(14, 14)
#define RTL8365MB_L2_UC_D4_SA_BLOCK_MASK	GENMASK(15, 15)
#define RTL8365MB_L2_UC_D5_DA_BLOCK_MASK	GENMASK(0, 0)
#define RTL8365MB_L2_UC_D5_PRIORITY_MASK	GENMASK(3, 1)
#define RTL8365MB_L2_UC_D5_FWD_PRI_MASK		GENMASK(4, 4)
#define RTL8365MB_L2_UC_D5_STATIC_MASK		GENMASK(5, 5)

#define RTL8365MB_L2_MC_MAC5_MASK		GENMASK(7, 0)   /* D0 */
#define RTL8365MB_L2_MC_MAC4_MASK		GENMASK(15, 8)  /* D0 */
#define RTL8365MB_L2_MC_MAC3_MASK		GENMASK(7, 0)   /* D1 */
#define RTL8365MB_L2_MC_MAC2_MASK		GENMASK(15, 8)  /* D1 */
#define RTL8365MB_L2_MC_MAC1_MASK		GENMASK(7, 0)   /* D2 */
#define RTL8365MB_L2_MC_MAC0_MASK		GENMASK(15, 8)  /* D2 */
#define RTL8365MB_L2_MC_VID_MASK		GENMASK(11, 0)  /* D3 */
#define RTL8365MB_L2_MC_IVL_MASK		GENMASK(13, 13) /* D3 */
#define RTL8365MB_L2_MC_MBR_EXT1_MASK		GENMASK(15, 14) /* D3 */

#define RTL8365MB_L2_MC_MBR_MASK		GENMASK(7, 0)   /* D4 */
#define RTL8365MB_L2_MC_IGMPIDX_MASK		GENMASK(15, 8)  /* D4 */

#define RTL8365MB_L2_MC_IGMP_ASIC_MASK		GENMASK(0, 0)   /* D5 */
#define RTL8365MB_L2_MC_PRIORITY_MASK		GENMASK(3, 1)   /* D5 */
#define RTL8365MB_L2_MC_FWD_PRI_MASK		GENMASK(4, 4)   /* D5 */
#define RTL8365MB_L2_MC_STATIC_MASK		GENMASK(5, 5)   /* D5 */
#define RTL8365MB_L2_MC_MBR_EXT2_MASK		GENMASK(7, 7)   /* D5 */

/* Port flush command registers - writing a 1 to the port's MASK bit will
 * initiate the flush procedure. Completion is signalled when the corresponding
 * BUSY bit is 0.
 */
#define RTL8365MB_L2_FLUSH_PORT_REG		0x0A36
#define   RTL8365MB_L2_FLUSH_PORT_MASK_MASK	GENMASK(7, 0)
#define   RTL8365MB_L2_FLUSH_PORT_BUSY_MASK	GENMASK(15, 8)

#define RTL8365MB_L2_FLUSH_PORT_EXT_REG		0x0A35
#define   RTL8365MB_L2_FLUSH_PORT_EXT_MASK_MASK	GENMASK(2, 0)
#define   RTL8365MB_L2_FLUSH_PORT_EXT_BUSY_MASK	GENMASK(5, 3)

#define RTL8365MB_L2_FLUSH_CTRL1_REG		0x0A37
#define   RTL8365MB_L2_FLUSH_CTRL1_VID_MASK	GENMASK(11, 0)
#define   RTL8365MB_L2_FLUSH_CTRL1_FID_MASK	GENMASK(15, 12)

#define RTL8365MB_L2_FLUSH_CTRL2_REG		0x0A38
#define   RTL8365MB_L2_FLUSH_CTRL2_MODE_MASK	GENMASK(1, 0)
#define   RTL8365MB_L2_FLUSH_CTRL2_MODE_PORT	0
#define   RTL8365MB_L2_FLUSH_CTRL2_MODE_PORT_VID 1
#define   RTL8365MB_L2_FLUSH_CTRL2_MODE_PORT_FID 2
#define   RTL8365MB_L2_FLUSH_CTRL2_TYPE_MASK	GENMASK(2, 2)
#define   RTL8365MB_L2_FLUSH_CTRL2_TYPE_DYNAMIC	0
#define   RTL8365MB_L2_FLUSH_CTRL2_TYPE_BOTH	0

/* This flushes the entire LUT, reading it back it will turn 0 when the
 * operation is complete
 */
#define RTL8365MB_L2_FLUSH_CTRL3_REG		0x0A39
#define   RTL8365MB_L2_FLUSH_CTRL3_MASK		GENMASK(0, 0)

struct rtl8365mb_l2_uc_key {
	u8 mac_addr[ETH_ALEN];
	union {
		u16 vid; /* IVL */
		u16 fid; /* SVL */
	};
	bool ivl;
	u16 efid;
};

struct rtl8365mb_l2_uc {
	struct rtl8365mb_l2_uc_key key;
	u8 port;
	u8 age;
	u8 priority;

	bool sa_block;
	bool da_block;
	bool auth;
	bool is_static;
	bool sa_pri;
	bool fwd_pri;
};

struct rtl8365mb_l2_mc_key {
	u8 mac_addr[ETH_ALEN];
	union {
		u16 vid; /* IVL */
		u16 fid; /* SVL */
	};
	bool ivl;
};

struct rtl8365mb_l2_mc {
	struct rtl8365mb_l2_mc_key key;
	u16 member;
	u8 priority;
	u8 igmpidx;

	bool is_static;
	bool fwd_pri;
	bool igmp_asic;
};

static void rtl8365mb_l2_data_to_uc(const u16 *data, struct rtl8365mb_l2_uc *uc)
{
	uc->key.mac_addr[5] = FIELD_GET(RTL8365MB_L2_UC_D0_MAC5_MASK, data[0]);
	uc->key.mac_addr[4] = FIELD_GET(RTL8365MB_L2_UC_D0_MAC4_MASK, data[0]);
	uc->key.mac_addr[3] = FIELD_GET(RTL8365MB_L2_UC_D1_MAC3_MASK, data[1]);
	uc->key.mac_addr[2] = FIELD_GET(RTL8365MB_L2_UC_D1_MAC2_MASK, data[1]);
	uc->key.mac_addr[1] = FIELD_GET(RTL8365MB_L2_UC_D2_MAC1_MASK, data[2]);
	uc->key.mac_addr[0] = FIELD_GET(RTL8365MB_L2_UC_D2_MAC0_MASK, data[2]);
	uc->key.efid = FIELD_GET(RTL8365MB_L2_UC_D4_EFID_MASK, data[4]);
	uc->key.vid = FIELD_GET(RTL8365MB_L2_UC_D3_VID_MASK, data[3]);
	uc->key.ivl = FIELD_GET(RTL8365MB_L2_UC_D3_IVL_MASK, data[3]);
	uc->key.fid = FIELD_GET(RTL8365MB_L2_UC_D4_FID_MASK, data[4]);
	uc->age = FIELD_GET(RTL8365MB_L2_UC_D4_AGE_MASK, data[4]);
	uc->auth = FIELD_GET(RTL8365MB_L2_UC_D4_AUTH_MASK, data[4]);
	uc->port = FIELD_GET(RTL8365MB_L2_UC_D4_PORT_MASK, data[4]) |
		   (FIELD_GET(RTL8365MB_L2_UC_D3_PORT_EXT_MASK, data[3]) << 3);
	uc->sa_pri = FIELD_GET(RTL8365MB_L2_UC_D4_SA_PRI_MASK, data[4]);
	uc->fwd_pri = FIELD_GET(RTL8365MB_L2_UC_D5_FWD_PRI_MASK, data[5]);
	uc->sa_block = FIELD_GET(RTL8365MB_L2_UC_D4_SA_BLOCK_MASK, data[4]);
	uc->da_block = FIELD_GET(RTL8365MB_L2_UC_D5_DA_BLOCK_MASK, data[5]);
	uc->priority = FIELD_GET(RTL8365MB_L2_UC_D5_PRIORITY_MASK, data[5]);
	uc->is_static = FIELD_GET(RTL8365MB_L2_UC_D5_STATIC_MASK, data[5]);
}

static void rtl8365mb_l2_uc_to_data(const struct rtl8365mb_l2_uc *uc, u16 *data)
{
	memset(data, 0, RTL8365MB_L2_ENTRY_SIZE * 2);
	data[0] |=
		FIELD_PREP(RTL8365MB_L2_UC_D0_MAC5_MASK, uc->key.mac_addr[5]);
	data[0] |=
		FIELD_PREP(RTL8365MB_L2_UC_D0_MAC4_MASK, uc->key.mac_addr[4]);
	data[1] |=
		FIELD_PREP(RTL8365MB_L2_UC_D1_MAC3_MASK, uc->key.mac_addr[3]);
	data[1] |=
		FIELD_PREP(RTL8365MB_L2_UC_D1_MAC2_MASK, uc->key.mac_addr[2]);
	data[2] |=
		FIELD_PREP(RTL8365MB_L2_UC_D2_MAC1_MASK, uc->key.mac_addr[1]);
	data[2] |=
		FIELD_PREP(RTL8365MB_L2_UC_D2_MAC0_MASK, uc->key.mac_addr[0]);
	data[3] |= FIELD_PREP(RTL8365MB_L2_UC_D3_VID_MASK, uc->key.vid);
	data[3] |= FIELD_PREP(RTL8365MB_L2_UC_D3_IVL_MASK, uc->key.ivl);
	data[3] |= FIELD_PREP(RTL8365MB_L2_UC_D3_PORT_EXT_MASK, uc->port >> 3);
	data[4] |= FIELD_PREP(RTL8365MB_L2_UC_D4_FID_MASK, uc->key.fid);
	data[4] |= FIELD_PREP(RTL8365MB_L2_UC_D4_EFID_MASK, uc->key.efid);
	data[4] |= FIELD_PREP(RTL8365MB_L2_UC_D4_AGE_MASK, uc->age);
	data[4] |= FIELD_PREP(RTL8365MB_L2_UC_D4_AUTH_MASK, uc->auth);
	data[4] |= FIELD_PREP(RTL8365MB_L2_UC_D4_PORT_MASK, uc->port);
	data[4] |= FIELD_PREP(RTL8365MB_L2_UC_D4_SA_PRI_MASK, uc->sa_pri);
	data[4] |= FIELD_PREP(RTL8365MB_L2_UC_D4_SA_BLOCK_MASK, uc->sa_block);
	data[5] |= FIELD_PREP(RTL8365MB_L2_UC_D5_FWD_PRI_MASK, uc->fwd_pri);
	data[5] |= FIELD_PREP(RTL8365MB_L2_UC_D5_DA_BLOCK_MASK, uc->da_block);
	data[5] |= FIELD_PREP(RTL8365MB_L2_UC_D5_PRIORITY_MASK, uc->priority);
	data[5] |= FIELD_PREP(RTL8365MB_L2_UC_D5_STATIC_MASK, uc->is_static);
}

static void rtl8365mb_l2_data_to_mc(const u16 *data, struct rtl8365mb_l2_mc *mc)
{
	mc->key.mac_addr[5] = FIELD_GET(RTL8365MB_L2_MC_MAC5_MASK, data[0]);
	mc->key.mac_addr[4] = FIELD_GET(RTL8365MB_L2_MC_MAC4_MASK, data[0]);
	mc->key.mac_addr[3] = FIELD_GET(RTL8365MB_L2_MC_MAC3_MASK, data[1]);
	mc->key.mac_addr[2] = FIELD_GET(RTL8365MB_L2_MC_MAC2_MASK, data[1]);
	mc->key.mac_addr[1] = FIELD_GET(RTL8365MB_L2_MC_MAC1_MASK, data[2]);
	mc->key.mac_addr[0] = FIELD_GET(RTL8365MB_L2_MC_MAC0_MASK, data[2]);
	mc->key.vid = FIELD_GET(RTL8365MB_L2_MC_VID_MASK, data[3]);
	mc->key.ivl = FIELD_GET(RTL8365MB_L2_MC_IVL_MASK, data[3]);
	mc->priority = FIELD_GET(RTL8365MB_L2_MC_PRIORITY_MASK, data[5]);
	mc->fwd_pri = FIELD_GET(RTL8365MB_L2_MC_FWD_PRI_MASK, data[5]);
	mc->is_static = FIELD_GET(RTL8365MB_L2_MC_STATIC_MASK, data[5]);
	mc->member = FIELD_GET(RTL8365MB_L2_MC_MBR_MASK, data[4]) |
		     (FIELD_GET(RTL8365MB_L2_MC_MBR_EXT1_MASK, data[3]) << 8) |
		     (FIELD_GET(RTL8365MB_L2_MC_MBR_EXT2_MASK, data[5]) << 8);
	mc->igmpidx = FIELD_GET(RTL8365MB_L2_MC_IGMPIDX_MASK, data[4]);
	mc->igmp_asic = FIELD_GET(RTL8365MB_L2_MC_IGMP_ASIC_MASK, data[5]);
}

static void rtl8365mb_l2_mc_to_data(const struct rtl8365mb_l2_mc *mc, u16 *data)
{
	memset(data, 0, 12);
	data[0] |= FIELD_PREP(RTL8365MB_L2_MC_MAC5_MASK, mc->key.mac_addr[5]);
	data[0] |= FIELD_PREP(RTL8365MB_L2_MC_MAC4_MASK, mc->key.mac_addr[4]);
	data[1] |= FIELD_PREP(RTL8365MB_L2_MC_MAC3_MASK, mc->key.mac_addr[3]);
	data[1] |= FIELD_PREP(RTL8365MB_L2_MC_MAC2_MASK, mc->key.mac_addr[2]);
	data[2] |= FIELD_PREP(RTL8365MB_L2_MC_MAC1_MASK, mc->key.mac_addr[1]);
	data[2] |= FIELD_PREP(RTL8365MB_L2_MC_MAC0_MASK, mc->key.mac_addr[0]);
	data[3] |= FIELD_PREP(RTL8365MB_L2_MC_VID_MASK, mc->key.vid);
	data[3] |= FIELD_PREP(RTL8365MB_L2_MC_IVL_MASK, mc->key.ivl);
	data[3] |= FIELD_PREP(RTL8365MB_L2_MC_MBR_EXT1_MASK, mc->member >> 8);
	data[4] |= FIELD_PREP(RTL8365MB_L2_MC_MBR_MASK, mc->member);
	data[4] |= FIELD_PREP(RTL8365MB_L2_MC_IGMPIDX_MASK, mc->igmpidx);
	data[5] |= FIELD_PREP(RTL8365MB_L2_MC_IGMP_ASIC_MASK, mc->igmp_asic);
	data[5] |= FIELD_PREP(RTL8365MB_L2_MC_PRIORITY_MASK, mc->priority);
	data[5] |= FIELD_PREP(RTL8365MB_L2_MC_FWD_PRI_MASK, mc->fwd_pri);
	data[5] |= FIELD_PREP(RTL8365MB_L2_MC_STATIC_MASK, mc->is_static);
	data[5] |= FIELD_PREP(RTL8365MB_L2_MC_MBR_EXT2_MASK, mc->member >> 10);
}

/**
 * rtl8365mb_l2_get_next_uc() - get the next Unicast L2 entry
 *
 * @priv: realtek_priv pointer
 * @addr: as input, the table index to start the walk
 *        as output, the found table index
 * @port: restrict the walk on entries related to port
 * @uc: returned L2 Unicast table entry
 *
 * This function get the next unicast L2 table entry starting from @addr
 * and checking exclusively entries related to @port. If no more entries
 * were found, the output @addr will be lower than the input @addr and @uc
 * will not be overwritten.
 *
 * Return: Returns 0 on success, a negative error on failure.
 **/
int rtl8365mb_l2_get_next_uc(struct realtek_priv *priv, u16 *addr, int port,
			     struct realtek_fdb_entry *entry)
{
	u16 data[RTL8365MB_L2_ENTRY_SIZE] = { 0 };
	struct rtl8365mb_l2_uc uc;
	int ret;

	ret = rtl8365mb_table_query(priv, RTL8365MB_TABLE_L2,
				    RTL8365MB_TABLE_OP_READ, addr,
				    RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT_UC_PORT,
				    port, data, RTL8365MB_L2_ENTRY_SIZE);
	if (ret)
		return ret;

	rtl8365mb_l2_data_to_uc(data, &uc);

	ether_addr_copy(entry->mac_addr, uc.key.mac_addr);
	entry->vid = uc.key.vid;
	entry->is_static = uc.is_static;

	return 0;
}

int rtl8365mb_l2_add_uc(struct realtek_priv *priv, int port,
			const unsigned char mac_addr[static ETH_ALEN],
			u16 efid, u16 vid)
{
	u16 data[RTL8365MB_L2_ENTRY_SIZE] = { 0 };
	struct rtl8365mb_l2_uc uc = { 0 };
	u16 addr;
	int ret;

	memcpy(uc.key.mac_addr, mac_addr, ETH_ALEN);
	uc.key.efid = efid;
	uc.key.ivl = true;
	uc.key.vid = vid;
	uc.port = port;
	/* do not let HW decrease age */
	uc.is_static = true;
	/* age greater than 0 adds/updates entries */
	uc.age = 1;
	rtl8365mb_l2_uc_to_data(&uc, data);

	/* add the new entry or update an existing one */
	ret = rtl8365mb_table_query(priv, RTL8365MB_TABLE_L2,
				    RTL8365MB_TABLE_OP_WRITE, &addr,
				    0, 0,
				    data, RTL8365MB_L2_ENTRY_SIZE);
	/* addr will hold the table index, but it is not used here */
	if (ret == -ENOENT) {
		/* -ENOENT means the just added entry was not found (and @addr
		 * does not hold the table index. Although any error will be
		 * treated equally by the caller, assume that the missing entry
		 * means the table is full (tested in real HW).
		 */
		return -ENOSPC;
	}
	return ret;
}

int rtl8365mb_l2_del_uc(struct realtek_priv *priv, int port,
			const unsigned char mac_addr[static ETH_ALEN],
			u16 efid, u16 vid)
{
	u16 data[RTL8365MB_L2_ENTRY_SIZE] = { 0 };
	struct rtl8365mb_l2_uc uc = { 0 };
	u16 addr;
	int ret;

	memcpy(uc.key.mac_addr, mac_addr, ETH_ALEN);
	uc.key.efid = efid;
	uc.key.ivl = true;
	uc.key.vid = vid;
	/* age 0 deletes the entry */
	uc.age = 0;
	rtl8365mb_l2_uc_to_data(&uc, data);

	/* it looks like the switch will always add/update the entry,
	 * even when age is 0 or uc.key did not match an existing entry,
	 * just to immediately drop it because age is zero. You can still
	 * get the added/updated address from @addr
	 */
	ret = rtl8365mb_table_query(priv, RTL8365MB_TABLE_L2,
				    RTL8365MB_TABLE_OP_WRITE, &addr,
				    0, 0,
				    data, RTL8365MB_L2_ENTRY_SIZE);
	/* addr will hold the table index, but it is not used here */
	return ret;
}

int rtl8365mb_l2_flush(struct realtek_priv *priv, int port, u16 vid)
{
	int mode = vid ? RTL8365MB_L2_FLUSH_CTRL2_MODE_PORT_VID :
			 RTL8365MB_L2_FLUSH_CTRL2_MODE_PORT;
	u32 val, mask;
	int ret;

	mutex_lock(&priv->map_lock);

	/* Configure flushing mode; only flush dynamic entries */
	ret = regmap_write(priv->map_nolock, RTL8365MB_L2_FLUSH_CTRL2_REG,
			   FIELD_PREP(RTL8365MB_L2_FLUSH_CTRL2_MODE_MASK,
				      mode) |
			   FIELD_PREP(RTL8365MB_L2_FLUSH_CTRL2_TYPE_MASK,
				      RTL8365MB_L2_FLUSH_CTRL2_TYPE_DYNAMIC));
	if (ret)
		goto out;

	ret = regmap_write(priv->map_nolock, RTL8365MB_L2_FLUSH_CTRL1_REG,
			   FIELD_PREP(RTL8365MB_L2_FLUSH_CTRL1_VID_MASK, vid));

	if (ret)
		goto out;
	/* Now issue the flush command and wait for its completion. There are
	 * two registers for this purpose, and which one to use depends on the
	 * port number. The _EXT register is for ports 8 or higher.
	 */
	if (port < 8) {
		val = FIELD_PREP(RTL8365MB_L2_FLUSH_PORT_MASK_MASK,
				 BIT(port) & 0xFF);
		ret = regmap_write(priv->map_nolock,
				   RTL8365MB_L2_FLUSH_PORT_REG, val);
		if (ret)
			goto out;

		mask = FIELD_PREP(RTL8365MB_L2_FLUSH_PORT_BUSY_MASK,
				  BIT(port) & 0xFF);
		ret = regmap_read_poll_timeout(priv->map_nolock,
					       RTL8365MB_L2_FLUSH_PORT_REG,
					       val, !(val & mask), 10, 100);
		if (ret)
			goto out;
	} else {
		val = FIELD_PREP(RTL8365MB_L2_FLUSH_PORT_EXT_MASK_MASK,
				 BIT(port) >> 8);
		ret = regmap_write(priv->map_nolock,
				   RTL8365MB_L2_FLUSH_PORT_EXT_REG, val);
		if (ret)
			goto out;

		mask = FIELD_PREP(RTL8365MB_L2_FLUSH_PORT_EXT_BUSY_MASK,
				  BIT(port) >> 8);
		ret = regmap_read_poll_timeout(priv->map_nolock,
					       RTL8365MB_L2_FLUSH_PORT_EXT_REG,
					       val, !(val & mask), 10, 100);
		if (ret)
			goto out;
	}

out:
	mutex_unlock(&priv->map_lock);

	return ret;
}

int rtl8365mb_l2_add_mc(struct realtek_priv *priv, int port,
			const unsigned char mac_addr[static ETH_ALEN],
			u16 vid)
{
	u16 data[RTL8365MB_L2_ENTRY_SIZE] = { 0 };
	struct rtl8365mb_l2_mc mc = { 0 };
	u16 addr;
	int ret;

	memcpy(mc.key.mac_addr, mac_addr, ETH_ALEN);
	mc.key.vid = vid;
	mc.key.ivl = true;
	/* Already set the port and is_static, although not used in OP_READ,
	 * data will be ready for OP_WRITE if it is a new entry.
	 */
	mc.member |= BIT(port);
	mc.is_static = 1;
	rtl8365mb_l2_mc_to_data(&mc, data);

	/* First look for an existing entry (to get existing port members) */
	ret = rtl8365mb_table_query(priv, RTL8365MB_TABLE_L2,
				    RTL8365MB_TABLE_OP_READ, &addr,
				    RTL8365MB_TABLE_L2_METHOD_MAC, 0,
				    data, RTL8365MB_L2_ENTRY_SIZE);
	if (!ret) {
		/* There is already an entry... */
		rtl8365mb_l2_data_to_mc(data, &mc);
		/* the port must be added as a member */
		mc.member |= BIT(port);
		rtl8365mb_l2_mc_to_data(&mc, data);
	} else if (ret == -ENOENT) {
		/* New entry, no need to update data again as it already
		 * includes the member
		 */
	} else {
		return ret;
	}

	/* add the new entry or update an existing one */
	ret = rtl8365mb_table_query(priv, RTL8365MB_TABLE_L2,
				    RTL8365MB_TABLE_OP_WRITE, &addr,
				    0, 0,
				    data, RTL8365MB_L2_ENTRY_SIZE);
	/* addr will hold the table index, but it is not used here */
	if (ret == -ENOENT) {
		/* -ENOENT means the just added entry was not found (and @addr
		 * does not hold the table index. Although any error will be
		 * treated equally by the caller, assume that the missing entry
		 * means the table is full (tested in real HW).
		 */
		return -ENOSPC;
	}

	return ret;
}

int rtl8365mb_l2_del_mc(struct realtek_priv *priv, int port,
			const unsigned char mac_addr[static ETH_ALEN],
			u16 vid)
{
	u16 data[RTL8365MB_L2_ENTRY_SIZE] = { 0 };
	struct rtl8365mb_l2_mc mc = { 0 };
	u16 addr;
	int ret;

	memcpy(mc.key.mac_addr, mac_addr, ETH_ALEN);
	mc.key.vid = vid;
	mc.key.ivl = true;
	rtl8365mb_l2_mc_to_data(&mc, data);

	/* First look for an existing entry (to get existing port members) */
	ret = rtl8365mb_table_query(priv, RTL8365MB_TABLE_L2,
				    RTL8365MB_TABLE_OP_READ, &addr,
				    RTL8365MB_TABLE_L2_METHOD_MAC, 0,
				    data, RTL8365MB_L2_ENTRY_SIZE);
	if (ret)
		/* Any error, including -ENOENT is unexpected */
		return ret;

	rtl8365mb_l2_data_to_mc(data, &mc);
	/* the port must be removed as a member */
	mc.member &= ~BIT(port);
	if (!mc.member) {
		/* With no members, zero all non-key fields to delete the
		 * entry. However is_static is everything else we wrote.
		 * (and probably all that is needed by the HW)
		 */
		mc.is_static = 0;
	}
	rtl8365mb_l2_mc_to_data(&mc, data);

	/* update the existing entry. */
	ret = rtl8365mb_table_query(priv, RTL8365MB_TABLE_L2,
				    RTL8365MB_TABLE_OP_WRITE, &addr,
				    0, 0,
				    data, RTL8365MB_L2_ENTRY_SIZE);
	return ret;
}
