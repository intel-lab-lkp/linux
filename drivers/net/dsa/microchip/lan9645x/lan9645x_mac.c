// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2026 Microchip Technology Inc.
 */

#include "lan9645x_main.h"

#define CMD_IDLE		0
#define CMD_LEARN		1
#define CMD_FORGET		2
#define CMD_AGE			3
#define CMD_GET_NEXT		4
#define CMD_INIT		5
#define CMD_READ		6
#define CMD_WRITE		7
#define CMD_SYNC_GET_NEXT	8

static bool lan9645x_mact_entry_equal(struct lan9645x_mact_entry *entry,
				      const unsigned char *mac, u16 vid)
{
	/* The hardware table is keyed on (vid,mac) */
	return entry->common.key.vid == vid &&
	       ether_addr_equal(mac, entry->common.key.mac);
}

static struct lan9645x_mact_entry *
lan9645x_mact_entry_find(struct lan9645x *lan9645x, const unsigned char *mac,
			 u16 vid)
{
	struct lan9645x_mact_entry *entry;

	lockdep_assert_held(&lan9645x->mact_lock);

	list_for_each_entry(entry, &lan9645x->mac_entries, list)
		if (lan9645x_mact_entry_equal(entry, mac, vid))
			return entry;

	return NULL;
}

static struct lan9645x_mact_entry *
lan9645x_mact_entry_alloc(struct lan9645x *lan9645x, const unsigned char *mac,
			  u16 vid, u8 pgid, enum macaccess_entry_type type)
{
	struct lan9645x_mact_entry *entry;

	entry = kzalloc_obj(*entry);
	if (!entry)
		return NULL;

	ether_addr_copy(entry->common.key.mac, mac);
	entry->common.key.vid = vid;
	entry->common.pgid = pgid;
	entry->common.type = type;

	dev_dbg(lan9645x->dev,
		"mac=%pM vid=%u pgid=%u type=%d\n",
		entry->common.key.mac, entry->common.key.vid,
		entry->common.pgid, entry->common.type);

	return entry;
}

static void lan9645x_mact_entry_dealloc(struct lan9645x *lan9645x,
					struct lan9645x_mact_entry *entry)
{
	if (!entry)
		return;

	dev_dbg(lan9645x->dev,
		"mac=%pM vid=%u pgid=%u type=%d\n",
		entry->common.key.mac, entry->common.key.vid,
		entry->common.pgid, entry->common.type);

	list_del(&entry->list);
	kfree(entry);
}

static int lan9645x_mac_wait_for_completion(struct lan9645x *lan9645x,
					    u32 *maca)
{
	u32 val = 0;
	int err;

	lockdep_assert_held(&lan9645x->mact_lock);

	err = lan9645x_rd_poll_timeout(lan9645x, ANA_MACACCESS, val,
				       ANA_MACACCESS_MAC_TABLE_CMD_GET(val) ==
				       CMD_IDLE);
	if (err)
		return err;

	if (maca)
		*maca = val;

	return 0;
}

static void lan9645x_mact_parse(u32 machi, u32 maclo, u32 maca,
				struct lan9645x_mact_common *rentry)
{
	u64 addr = ANA_MACHDATA_MACHDATA_GET(machi);

	addr = addr << 32 | maclo;
	u64_to_ether_addr(addr, rentry->key.mac);
	rentry->key.vid = ANA_MACHDATA_VID_GET(machi);
	rentry->pgid = ANA_MACACCESS_DEST_IDX_GET(maca);
	rentry->type = ANA_MACACCESS_ENTRYTYPE_GET(maca);
}

static void lan9645x_mac_select(struct lan9645x *lan9645x,
				const unsigned char *addr, u16 vid)
{
	u64 maddr = ether_addr_to_u64(addr);

	lockdep_assert_held(&lan9645x->mact_lock);

	lan_wr(ANA_MACHDATA_VID_SET(vid) |
	       ANA_MACHDATA_MACHDATA_SET(maddr >> 32),
	       lan9645x,
	       ANA_MACHDATA);

	lan_wr(maddr & GENMASK(31, 0),
	       lan9645x,
	       ANA_MACLDATA);
}

static int __lan9645x_mact_forget(struct lan9645x *lan9645x,
				  const unsigned char mac[ETH_ALEN],
				  unsigned int vid,
				  enum macaccess_entry_type type)
{
	lockdep_assert_held(&lan9645x->mact_lock);

	lan9645x_mac_select(lan9645x, mac, vid);

	lan_wr(ANA_MACACCESS_ENTRYTYPE_SET(type) |
	       ANA_MACACCESS_MAC_TABLE_CMD_SET(CMD_FORGET),
	       lan9645x,
	       ANA_MACACCESS);

	return lan9645x_mac_wait_for_completion(lan9645x, NULL);
}

int lan9645x_mact_forget(struct lan9645x *lan9645x,
			 const unsigned char mac[ETH_ALEN], unsigned int vid,
			 enum macaccess_entry_type type)
{
	int err;

	mutex_lock(&lan9645x->mact_lock);
	err = __lan9645x_mact_forget(lan9645x, mac, vid, type);
	mutex_unlock(&lan9645x->mact_lock);

	return err;
}

static bool lan9645x_mac_ports_use_cpu(const unsigned char *mac,
				       enum macaccess_entry_type type)
{
	u32 mc_ports;

	switch (type) {
	case ENTRYTYPE_MACV4:
		mc_ports = (mac[1] << 8) | mac[2];
		break;
	case ENTRYTYPE_MACV6:
		mc_ports = (mac[0] << 8) | mac[1];
		break;
	default:
		return false;
	}

	return !!(mc_ports & BIT(CPU_PORT));
}

static int __lan9645x_mact_learn_cpu_copy(struct lan9645x *lan9645x, int port,
					  const unsigned char *addr, u16 vid,
					  enum macaccess_entry_type type,
					  bool cpu_copy)
{
	lockdep_assert_held(&lan9645x->mact_lock);

	lan9645x_mac_select(lan9645x, addr, vid);

	lan_wr(ANA_MACACCESS_VALID_SET(1) |
	       ANA_MACACCESS_DEST_IDX_SET(port) |
	       ANA_MACACCESS_MAC_CPU_COPY_SET(cpu_copy) |
	       ANA_MACACCESS_ENTRYTYPE_SET(type) |
	       ANA_MACACCESS_MAC_TABLE_CMD_SET(CMD_LEARN),
	       lan9645x, ANA_MACACCESS);

	return lan9645x_mac_wait_for_completion(lan9645x, NULL);
}

static int __lan9645x_mact_learn(struct lan9645x *lan9645x, int port,
				 const unsigned char *addr, u16 vid,
				 enum macaccess_entry_type type)
{
	bool cpu_copy = lan9645x_mac_ports_use_cpu(addr, type);

	return __lan9645x_mact_learn_cpu_copy(lan9645x, port, addr, vid, type,
					      cpu_copy);
}

int lan9645x_mact_learn(struct lan9645x *lan9645x, int port,
			const unsigned char *addr, u16 vid,
			enum macaccess_entry_type type)
{
	int err;

	mutex_lock(&lan9645x->mact_lock);
	err = __lan9645x_mact_learn(lan9645x, port, addr, vid, type);
	mutex_unlock(&lan9645x->mact_lock);

	return err;
}

int lan9645x_mact_flush(struct lan9645x *lan9645x, int port)
{
	int err;

	mutex_lock(&lan9645x->mact_lock);
	/* MAC table entries with dst index matching port are aged on scan. */
	lan_wr(ANA_ANAGEFIL_PID_EN_SET(1) |
	       ANA_ANAGEFIL_PID_VAL_SET(port),
	       lan9645x, ANA_ANAGEFIL);

	/* Flushing requires two scans. First sets AGE_FLAG=1, second removes
	 * entries with AGE_FLAG=1.
	 */
	lan_wr(ANA_MACACCESS_MAC_TABLE_CMD_SET(CMD_AGE),
	       lan9645x,
	       ANA_MACACCESS);

	err = lan9645x_mac_wait_for_completion(lan9645x, NULL);
	if (err)
		goto mact_unlock;

	lan_wr(ANA_MACACCESS_MAC_TABLE_CMD_SET(CMD_AGE),
	       lan9645x,
	       ANA_MACACCESS);

	err = lan9645x_mac_wait_for_completion(lan9645x, NULL);

mact_unlock:
	lan_wr(0, lan9645x, ANA_ANAGEFIL);
	mutex_unlock(&lan9645x->mact_lock);
	return err;
}

int lan9645x_mact_entry_add(struct lan9645x *lan9645x, int pgid,
			    const unsigned char *mac, u16 vid)
{
	struct lan9645x_mact_entry *entry;
	int err;

	mutex_lock(&lan9645x->mact_lock);

	/* Users can not move (vid,mac) to a different port, without removing
	 * the original entry first. But we overwrite entry in HW, and update
	 * software pgid for good measure.
	 */
	entry = lan9645x_mact_entry_find(lan9645x, mac, vid);
	if (entry) {
		err = __lan9645x_mact_learn(lan9645x, pgid, mac, vid,
					    ENTRYTYPE_LOCKED);
		if (!err)
			entry->common.pgid = pgid;
		mutex_unlock(&lan9645x->mact_lock);
		return err;
	}

	entry = lan9645x_mact_entry_alloc(lan9645x, mac, vid, pgid,
					  ENTRYTYPE_LOCKED);
	if (!entry) {
		mutex_unlock(&lan9645x->mact_lock);
		return -ENOMEM;
	}

	list_add_tail(&entry->list, &lan9645x->mac_entries);

	err = __lan9645x_mact_learn(lan9645x, pgid, mac, vid, ENTRYTYPE_LOCKED);
	if (err)
		lan9645x_mact_entry_dealloc(lan9645x, entry);

	mutex_unlock(&lan9645x->mact_lock);
	return err;
}

int lan9645x_mact_entry_del(struct lan9645x *lan9645x, int pgid,
			    const unsigned char *mac, u16 vid)
{
	struct lan9645x_mact_entry *entry;
	int err;

	mutex_lock(&lan9645x->mact_lock);

	entry = lan9645x_mact_entry_find(lan9645x, mac, vid);
	if (!entry) {
		mutex_unlock(&lan9645x->mact_lock);
		return -ENOENT;
	}

	WARN_ON(entry->common.pgid != pgid);
	err = __lan9645x_mact_forget(lan9645x, mac, vid, ENTRYTYPE_LOCKED);
	if (!err)
		lan9645x_mact_entry_dealloc(lan9645x, entry);

	mutex_unlock(&lan9645x->mact_lock);
	return err;
}

void lan9645x_mac_init(struct lan9645x *lan9645x)
{
	u32 val;

	mutex_init(&lan9645x->mact_lock);
	INIT_LIST_HEAD(&lan9645x->mac_entries);

	/* Clear the MAC table */
	lan_wr(ANA_MACACCESS_MAC_TABLE_CMD_SET(CMD_INIT),
	       lan9645x, ANA_MACACCESS);

	if (lan9645x_rd_poll_timeout(lan9645x, ANA_MACACCESS, val,
				     ANA_MACACCESS_MAC_TABLE_CMD_GET(val) ==
				     CMD_IDLE))
		dev_err(lan9645x->dev, "mac init timeout\n");
}

void lan9645x_mac_deinit(struct lan9645x *lan9645x)
{
	struct lan9645x_mact_entry *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, &lan9645x->mac_entries, list)
		lan9645x_mact_entry_dealloc(lan9645x, entry);

	mutex_destroy(&lan9645x->mact_lock);
}

int lan9645x_mact_dsa_dump(struct lan9645x *lan9645x, int port,
			   dsa_fdb_dump_cb_t *cb, void *data)
{
	struct lan9645x_mact_entry entry = {};
	u32 mach, macl, maca;
	int err = 0;
	u32 autoage;

	entry.common.type = ENTRYTYPE_NORMAL;

	mutex_lock(&lan9645x->mact_lock);

	/* The aging filter works both for aging scans and GET_NEXT table scans.
	 * With it, the HW table iteration only stops at entries matching our
	 * filter. Since DSA calls us for each port on a table dump, this helps
	 * avoid unnecessary work.
	 *
	 * Disable automatic aging temporarily. First save current state.
	 */
	autoage = lan_rd(lan9645x, ANA_AUTOAGE);

	/* Disable aging */
	lan_rmw(ANA_AUTOAGE_AGE_PERIOD_SET(0),
		ANA_AUTOAGE_AGE_PERIOD,
		lan9645x, ANA_AUTOAGE);

	/* Setup filter on our port */
	lan_wr(ANA_ANAGEFIL_PID_EN_SET(1) |
	       ANA_ANAGEFIL_PID_VAL_SET(port),
	       lan9645x, ANA_ANAGEFIL);

	lan_wr(0, lan9645x, ANA_MACHDATA);
	lan_wr(0, lan9645x, ANA_MACLDATA);

	while (1) {
		/* NOTE: we rely on mach, macl and type being set correctly in
		 * the registers from previous round, vis a vis the GET_NEXT
		 * semantics, so locking entire loop is important.
		 */
		lan_wr(ANA_MACACCESS_MAC_TABLE_CMD_SET(CMD_GET_NEXT) |
		       ANA_MACACCESS_ENTRYTYPE_SET(entry.common.type),
		       lan9645x, ANA_MACACCESS);

		err = lan9645x_mac_wait_for_completion(lan9645x, &maca);
		if (err)
			break;

		if (ANA_MACACCESS_VALID_GET(maca) == 0)
			break;

		mach = lan_rd(lan9645x, ANA_MACHDATA);
		macl = lan_rd(lan9645x, ANA_MACLDATA);

		lan9645x_mact_parse(mach, macl, maca, &entry.common);

		if (ANA_MACACCESS_DEST_IDX_GET(maca) == port &&
		    entry.common.type == ENTRYTYPE_NORMAL) {
			if (entry.common.key.vid > VLAN_MAX)
				entry.common.key.vid = 0;

			err = cb(entry.common.key.mac, entry.common.key.vid,
				 false, data);
			if (err)
				break;
		}
	}

	/* Remove aging filters and restore aging */
	lan_wr(0, lan9645x, ANA_ANAGEFIL);
	lan_rmw(ANA_AUTOAGE_AGE_PERIOD_SET(ANA_AUTOAGE_AGE_PERIOD_GET(autoage)),
		ANA_AUTOAGE_AGE_PERIOD,
		lan9645x, ANA_AUTOAGE);

	mutex_unlock(&lan9645x->mact_lock);

	return err;
}
