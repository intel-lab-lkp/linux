// SPDX-License-Identifier: GPL-2.0
/*
 * System Control and Management Interface (SCMI) Telemetry Protocol
 *
 * Copyright (C) 2026 ARM Ltd.
 */

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/compiler_types.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/sprintf.h>
#include <linux/string.h>
#include <linux/xarray.h>

#include "protocols.h"
#include "notify.h"

/* Updated only after ALL the mandatory features for that version are merged */
#define SCMI_PROTOCOL_SUPPORTED_VERSION		0x10000

#define SCMI_TLM_TDCF_MAX_RETRIES	5

enum scmi_telemetry_protocol_cmd {
	TELEMETRY_LIST_SHMTI = 0x3,
	TELEMETRY_DE_DESCRIPTION = 0x4,
	TELEMETRY_LIST_UPDATE_INTERVALS = 0x5,
	TELEMETRY_DE_CONFIGURE = 0x6,
	TELEMETRY_DE_ENABLED_LIST = 0x7,
	TELEMETRY_CONFIG_SET = 0x8,
	TELEMETRY_READING_COMPLETE = TELEMETRY_CONFIG_SET,
	TELEMETRY_CONFIG_GET = 0x9,
	TELEMETRY_RESET = 0xA,
};

struct scmi_msg_resp_telemetry_protocol_attributes {
	__le32 de_num;
	__le32 groups_num;
	__le32 de_implementation_rev_dword[SCMI_TLM_DE_IMPL_MAX_DWORDS];
	__le32 attributes;
#define SUPPORTS_SINGLE_READ(x)		((x) & BIT(31))
#define SUPPORTS_CONTINUOS_UPDATE(x)	((x) & BIT(30))
#define SUPPORTS_PER_GROUP_CONFIG(x)	((x) & BIT(18))
#define SUPPORTS_RESET(x)		((x) & BIT(17))
#define SUPPORTS_FC(x)			((x) & BIT(16))
};

struct scmi_telemetry_update_notify_payld {
	__le32 agent_id;
	__le32 status;
	__le32 num_dwords;
	__le32 array[] __counted_by(num_dwords);
};

struct scmi_shmti_desc {
	__le32 id;
	__le32 addr_low;
	__le32 addr_high;
	__le32 length;
};

struct scmi_msg_resp_telemetry_shmti_list {
	__le32 num_shmti;
	struct scmi_shmti_desc desc[] __counted_by(num_shmti);
};

struct de_desc_fc {
	__le32 addr_low;
	__le32 addr_high;
	__le32 size;
};

struct scmi_de_desc {
	__le32 id;
	__le32 grp_id;
	__le32 data_sz;
	__le32 attr_1;
#define	IS_NAME_SUPPORTED(d)	((d)->attr_1 & BIT(31))
#define	IS_FC_SUPPORTED(d)	((d)->attr_1 & BIT(30))
#define	GET_DE_TYPE(d)		(le32_get_bits((d)->attr_1, GENMASK(29, 22)))
#define	IS_PERSISTENT(d)	((d)->attr_1 & BIT(21))
#define GET_DE_UNIT_EXP(d)						\
	({								\
		__u32 __signed_exp =					\
			le32_get_bits((d)->attr_1, GENMASK(20, 13));	\
									\
		sign_extend32(__signed_exp, 7);				\
	})

#define	GET_DE_UNIT(d)		(le32_get_bits((d)->attr_1, GENMASK(12, 5)))

#define GET_DE_TSTAMP_EXP(d)						\
	({								\
		__u32 __signed_exp =					\
			FIELD_GET(GENMASK(4, 1), (d)->attr_1);		\
									\
		sign_extend32(__signed_exp, 3);				\
	})
#define	IS_TSTAMP_SUPPORTED(d)	((d)->attr_1 & BIT(0))
	__le32 attr_2;
#define	GET_DE_INSTA_ID(d)	(le32_get_bits((d)->attr_2, GENMASK(31, 24)))
#define	GET_COMPO_INSTA_ID(d)	(le32_get_bits((d)->attr_2, GENMASK(23, 8)))
#define	GET_COMPO_TYPE(d)	(le32_get_bits((d)->attr_2, GENMASK(7, 0)))
	__le32 reserved;
};

struct scmi_msg_resp_telemetry_de_description {
	__le32 num_desc;
	struct scmi_de_desc desc[] __counted_by(num_desc);
};

struct scmi_msg_telemetry_update_intervals {
	__le32 index;
	__le32 group_identifier;
#define	ALL_DES_NO_GROUP	0x0
#define SPECIFIC_GROUP_DES	0x1
#define ALL_DES_ANY_GROUP	0x2
	__le32 flags;
};

struct scmi_msg_resp_telemetry_update_intervals {
	__le32 flags;
#define INTERVALS_DISCRETE(x)	(!((x) & BIT(12)))
	__le32 intervals[];
};

struct scmi_msg_telemetry_de_enabled_list {
	__le32 index;
	__le32 flags;
};

struct scmi_enabled_de_desc {
	__le32 id;
	__le32 mode;
};

struct scmi_msg_resp_telemetry_de_enabled_list {
	__le32 flags;
	struct scmi_enabled_de_desc entry[];
};

struct scmi_msg_telemetry_de_configure {
	__le32 id;
	__le32 flags;
#define DE_ENABLE_NO_TSTAMP	BIT(0)
#define DE_ENABLE_WTH_TSTAMP	BIT(1)
#define DE_DISABLE_ALL		BIT(2)
#define GROUP_SELECTOR		BIT(3)
#define EVENT_DE		0
#define EVENT_GROUP		1
#define DE_DISABLE_ONE		0x0
};

struct scmi_msg_resp_telemetry_de_configure {
	__le32 shmti_id;
#define IS_SHMTI_ID_VALID(x)	((x) != 0xFFFFFFFF)
	__le32 tdcf_de_offset;
};

struct scmi_msg_telemetry_config_set {
	__le32 grp_id;
	__le32 control;
#define TELEMETRY_ENABLE		(BIT(0))

#define TELEMETRY_MODE_SET(x)		(FIELD_PREP(GENMASK(4, 1), (x)))
#define	TLM_ONDEMAND			(0)
#define	TLM_NOTIFS			(1)
#define	TLM_SINGLE			(2)
#define TELEMETRY_MODE_ONDEMAND		TELEMETRY_MODE_SET(TLM_ONDEMAND)
#define TELEMETRY_MODE_NOTIFS		TELEMETRY_MODE_SET(TLM_NOTIFS)
#define TELEMETRY_MODE_SINGLE		TELEMETRY_MODE_SET(TLM_SINGLE)

#define TLM_ORPHANS			(0)
#define TLM_GROUP			(1)
#define TLM_ALL				(2)
#define TELEMETRY_SET_SELECTOR(x)	(FIELD_PREP(GENMASK(8, 5), (x)))
#define	TELEMETRY_SET_SELECTOR_ORPHANS	TELEMETRY_SET_SELECTOR(TLM_ORPHANS)
#define	TELEMETRY_SET_SELECTOR_GROUP	TELEMETRY_SET_SELECTOR(TLM_GROUP)
#define	TELEMETRY_SET_SELECTOR_ALL	TELEMETRY_SET_SELECTOR(TLM_ALL)
	__le32 sampling_rate;
};

struct scmi_msg_resp_telemetry_reading_complete {
	__le32 num_dwords;
	__le32 dwords[] __counted_by(num_dwords);
};

struct scmi_msg_telemetry_config_get {
	__le32 grp_id;
	__le32 flags;
#define TELEMETRY_GET_SELECTOR(x)	(FIELD_PREP(GENMASK(3, 0), (x)))
#define	TELEMETRY_GET_SELECTOR_ORPHANS	TELEMETRY_GET_SELECTOR(TLM_ORPHANS)
#define	TELEMETRY_GET_SELECTOR_GROUP	TELEMETRY_GET_SELECTOR(TLM_GROUP)
#define	TELEMETRY_GET_SELECTOR_ALL	TELEMETRY_GET_SELECTOR(TLM_ALL)
};

struct scmi_msg_resp_telemetry_config_get {
	__le32 control;
#define TELEMETRY_MODE_GET		(FIELD_GET(GENMASK(4, 1)))
	__le32 sampling_rate;
};

/* TDCF */

#define _I(__a)		(ioread32((void __iomem *)(__a)))

#define TO_CPU_64(h, l)	((((u64)(h)) << 32) | (l))

enum scan_mode {
	SCAN_LOOKUP,
	SCAN_UPDATE,
	SCAN_DISCOVERY
};

struct fc_line {
	u32 data_low;
	u32 data_high;
};

struct fc_tsline {
	u32 data_low;
	u32 data_high;
	u32 ts_low;
	u32 ts_high;
};

struct line {
	u32 data_low;
	u32 data_high;
};

struct blk_tsline {
	u32 ts_low;
	u32 ts_high;
};

struct tsline {
	u32 data_low;
	u32 data_high;
	u32 ts_low;
	u32 ts_high;
};

#define LINE_DATA_GET(f)					\
({								\
	typeof(f) _f = (f);					\
								\
	(TO_CPU_64(_I(&_f->data_high), _I(&_f->data_low)));	\
})

#define LINE_TSTAMP_GET(f)					\
({								\
	typeof(f) _f = (f);					\
								\
	(TO_CPU_64(_I(&_f->ts_high), _I(&_f->ts_low)));	\
})

#define BLK_TSTAMP_GET(f)	LINE_TSTAMP_GET(f)

struct payload {
	u32 meta;
#define IS_BLK_TS(x)	(_I(&((x)->meta)) & BIT(4))
#define USE_BLK_TS(x)	(_I(&((x)->meta)) & BIT(3))
#define USE_LINE_TS(x)	(_I(&((x)->meta)) & BIT(2))
#define TS_VALID(x)	(_I(&((x)->meta)) & BIT(1))
#define	DATA_INVALID(x) (_I(&((x)->meta)) & BIT(0))
	u32 id;
	union {
		struct line l;
		struct tsline tsl;
		struct blk_tsline blk_tsl;
	};
};

#define PAYLD_ID(x)	(_I(&(((struct payload *)(x))->id)))

#define LINE_DATA_PAYLD_WORDS						       \
	((sizeof(u32) + sizeof(u32) + sizeof(struct line)) / sizeof(u32))
#define TS_LINE_DATA_PAYLD_WORDS					       \
	((sizeof(u32) + sizeof(u32) + sizeof(struct tsline)) / sizeof(u32))

#define QWORDS_LINE_DATA_PAYLD		(LINE_DATA_PAYLD_WORDS / 2)
#define QWORDS_TS_LINE_DATA_PAYLD	(TS_LINE_DATA_PAYLD_WORDS / 2)

struct prlg {
	u32 seq_low;
	u32 seq_high;
	u32 num_qwords;
	u32 _meta_header_high;
};

struct eplg {
	u32 seq_low;
	u32 seq_high;
};

#define TDCF_EPLG_SZ	(sizeof(struct eplg))

struct tdcf {
	struct prlg prlg;
	unsigned char payld[];
};

#define QWORDS(_t)	(_I(&(_t)->prlg.num_qwords))

#define SHMTI_MIN_SIZE	(sizeof(struct tdcf) + TDCF_EPLG_SZ)

#define	TDCF_BAD_END_SEQ	GENMASK_U64(63, 0)
#define TDCF_START_SEQ_GET(x)						       \
	({								       \
		u64 _val;						       \
		struct prlg *_p = &((x)->prlg);				       \
									       \
		_val = TO_CPU_64(_I(&_p->seq_high), _I(&_p->seq_low));         \
		(_val);							       \
	})

#define IS_BAD_START_SEQ(s)	((s) & 0x1)

#define	TDCF_END_SEQ_GET(e)						       \
	({								       \
		u64 _val;						       \
		struct eplg *_e = (e);					       \
									       \
		_val = TO_CPU_64(_I(&_e->seq_high), _I(&_e->seq_low));	       \
		(_val);							       \
	 })

struct telemetry_shmti {
	int id;
	void __iomem *base;
	u32 len;
	u64 last_magic;
};

#define SHMTI_EPLG(s)						\
	({							\
		struct telemetry_shmti *_s = (s);		\
		void *_eplg;					\
								\
		_eplg = _s->base + _s->len - TDCF_EPLG_SZ;	\
		(_eplg);					\
	})

struct telemetry_block_ts {
	refcount_t users;
	/* Protect block_ts accesses  */
	struct mutex mtx;
	u64 last_ts;
	u64 last_magic;
	struct payload __iomem *payld;
	struct xarray *xa_bts;
};

struct telemetry_de {
	bool enumerated;
	bool cached;
	void __iomem *base;
	void __iomem *eplg;
	u32 offset;
	/* NOTE THAT DE data_sz is registered in scmi_telemetry_de */
	u32 fc_size;
	/* Protect last_val/ts/magic accesses  */
	struct mutex mtx;
	u64 last_val;
	u64 last_ts;
	u64 last_magic;
	struct list_head item;
	struct telemetry_block_ts *bts;
	struct scmi_telemetry_de de;
};

#define to_tde(d)	container_of(d, struct telemetry_de, de)

#define DE_ENABLED_WITH_TSTAMP	2

struct telemetry_info {
	bool streaming_mode;
	int num_shmti;
	const struct scmi_protocol_handle *ph;
	struct telemetry_shmti *shmti;
	struct telemetry_de *tdes;
	struct scmi_telemetry_group *grps;
	struct xarray xa_des;
	struct xarray xa_bts;
	/* Mutex to protect access to @free_des */
	struct mutex free_mtx;
	struct list_head free_des;
	struct list_head fcs_des;
	struct scmi_telemetry_info info;
	struct notifier_block telemetry_nb;
	atomic_t rinfo_initializing;
	struct completion rinfo_initdone;
	struct scmi_telemetry_res_info __private *rinfo;
	struct scmi_telemetry_res_info *(*res_get)(struct telemetry_info *ti);
};

#define telemetry_nb_to_info(x)	\
	container_of(x, struct telemetry_info, telemetry_nb)

static struct scmi_telemetry_res_info *
__scmi_telemetry_resources_get(struct telemetry_info *ti);

static int scmi_telemetry_shmti_scan(struct telemetry_info *ti,
				     unsigned int shmti_id, u64 ts,
				     enum scan_mode mode);

static struct telemetry_de *
scmi_telemetry_free_tde_get(struct telemetry_info *ti)
{
	struct telemetry_de *tde;

	guard(mutex)(&ti->free_mtx);

	tde = list_first_entry_or_null(&ti->free_des, struct telemetry_de, item);
	if (!tde)
		return tde;

	list_del(&tde->item);

	return tde;
}

static void scmi_telemetry_free_tde_put(struct telemetry_info *ti,
					struct telemetry_de *tde)
{
	guard(mutex)(&ti->free_mtx);

	list_add_tail(&tde->item, &ti->free_des);
}

static struct telemetry_de *scmi_telemetry_tde_lookup(struct telemetry_info *ti,
						      unsigned int de_id)
{
	struct scmi_telemetry_de *de;

	de = xa_load(&ti->xa_des, de_id);
	if (!de)
		return NULL;

	return to_tde(de);
}

static struct telemetry_de *scmi_telemetry_tde_get(struct telemetry_info *ti,
						   unsigned int de_id)
{
	static struct telemetry_de *tde;

	/* Pick a new tde */
	tde = scmi_telemetry_free_tde_get(ti);
	if (!tde) {
		dev_err(ti->ph->dev, "Cannot allocate DE for ID:0x%08X\n", de_id);
		return ERR_PTR(-ENOSPC);
	}

	return tde;
}

static int scmi_telemetry_tde_register(struct telemetry_info *ti,
				       struct telemetry_de *tde)
{
	int ret;

	if (ti->rinfo->num_des >= ti->info.base.num_des) {
		ret = -ENOSPC;
		goto err;
	}

	/* Store DE pointer by de_id ... */
	ret = xa_insert(&ti->xa_des, tde->de.info->id, &tde->de, GFP_KERNEL);
	if (ret)
		goto err;

	/* ... and in the general array */
	ti->rinfo->des[ti->rinfo->num_des++] = &tde->de;

	return 0;

err:
	dev_err(ti->ph->dev, "Cannot register DE for ID:0x%08X\n",
		tde->de.info->id);

	return ret;
}

struct scmi_tlm_de_priv {
	struct telemetry_info *ti;
	void *next;
};

static int
scmi_telemetry_protocol_attributes_get(struct telemetry_info *ti)
{
	struct scmi_msg_resp_telemetry_protocol_attributes *resp;
	const struct scmi_protocol_handle *ph = ti->ph;
	struct scmi_xfer *t;
	int ret;

	ret = ph->xops->xfer_get_init(ph, PROTOCOL_ATTRIBUTES, 0,
				      sizeof(*resp), &t);
	if (ret)
		return ret;

	resp = t->rx.buf;
	ret = ph->xops->do_xfer(ph, t);
	if (!ret) {
		__le32 attr = resp->attributes;

		ti->info.base.num_des = le32_to_cpu(resp->de_num);
		ti->info.base.num_groups = le32_to_cpu(resp->groups_num);
		for (int i = 0; i < SCMI_TLM_DE_IMPL_MAX_DWORDS; i++)
			ti->info.base.de_impl_version[i] =
				le32_to_cpu(resp->de_implementation_rev_dword[i]);
		ti->info.single_read_support = SUPPORTS_SINGLE_READ(attr);
		ti->info.continuos_update_support = SUPPORTS_CONTINUOS_UPDATE(attr);
		ti->info.per_group_config_support = SUPPORTS_PER_GROUP_CONFIG(attr);
		ti->info.reset_support = SUPPORTS_RESET(attr);
		ti->info.fc_support = SUPPORTS_FC(attr);
		ti->num_shmti = le32_get_bits(attr, GENMASK(15, 0));
	}

	ph->xops->xfer_put(ph, t);

	return ret;
}

static void iter_tlm_prepare_message(void *message,
				     unsigned int desc_index, const void *priv)
{
	put_unaligned_le32(desc_index, message);
}

static int iter_de_descr_update_state(struct scmi_iterator_state *st,
				      const void *response, void *priv)
{
	const struct scmi_msg_resp_telemetry_de_description *r = response;
	struct scmi_tlm_de_priv *p = priv;

	st->num_returned = le32_get_bits(r->num_desc, GENMASK(15, 0));
	st->num_remaining = le32_get_bits(r->num_desc, GENMASK(31, 16));

	/* Initialized to first descriptor */
	p->next = (void *)r->desc;

	return 0;
}

static int scmi_telemetry_de_descriptor_parse(struct telemetry_info *ti,
					      struct telemetry_de *tde,
					      void **next)
{
	struct scmi_telemetry_res_info *rinfo = ti->rinfo;
	const struct scmi_de_desc *desc = *next;
	unsigned int grp_id;

	tde->de.info->id = le32_to_cpu(desc->id);
	grp_id = le32_to_cpu(desc->grp_id);
	if (grp_id != SCMI_TLM_GRP_INVALID) {
		/* Group descriptors are empty but allocated at this point */
		if (grp_id >= ti->info.base.num_groups)
			return -EINVAL;

		/* Link to parent group */
		tde->de.info->grp_id = grp_id;
		tde->de.grp = &rinfo->grps[grp_id];
	}

	tde->de.info->data_sz = le32_to_cpu(desc->data_sz);
	tde->de.info->type = GET_DE_TYPE(desc);
	tde->de.info->unit = GET_DE_UNIT(desc);
	tde->de.info->unit_exp = GET_DE_UNIT_EXP(desc);
	tde->de.info->tstamp_exp = GET_DE_TSTAMP_EXP(desc);
	tde->de.info->instance_id = GET_DE_INSTA_ID(desc);
	tde->de.info->compo_instance_id = GET_COMPO_INSTA_ID(desc);
	tde->de.info->compo_type = GET_COMPO_TYPE(desc);
	tde->de.info->persistent = IS_PERSISTENT(desc);
	tde->de.tstamp_support = IS_TSTAMP_SUPPORTED(desc);
	tde->de.fc_support = IS_FC_SUPPORTED(desc);
	tde->de.name_support = IS_NAME_SUPPORTED(desc);
	/* Update DE_DESCRIPTOR size for the next iteration */
	*next += sizeof(*desc);
	if (tde->de.fc_support) {
		u32 size;
		u64 phys_addr;
		void __iomem *addr;
		struct de_desc_fc *dfc;

		dfc = *next;
		phys_addr = le32_to_cpu(dfc->addr_low);
		phys_addr |= (u64)le32_to_cpu(dfc->addr_high) << 32;

		size = le32_to_cpu(dfc->size);
		addr = devm_ioremap(ti->ph->dev, phys_addr, size);
		if (!addr)
			return -EADDRNOTAVAIL;

		tde->base = addr;
		tde->offset = 0;
		tde->fc_size = size;

		/* Add to FastChannels list */
		list_add(&tde->item, &ti->fcs_des);

		/* Variably sized depending on FC support */
		*next += sizeof(*dfc);
	}

	if (tde->de.name_support) {
		const char *de_name = *next;

		strscpy(tde->de.info->name, de_name, SCMI_SHORT_NAME_MAX_SIZE);
		/* Variably sized depending on name support */
		*next += SCMI_SHORT_NAME_MAX_SIZE;
	}

	return 0;
}

static int iter_de_descr_process_response(const struct scmi_protocol_handle *ph,
					  const void *response,
					  struct scmi_iterator_state *st,
					  void *priv)
{
	struct scmi_tlm_de_priv *p = priv;
	struct telemetry_info *ti = p->ti;
	const struct scmi_de_desc *desc = p->next;
	struct telemetry_de *tde;
	bool discovered = false;
	unsigned int de_id;
	int ret;

	de_id = le32_to_cpu(desc->id);
	/* Check if this DE has already been discovered by other means... */
	tde = scmi_telemetry_tde_lookup(ti, de_id);
	if (!tde) {
		/* Create a new one */
		tde = scmi_telemetry_tde_get(ti, de_id);
		if (IS_ERR(tde))
			return PTR_ERR(tde);

		discovered = true;
	} else if (tde->enumerated) {
		/* Cannot be a duplicate of a DE already created by enumeration */
		dev_err(ph->dev,
			"Discovered INVALID DE with DUPLICATED ID:0x%08X\n",
			de_id);
		return -EINVAL;
	}

	ret = scmi_telemetry_de_descriptor_parse(ti, tde, &p->next);
	if (ret)
		goto err;

	if (discovered) {
		/* Register if it was not already ... */
		ret = scmi_telemetry_tde_register(ti, tde);
		if (ret)
			goto err;

		tde->enumerated = true;
	}

	/* Account for this DE in group num_de counter */
	if (tde->de.grp)
		tde->de.grp->info->num_des++;

	return 0;

err:
	/* DE not enumerated at this point were created in this call */
	if (!tde->enumerated)
		scmi_telemetry_free_tde_put(ti, tde);

	return ret;
}

static int scmi_telemetry_config_lookup(struct telemetry_info *ti,
					unsigned int grp_id, bool *enabled,
					unsigned int *active_update_interval)
{
	const struct scmi_protocol_handle *ph = ti->ph;
	struct scmi_msg_telemetry_config_get *msg;
	struct scmi_msg_resp_telemetry_config_get *resp;
	struct scmi_xfer *t;
	int ret;

	ret = ph->xops->xfer_get_init(ph, TELEMETRY_CONFIG_GET,
				      sizeof(*msg), sizeof(*resp), &t);
	if (ret)
		return ret;

	msg = t->tx.buf;
	msg->grp_id = grp_id;
	msg->flags = grp_id == SCMI_TLM_GRP_INVALID ?
		TELEMETRY_GET_SELECTOR_ORPHANS : TELEMETRY_GET_SELECTOR_GROUP;

	resp = t->rx.buf;
	ret = ph->xops->do_xfer(ph, t);
	if (!ret) {
		*enabled = resp->control & TELEMETRY_ENABLE;
		*active_update_interval =
			SCMI_TLM_GET_UPDATE_INTERVAL(resp->sampling_rate);
	}

	ph->xops->xfer_put(ph, t);

	return 0;
}

static int scmi_telemetry_group_config_lookup(struct telemetry_info *ti,
					      struct scmi_telemetry_group *grp)
{
	return scmi_telemetry_config_lookup(ti, grp->info->id, &grp->enabled,
					    &grp->active_update_interval);
}

static void iter_enabled_list_prepare_message(void *message,
					      unsigned int desc_index,
					      const void *priv)
{
	struct scmi_msg_telemetry_de_enabled_list *msg = message;

	msg->index = cpu_to_le32(desc_index);
	msg->flags = 0;
}

static int iter_enabled_list_update_state(struct scmi_iterator_state *st,
					  const void *response, void *priv)
{
	const struct scmi_msg_resp_telemetry_de_enabled_list *r = response;

	st->num_returned = le32_get_bits(r->flags, GENMASK(15, 0));
	st->num_remaining = le32_get_bits(r->flags, GENMASK(31, 16));

	/*
	 * total enabled is not declared previously anywhere so we
	 * assume it's returned+remaining on first call.
	 */
	if (!st->max_resources)
		st->max_resources = st->num_returned + st->num_remaining;

	return 0;
}

static int
iter_enabled_list_process_response(const struct scmi_protocol_handle *ph,
				   const void *response,
				   struct scmi_iterator_state *st, void *priv)
{
	const struct scmi_msg_resp_telemetry_de_enabled_list *r = response;
	const struct scmi_enabled_de_desc *desc;
	struct telemetry_info *ti = priv;
	struct telemetry_de *tde;
	u32 de_id;
	int ret;

	desc = &r->entry[st->loop_idx];
	de_id = le32_to_cpu(desc->id);
	if (scmi_telemetry_tde_lookup(ti, de_id)) {
		dev_err(ph->dev,
			"Found INVALID DE with DUPLICATED ID:0x%08X\n", de_id);
		return -EINVAL;
	}

	tde = scmi_telemetry_tde_get(ti, de_id);
	if (IS_ERR(tde))
		return PTR_ERR(tde);

	tde->de.info->id = de_id;
	tde->de.enabled = true;
	tde->de.tstamp_enabled = desc->mode == DE_ENABLED_WITH_TSTAMP;

	ret = scmi_telemetry_tde_register(ti, tde);
	if (ret) {
		scmi_telemetry_free_tde_put(ti, tde);
		return ret;
	}

	dev_dbg(ph->dev, "Registered new ENABLED DE with ID:0x%08X\n",
		tde->de.info->id);

	return 0;
}

static int scmi_telemetry_enumerate_des_enabled_list(struct telemetry_info *ti)
{
	const struct scmi_protocol_handle *ph = ti->ph;
	struct scmi_iterator_ops ops = {
		.prepare_message = iter_enabled_list_prepare_message,
		.update_state = iter_enabled_list_update_state,
		.process_response = iter_enabled_list_process_response,
	};
	void *iter;
	int ret;

	iter = ph->hops->iter_response_init(ph, &ops, 0,
					    TELEMETRY_DE_ENABLED_LIST,
					    sizeof(u32) * 2, ti);
	if (IS_ERR(iter))
		return PTR_ERR(iter);

	ret = ph->hops->iter_response_run(iter);
	if (ret)
		return ret;

	dev_info(ti->ph->dev, "Found %u enabled DEs.\n", ti->rinfo->num_des);

	return 0;
}

static int scmi_telemetry_initial_state_lookup(struct telemetry_info *ti)
{
	struct device *dev = ti->ph->dev;
	int ret;

	ret = scmi_telemetry_config_lookup(ti, SCMI_TLM_GRP_INVALID,
					   &ti->info.enabled,
					   &ti->info.active_update_interval);
	if (ret)
		return ret;

	if (ti->info.enabled) {
		/*
		 * When Telemetry is found already enabled on the platform,
		 * proceed with passive discovery using DE_ENABLED_LIST and
		 * TCDF scanning: note that this CAN only discover DEs exposed
		 * via SHMTIs.
		 * FastChannel DEs need a proper DE_DESCRIPTION enumeration,
		 * while, even though incoming Notifications could be used for
		 * passive discovery too, it would carry a considerable risk
		 * of assimilating trash as DEs.
		 */
		dev_info(dev,
			 "Telemetry found enabled with update interval %ux10^%d\n",
			 SCMI_TLM_GET_UPDATE_INTERVAL_SECS(ti->info.active_update_interval),
			 SCMI_TLM_GET_UPDATE_INTERVAL_EXP(ti->info.active_update_interval));
		/*
		 * Query enabled DEs list: collect states.
		 * It will include DEs from any interface.
		 * Enabled groups still NOT enumerated.
		 */
		ret = scmi_telemetry_enumerate_des_enabled_list(ti);
		if (ret)
			dev_warn(dev, FW_BUG "Cannot query enabled DE list. Carry-on.\n");

		/* Discover DEs on SHMTis: collect states/offsets/values */
		for (int id = 0; id < ti->num_shmti; id++) {
			ret = scmi_telemetry_shmti_scan(ti, id, 0, SCAN_DISCOVERY);
			if (ret)
				dev_warn(dev, "Failed discovery-scan of SHMTI ID:%d\n", id);
		}
	}

	return 0;
}

static int
scmi_telemetry_de_groups_init(struct device *dev, struct telemetry_info *ti)
{
	struct scmi_telemetry_res_info *rinfo = ti->rinfo;

	/* Allocate all groups DEs IDs arrays at first ... */
	for (int i = 0; i < ti->info.base.num_groups; i++) {
		struct scmi_telemetry_group *grp = &rinfo->grps[i];
		size_t des_str_sz;

		unsigned int *des __free(kfree) = kcalloc(grp->info->num_des,
							  sizeof(unsigned int),
							  GFP_KERNEL);
		if (!des)
			return -ENOMEM;

		/*
		 * Max size 32bit ID string in Hex: 0xCAFECAFE
		 *  - 10 digits + ' '/'\n' = 11 bytes per  number
		 *  - terminating NUL character
		 */
		des_str_sz = grp->info->num_des * 11 + 1;
		char *des_str __free(kfree) = kzalloc(des_str_sz, GFP_KERNEL);
		if (!des_str)
			return -ENOMEM;

		grp->des = no_free_ptr(des);
		grp->des_str = no_free_ptr(des_str);
		/* Reset group DE counter */
		grp->info->num_des = 0;
	}

	/* Scan DEs and populate DE IDs arrays for all groups */
	for (int i = 0; i < rinfo->num_des; i++) {
		struct scmi_telemetry_group *grp = rinfo->des[i]->grp;

		if (!grp)
			continue;

		/*
		 * Note that, at this point, num_des is guaranteed to be
		 * sane (in-bounds) by construction.
		 */
		grp->des[grp->info->num_des++] = i;
	}

	/* Build composing DES string */
	for (int i = 0; i < ti->info.base.num_groups; i++) {
		struct scmi_telemetry_group *grp = &rinfo->grps[i];
		size_t bufsize = grp->info->num_des * 11 + 1;
		char *buf = grp->des_str;

		for (int j = 0; j < grp->info->num_des; j++) {
			char term = j != (grp->info->num_des - 1) ? ' ' : '\0';
			int len;

			len = scnprintf(buf, bufsize, "0x%04X%c",
					rinfo->des[grp->des[j]]->info->id, term);

			buf += len;
			bufsize -= len;
		}
	}

	for (int i = 0; i < ti->info.base.num_groups; i++)
		scmi_telemetry_group_config_lookup(ti, &rinfo->grps[i]);

	rinfo->num_groups = ti->info.base.num_groups;

	return 0;
}

static int scmi_telemetry_de_descriptors_get(struct telemetry_info *ti)
{
	const struct scmi_protocol_handle *ph = ti->ph;

	struct scmi_iterator_ops ops = {
		.prepare_message = iter_tlm_prepare_message,
		.update_state = iter_de_descr_update_state,
		.process_response = iter_de_descr_process_response,
	};
	struct scmi_tlm_de_priv tpriv = {
		.ti = ti,
		.next = NULL,
	};
	void *iter;
	int ret;

	if (!ti->info.base.num_des)
		return 0;

	iter = ph->hops->iter_response_init(ph, &ops, ti->info.base.num_des,
					    TELEMETRY_DE_DESCRIPTION,
					    sizeof(u32), &tpriv);
	if (IS_ERR(iter))
		return PTR_ERR(iter);

	ret = ph->hops->iter_response_run(iter);
	if (ret)
		return ret;

	return scmi_telemetry_de_groups_init(ph->dev, ti);
}

struct scmi_tlm_ivl_priv {
	struct device *dev;
	struct scmi_tlm_intervals **intrvs;
	unsigned int grp_id;
	unsigned int flags;
};

static void iter_intervals_prepare_message(void *message,
					   unsigned int desc_index,
					   const void *priv)
{
	struct scmi_msg_telemetry_update_intervals *msg = message;
	const struct scmi_tlm_ivl_priv *p = priv;

	msg->index = cpu_to_le32(desc_index);
	msg->group_identifier = cpu_to_le32(p->grp_id);
	msg->flags = FIELD_PREP(GENMASK(3, 0), p->flags);
}

static int iter_intervals_update_state(struct scmi_iterator_state *st,
				       const void *response, void *priv)
{
	const struct scmi_msg_resp_telemetry_update_intervals *r = response;

	st->num_returned = le32_get_bits(r->flags, GENMASK(11, 0));
	st->num_remaining = le32_get_bits(r->flags, GENMASK(31, 16));

	/*
	 * total intervals is not declared previously anywhere so we
	 * assume it's returned+remaining on first call.
	 */
	if (!st->max_resources) {
		struct scmi_tlm_ivl_priv *p = priv;
		bool discrete;
		int inum;

		discrete = INTERVALS_DISCRETE(r->flags);
		/* Check consistency on first call */
		if (!discrete && (st->num_returned != 3 || st->num_remaining != 0))
			return -EINVAL;

		inum = st->num_returned + st->num_remaining;
		struct scmi_tlm_intervals *intrvs __free(kfree) =
			kzalloc(sizeof(*intrvs) + inum * sizeof(__u32), GFP_KERNEL);
		if (!intrvs)
			return -ENOMEM;

		intrvs->num = inum;
		intrvs->discrete = discrete;
		st->max_resources = intrvs->num;

		*p->intrvs = no_free_ptr(intrvs);
	}

	return 0;
}

static int
iter_intervals_process_response(const struct scmi_protocol_handle *ph,
				const void *response,
				struct scmi_iterator_state *st, void *priv)
{
	const struct scmi_msg_resp_telemetry_update_intervals *r = response;
	struct scmi_tlm_ivl_priv *p = priv;
	struct scmi_tlm_intervals *intrvs = *p->intrvs;
	unsigned int idx = st->loop_idx;

	intrvs->update_intervals[st->desc_index + idx] = r->intervals[idx];

	return 0;
}

static int
scmi_tlm_enumerate_update_intervals(struct telemetry_info *ti,
				    struct scmi_tlm_intervals **intervals,
				    int grp_id, unsigned int flags)
{
	struct scmi_iterator_ops ops = {
		.prepare_message = iter_intervals_prepare_message,
		.update_state = iter_intervals_update_state,
		.process_response = iter_intervals_process_response,
	};
	const struct scmi_protocol_handle *ph = ti->ph;
	struct scmi_tlm_ivl_priv ipriv = {
		.dev = ph->dev,
		.grp_id = grp_id,
		.intrvs = intervals,
		.flags = flags,
	};
	void *iter;

	iter = ph->hops->iter_response_init(ph, &ops, 0,
					    TELEMETRY_LIST_UPDATE_INTERVALS,
			     sizeof(struct scmi_msg_telemetry_update_intervals),
					    &ipriv);
	if (IS_ERR(iter))
		return PTR_ERR(iter);

	return ph->hops->iter_response_run(iter);
}

static int
scmi_telemetry_enumerate_groups_intervals(struct telemetry_info *ti)
{
	struct scmi_telemetry_res_info *rinfo = ti->rinfo;

	if (!ti->info.per_group_config_support)
		return 0;

	for (int id = 0; id < rinfo->num_groups; id++) {
		int ret;

		ret = scmi_tlm_enumerate_update_intervals(ti,
							  &rinfo->grps[id].intervals,
							  id, SPECIFIC_GROUP_DES);
		if (ret)
			return ret;

		rinfo->grps_store[id].num_intervals =
			rinfo->grps[id].intervals->num;
	}

	return 0;
}

static void scmi_telemetry_intervals_free(void *interval)
{
	kfree(interval);
}

static int
scmi_telemetry_enumerate_common_intervals(struct telemetry_info *ti)
{
	unsigned int flags;
	int ret;

	flags = !ti->info.per_group_config_support ?
		ALL_DES_ANY_GROUP : ALL_DES_NO_GROUP;

	ret = scmi_tlm_enumerate_update_intervals(ti, &ti->info.intervals,
						  SCMI_TLM_GRP_INVALID, flags);
	if (ret)
		return ret;

	/* A copy for UAPI access... */
	ti->info.base.num_intervals = ti->info.intervals->num;

	/* Delegate freeing of allocated intervals to unbind time */
	return devm_add_action_or_reset(ti->ph->dev,
					scmi_telemetry_intervals_free,
					ti->info.intervals);
}

static int iter_shmti_update_state(struct scmi_iterator_state *st,
				   const void *response, void *priv)
{
	const struct scmi_msg_resp_telemetry_shmti_list *r = response;

	st->num_returned = le32_get_bits(r->num_shmti, GENMASK(15, 0));
	st->num_remaining = le32_get_bits(r->num_shmti, GENMASK(31, 16));

	return 0;
}

static int iter_shmti_process_response(const struct scmi_protocol_handle *ph,
				       const void *response,
				       struct scmi_iterator_state *st,
				       void *priv)
{
	const struct scmi_msg_resp_telemetry_shmti_list *r = response;
	struct telemetry_info *ti = priv;
	struct telemetry_shmti *shmti;
	const struct scmi_shmti_desc *desc;
	void __iomem *addr;
	u64 phys_addr;
	u32 len;

	desc = &r->desc[st->loop_idx];
	shmti = &ti->shmti[st->desc_index + st->loop_idx];

	shmti->id = le32_to_cpu(desc->id);
	phys_addr = le32_to_cpu(desc->addr_low);
	phys_addr |= (u64)le32_to_cpu(desc->addr_high) << 32;

	len = le32_to_cpu(desc->length);
	if (len < SHMTI_MIN_SIZE)
		return -EINVAL;

	addr = devm_ioremap(ph->dev, phys_addr, len);
	if (!addr)
		return -EADDRNOTAVAIL;

	shmti->base = addr;
	shmti->len = len;

	return 0;
}

static int scmi_telemetry_shmti_list(const struct scmi_protocol_handle *ph,
				     struct telemetry_info *ti)
{
	struct scmi_iterator_ops ops = {
		.prepare_message = iter_tlm_prepare_message,
		.update_state = iter_shmti_update_state,
		.process_response = iter_shmti_process_response,
	};
	void *iter;

	iter = ph->hops->iter_response_init(ph, &ops, ti->info.base.num_des,
					    TELEMETRY_LIST_SHMTI,
					    sizeof(u32), ti);
	if (IS_ERR(iter))
		return PTR_ERR(iter);

	return ph->hops->iter_response_run(iter);
}

static int scmi_telemetry_enumerate_shmti(struct telemetry_info *ti)
{
	const struct scmi_protocol_handle *ph = ti->ph;
	int ret;

	if (!ti->num_shmti)
		return 0;

	ti->shmti = devm_kcalloc(ph->dev, ti->num_shmti, sizeof(*ti->shmti),
				 GFP_KERNEL);
	if (!ti->shmti)
		return -ENOMEM;

	ret = scmi_telemetry_shmti_list(ph, ti);
	if (ret) {
		dev_err(ph->dev, "Cannot get SHMTI list descriptors");
		return ret;
	}

	return 0;
}

static const struct scmi_telemetry_info *
scmi_telemetry_info_get(const struct scmi_protocol_handle *ph)
{
	struct telemetry_info *ti = ph->get_priv(ph);

	return &ti->info;
}

static const struct scmi_telemetry_de *
scmi_telemetry_de_lookup(const struct scmi_protocol_handle *ph, u32 id)
{
	struct telemetry_info *ti = ph->get_priv(ph);
	struct scmi_telemetry_de *de;

	ti->res_get(ti);
	de = xa_load(&ti->xa_des, id);
	if (!de)
		return NULL;

	return de;
}

static const struct scmi_telemetry_res_info *
scmi_telemetry_resources_get(const struct scmi_protocol_handle *ph)
{
	struct telemetry_info *ti = ph->get_priv(ph);

	return ti->res_get(ti);
}

static u64
scmi_telemetry_blkts_read(u64 magic, struct telemetry_block_ts *bts)
{
	if (WARN_ON(!bts || !refcount_read(&bts->users)))
		return 0;

	guard(mutex)(&bts->mtx);

	if (bts->last_magic == magic)
		return bts->last_ts;

	bts->last_ts = BLK_TSTAMP_GET(&bts->payld->blk_tsl);
	bts->last_magic = magic;

	return bts->last_ts;
}

static void scmi_telemetry_blkts_update(u64 magic,
					struct telemetry_block_ts *bts)
{
	guard(mutex)(&bts->mtx);

	if (bts->last_magic != magic) {
		bts->last_ts = BLK_TSTAMP_GET(&bts->payld->blk_tsl);
		bts->last_magic = magic;
	}
}

static void scmi_telemetry_blkts_put(struct device *dev,
				     struct telemetry_block_ts *bts)
{
	if (refcount_dec_and_test(&bts->users)) {
		scoped_guard(mutex, &bts->mtx)
			xa_erase(bts->xa_bts, (unsigned long)bts->payld);
		devm_kfree(dev, bts);
	}
}

static struct telemetry_block_ts *
scmi_telemetry_blkts_get(struct xarray *xa_bts, struct payload *payld)
{
	struct telemetry_block_ts *bts;

	bts = xa_load(xa_bts, (unsigned long)payld);
	if (!bts)
		return NULL;

	refcount_inc(&bts->users);

	return bts;
}

static struct payload *
scmi_telemetry_nearest_blk_ts(struct telemetry_shmti *shmti,
			      struct payload *last_payld)
{
	struct payload *payld, *bts_payld = NULL;
	struct tdcf __iomem *tdcf = shmti->base;
	u32 *next;

	/* Scan from start of TDCF payloads up to last_payld */
	payld = (struct payload *)tdcf->payld;
	next = (u32 *)payld;
	while (payld < last_payld) {
		if (IS_BLK_TS(payld))
			bts_payld = payld;

		next += USE_LINE_TS(payld) ?
			TS_LINE_DATA_PAYLD_WORDS : LINE_DATA_PAYLD_WORDS;
		payld = (struct payload *)next;
	}

	return bts_payld;
}

static struct telemetry_block_ts *
scmi_telemetry_blkts_lookup(struct device *dev, struct xarray *xa_bts,
			    struct payload *payld)
{
	struct telemetry_block_ts *bts;

	bts = xa_load(xa_bts, (unsigned long)payld);
	if (!bts) {
		int ret;

		bts = devm_kzalloc(dev, sizeof(*bts), GFP_KERNEL);
		if (!bts)
			return NULL;

		refcount_set(&bts->users, 1);
		bts->payld = payld;
		bts->xa_bts = xa_bts;
		mutex_init(&bts->mtx);
		ret = xa_insert(xa_bts, (unsigned long)payld, bts, GFP_KERNEL);
		if (ret) {
			devm_kfree(dev, bts);
			return NULL;
		}
	}

	return bts;
}

static struct telemetry_block_ts *
scmi_telemetry_blkts_bind(struct device *dev, struct telemetry_shmti *shmti,
			  struct payload *payld, struct xarray *xa_bts)
{
	struct telemetry_block_ts *bts;
	struct payload *bts_payld;

	/* Find the BLK_TS immediately preceding this DE payld */
	bts_payld = scmi_telemetry_nearest_blk_ts(shmti, payld);
	if (!bts_payld)
		return NULL;

	bts = scmi_telemetry_blkts_get(xa_bts, bts_payld);
	if (bts)
		return bts;

	return scmi_telemetry_blkts_lookup(dev, xa_bts, payld);
}

static void scmi_telemetry_tdcf_blkts_parse(struct telemetry_info *ti,
					    struct payload __iomem *payld,
					    struct telemetry_shmti *shmti)
{
	struct telemetry_block_ts *bts;

	/* Check for spec compliance */
	if (USE_LINE_TS(payld) || USE_BLK_TS(payld) ||
	    DATA_INVALID(payld) || (PAYLD_ID(payld) != 0))
		return;

	/* A BLK_TS descriptor MUST be returned: it is found or it is crated */
	bts = scmi_telemetry_blkts_lookup(ti->ph->dev, &ti->xa_bts, payld);
	if (WARN_ON(!bts))
		return;

	/* Update the descriptor with the lastest TS*/
	scmi_telemetry_blkts_update(shmti->last_magic, bts);
}

static void scmi_telemetry_tdcf_data_parse(struct telemetry_info *ti,
					   struct payload __iomem *payld,
					   struct telemetry_shmti *shmti,
					   enum scan_mode mode)
{
	bool ts_valid = TS_VALID(payld);
	struct telemetry_de *tde;
	bool discovered = false;
	u64 val, tstamp = 0;
	u32 de_id;

	de_id = PAYLD_ID(payld);
	/* Is thi DE ID know ? */
	tde = scmi_telemetry_tde_lookup(ti, de_id);
	if (!tde) {
		if (mode != SCAN_DISCOVERY)
			return;

		/* In SCAN_DISCOVERY mode we allocate new DEs for unknown IDs */
		tde = scmi_telemetry_tde_get(ti, de_id);
		if (IS_ERR(tde))
			return;

		tde->de.info->id = de_id;
		tde->de.enabled = true;
		tde->de.tstamp_enabled = ts_valid;
		discovered = true;
	}

	/* Update DE location refs if requested: normally done only on enable */
	if (mode >= SCAN_UPDATE) {
		tde->base = shmti->base;
		tde->eplg = SHMTI_EPLG(shmti);
		tde->offset = (void *)payld - (void *)shmti->base;

		dev_dbg(ti->ph->dev,
			"TDCF-updated DE_ID:0x%08X - shmti:%pX  offset:%u\n",
			tde->de.info->id, tde->base, tde->offset);
	}

	if (discovered) {
		if (scmi_telemetry_tde_register(ti, tde)) {
			scmi_telemetry_free_tde_put(ti, tde);
			return;
		}
	}

	scoped_guard(mutex, &tde->mtx) {
		if (tde->last_magic == shmti->last_magic)
			return;
	}

	/* Data is always valid since we are NOT handling BLK TS lines here */
	val = LINE_DATA_GET(&payld->l);
	/* Collect the right TS */
	if (ts_valid) {
		if (USE_LINE_TS(payld)) {
			tstamp = LINE_TSTAMP_GET(&payld->tsl);
		} else if (USE_BLK_TS(payld)) {
			if (!tde->bts) {
				/*
				 * Scanning a TDCF looking for the nearest
				 * previous valid BLK_TS, after having found a
				 * USE_BLK_TS() payload, MUST succeed.
				 */
				tde->bts = scmi_telemetry_blkts_bind(ti->ph->dev,
								     shmti, payld,
								     &ti->xa_bts);
				if (WARN_ON(!tde->bts))
					return;
			}

			tstamp = scmi_telemetry_blkts_read(tde->last_magic,
							   tde->bts);
		}
	}

	guard(mutex)(&tde->mtx);
	tde->last_magic = shmti->last_magic;
	tde->last_val = val;
	if (tde->de.tstamp_enabled)
		tde->last_ts = tstamp;
	else
		tde->last_ts = 0;
}

static int scmi_telemetry_tdcf_line_parse(struct telemetry_info *ti,
					  struct payload __iomem *payld,
					  struct telemetry_shmti *shmti,
					  enum scan_mode mode)
{
	int used_qwords;

	used_qwords = (USE_LINE_TS(payld) && TS_VALID(payld)) ?
		QWORDS_TS_LINE_DATA_PAYLD : QWORDS_LINE_DATA_PAYLD;

	/* Invalid lines are not an error, could simply be disabled DEs */
	if (DATA_INVALID(payld))
		return used_qwords;

	if (!IS_BLK_TS(payld))
		scmi_telemetry_tdcf_data_parse(ti, payld, shmti, mode);
	else
		scmi_telemetry_tdcf_blkts_parse(ti, payld, shmti);

	return used_qwords;
}

static int scmi_telemetry_shmti_scan(struct telemetry_info *ti,
				     unsigned int shmti_id, u64 ts,
				     enum scan_mode mode)
{
	struct telemetry_shmti *shmti = &ti->shmti[shmti_id];
	struct tdcf __iomem *tdcf = shmti->base;
	int retries = SCMI_TLM_TDCF_MAX_RETRIES;
	u64 startm = 0, endm = TDCF_BAD_END_SEQ;
	void *eplg = SHMTI_EPLG(shmti);

	if (!tdcf)
		return -ENODEV;

	do {
		unsigned int qwords;
		void __iomem *next;

		/* A bit of exponential backoff between retries */
		fsleep((SCMI_TLM_TDCF_MAX_RETRIES - retries) * 1000);

		startm = TDCF_START_SEQ_GET(tdcf);
		if (IS_BAD_START_SEQ(startm))
			continue;

		/* On a BAD_SEQ this will be updated on the next attempt */
		shmti->last_magic = startm;

		qwords = QWORDS(tdcf);
		next = tdcf->payld;
		while (qwords) {
			int used_qwords;

			used_qwords = scmi_telemetry_tdcf_line_parse(ti, next,
								     shmti, mode);
			if (qwords < used_qwords)
				return -EINVAL;

			next += used_qwords * 8;
			qwords -= used_qwords;
		}

		endm = TDCF_END_SEQ_GET(eplg);
	} while (startm != endm && --retries);

	if (startm != endm)
		return -EPROTO;

	return 0;
}

static int scmi_telemetry_group_state_update(struct telemetry_info *ti,
					     struct scmi_telemetry_group *grp,
					     bool *enable, bool *tstamp)
{
	struct scmi_telemetry_res_info *rinfo;

	rinfo = ti->res_get(ti);
	for (int i = 0; i < grp->info->num_des; i++) {
		struct scmi_telemetry_de *de = rinfo->des[grp->des[i]];

		if (enable)
			de->enabled = *enable;
		if (tstamp)
			de->tstamp_enabled = *tstamp;
	}

	return 0;
}

static int
scmi_telemetry_state_set_resp_process(struct telemetry_info *ti,
				      struct scmi_telemetry_de *de,
				      void *r, bool is_group)
{
	struct scmi_msg_resp_telemetry_de_configure *resp = r;
	u32 sid = le32_to_cpu(resp->shmti_id);

	/* Update DE SHMTI and offset, if applicable */
	if (IS_SHMTI_ID_VALID(sid)) {
		if (sid >= ti->num_shmti)
			return -EPROTO;

		/*
		 * Update SHMTI/offset while skipping non-SHMTI-DEs like
		 * FCs and notif-only.
		 */
		if (!is_group) {
			struct telemetry_de *tde;
			struct payload *payld;
			u32 offs;

			offs = le32_to_cpu(resp->tdcf_de_offset);
			if (offs >= ti->shmti[sid].len - de->info->data_sz)
				return -EPROTO;

			tde = to_tde(de);
			tde->base = ti->shmti[sid].base;
			tde->offset = offs;
			/* A handy reference to the Epilogue updated */
			tde->eplg = SHMTI_EPLG(&ti->shmti[sid]);

			payld = tde->base + tde->offset;
			if (USE_BLK_TS(payld) && !tde->bts) {
				tde->bts = scmi_telemetry_blkts_bind(ti->ph->dev,
								     &ti->shmti[sid],
								     payld,
								     &ti->xa_bts);
				if (WARN_ON(!tde->bts))
					return -EPROTO;
			}
		} else {
			int ret;

			/*
			 * A full SHMTI scan is needed when enabling a
			 * group or its timestamps in order to retrieve
			 * offsets: node that when group-timestamp is
			 * enabled for composing DEs a re-scan is needed
			 * since some DEs could have been relocated due
			 * to lack of space in the TDCF.
			 */
			ret = scmi_telemetry_shmti_scan(ti, sid, 0, SCAN_UPDATE);
			if (ret)
				dev_warn(ti->ph->dev,
					 "Failed group-scan of SHMTI ID:%d\n", sid);
		}
	} else if (!is_group) {
		struct telemetry_de *tde;

		tde = to_tde(de);
		if (tde->bts) {
			/* Unlink the related BLK_TS on disable */
			scmi_telemetry_blkts_put(ti->ph->dev, tde->bts);
			tde->bts = NULL;
		}
	}

	return 0;
}

static int __scmi_telemetry_state_set(const struct scmi_protocol_handle *ph,
				      bool is_group, bool *enable,
				      bool *enabled_state, bool *tstamp,
				      bool *tstamp_enabled_state, void *obj)
{
	struct scmi_msg_resp_telemetry_de_configure *resp;
	struct scmi_msg_telemetry_de_configure *msg;
	struct telemetry_info *ti = ph->get_priv(ph);
	struct scmi_telemetry_group *grp;
	struct scmi_telemetry_de *de;
	unsigned int obj_id;
	struct scmi_xfer *t;
	int ret;

	if (!enabled_state || !tstamp_enabled_state)
		return -EINVAL;

	/* Is anything to do at all on this DE ? */
	if (!is_group && (!enable || *enable == *enabled_state) &&
	    (!tstamp || *tstamp == *tstamp_enabled_state))
		return 0;

	/*
	 * DE is currently disabled AND no enable state change was requested,
	 * while timestamp is being changed: update only local state...no need
	 * to send a message.
	 */
	if (!is_group && !enable && !*enabled_state) {
		*tstamp_enabled_state = *tstamp;
		return 0;
	}

	if (!is_group) {
		de = obj;
		obj_id = de->info->id;
	} else {
		grp = obj;
		obj_id = grp->info->id;
	}

	ret = ph->xops->xfer_get_init(ph, TELEMETRY_DE_CONFIGURE,
				      sizeof(*msg), sizeof(*resp), &t);
	if (ret)
		return ret;

	msg = t->tx.buf;
	/* Note that BOTH DE and GROUPS have a first ID field.. */
	msg->id = cpu_to_le32(obj_id);
	/* Default to disable mode for one DE */
	msg->flags = DE_DISABLE_ONE;
	msg->flags |= FIELD_PREP(GENMASK(3, 3),
				 is_group ? EVENT_GROUP : EVENT_DE);

	if ((!enable && *enabled_state) || (enable && *enable)) {
		/* Already enabled but tstamp_enabled state changed */
		if (tstamp) {
			/* Here, tstamp cannot be NULL too */
			msg->flags |= *tstamp ? DE_ENABLE_WTH_TSTAMP :
				DE_ENABLE_NO_TSTAMP;
		} else {
			msg->flags |= *tstamp_enabled_state ?
				DE_ENABLE_WTH_TSTAMP : DE_ENABLE_NO_TSTAMP;
		}
	}

	resp = t->rx.buf;
	ret = ph->xops->do_xfer(ph, t);
	if (!ret) {
		ret = scmi_telemetry_state_set_resp_process(ti, de, resp, is_group);
		if (!ret) {
			/* Update cached state on success */
			if (enable)
				*enabled_state = *enable;
			if (tstamp)
				*tstamp_enabled_state = *tstamp;

			if (is_group)
				scmi_telemetry_group_state_update(ti, grp, enable,
								  tstamp);
		}
	}

	ph->xops->xfer_put(ph, t);

	return ret;
}

static int scmi_telemetry_state_get(const struct scmi_protocol_handle *ph,
				    u32 id, bool *enabled, bool *tstamp_enabled)
{
	struct telemetry_info *ti = ph->get_priv(ph);
	struct scmi_telemetry_de *de;

	if (!enabled || !tstamp_enabled)
		return -EINVAL;

	de = xa_load(&ti->xa_des, id);
	if (!de)
		return -ENODEV;

	*enabled = de->enabled;
	*tstamp_enabled = de->tstamp_enabled;

	return 0;
}

static int scmi_telemetry_state_set(const struct scmi_protocol_handle *ph,
				    bool is_group, u32 id, bool *enable,
				    bool *tstamp)
{
	struct telemetry_info *ti = ph->get_priv(ph);
	bool *enabled_state, *tstamp_enabled_state;
	struct scmi_telemetry_res_info *rinfo;
	void *obj;

	rinfo = ti->res_get(ti);
	if (!is_group) {
		struct scmi_telemetry_de *de;

		de = xa_load(&ti->xa_des, id);
		if (!de)
			return -ENODEV;

		enabled_state = &de->enabled;
		tstamp_enabled_state = &de->tstamp_enabled;
		obj = de;
	} else {
		struct scmi_telemetry_group *grp;

		if (id >= ti->info.base.num_groups)
			return -EINVAL;

		grp = &rinfo->grps[id];

		enabled_state = &grp->enabled;
		tstamp_enabled_state = &grp->tstamp_enabled;
		obj = grp;
	}

	return __scmi_telemetry_state_set(ph, is_group, enable, enabled_state,
					  tstamp, tstamp_enabled_state, obj);
}

static int scmi_telemetry_all_disable(const struct scmi_protocol_handle *ph,
				      bool is_group)
{
	struct telemetry_info *ti = ph->get_priv(ph);
	struct scmi_msg_telemetry_de_configure *msg;
	struct scmi_telemetry_res_info *rinfo;
	struct scmi_xfer *t;
	int ret;

	rinfo = ti->res_get(ti);
	ret = ph->xops->xfer_get_init(ph, TELEMETRY_DE_CONFIGURE,
				      sizeof(*msg), 0, &t);
	if (ret)
		return ret;

	msg = t->tx.buf;
	msg->flags = DE_DISABLE_ALL;
	if (is_group)
		msg->flags |= GROUP_SELECTOR;
	ret = ph->xops->do_xfer(ph, t);
	if (!ret) {
		for (int i = 0; i < ti->info.base.num_des; i++)
			rinfo->des[i]->enabled = false;

		if (is_group) {
			for (int i = 0; i < ti->info.base.num_groups; i++)
				rinfo->grps[i].enabled = false;
		}
	}

	ph->xops->xfer_put(ph, t);

	return ret;
}

static int
scmi_telemetry_collection_configure(const struct scmi_protocol_handle *ph,
				    unsigned int res_id, bool grp_ignore,
				    bool *enable,
				    unsigned int *update_interval_ms,
				    enum scmi_telemetry_collection *mode)
{
	enum scmi_telemetry_collection *current_mode, next_mode;
	struct telemetry_info *ti = ph->get_priv(ph);
	struct scmi_msg_telemetry_config_set *msg;
	unsigned int *active_update_interval;
	struct scmi_xfer *t;
	bool tlm_enable;
	u32 interval;
	int ret;

	if (mode && *mode == SCMI_TLM_NOTIFICATION &&
	    !ti->info.continuos_update_support)
		return -EINVAL;

	if (res_id != SCMI_TLM_GRP_INVALID && res_id >= ti->info.base.num_groups)
		return -EINVAL;

	if (res_id == SCMI_TLM_GRP_INVALID || grp_ignore) {
		active_update_interval = &ti->info.active_update_interval;
		current_mode = &ti->info.current_mode;
	} else {
		struct scmi_telemetry_res_info *rinfo;

		rinfo = ti->res_get(ti);
		active_update_interval =
			&rinfo->grps[res_id].active_update_interval;
		current_mode = &rinfo->grps[res_id].current_mode;
	}

	if (!enable && !update_interval_ms && (!mode || *mode == *current_mode))
		return 0;

	ret = ph->xops->xfer_get_init(ph, TELEMETRY_CONFIG_SET,
				      sizeof(*msg), 0, &t);
	if (ret)
		return ret;

	if (!update_interval_ms)
		interval = cpu_to_le32(*active_update_interval);
	else
		interval = *update_interval_ms;

	tlm_enable = enable ? *enable : ti->info.enabled;
	next_mode = mode ? *mode : *current_mode;

	msg = t->tx.buf;
	msg->grp_id = res_id;
	msg->control = tlm_enable ? TELEMETRY_ENABLE : 0;
	msg->control |= grp_ignore ? TELEMETRY_SET_SELECTOR_ALL :
		TELEMETRY_SET_SELECTOR_GROUP;
	msg->control |= TELEMETRY_MODE_SET(next_mode);
	msg->sampling_rate = interval;
	ret = ph->xops->do_xfer(ph, t);
	if (!ret) {
		ti->info.enabled = tlm_enable;
		*current_mode = next_mode;
		ti->info.notif_enabled = *current_mode == SCMI_TLM_NOTIFICATION;
		if (update_interval_ms)
			*active_update_interval = interval;
	}

	ph->xops->xfer_put(ph, t);

	return ret;
}

static inline void scmi_telemetry_de_data_fc_read(struct telemetry_de *tde,
						  u64 *tstamp, u64 *val)
{
	struct fc_tsline __iomem *fc = tde->base + tde->offset;

	*val = LINE_DATA_GET(fc);
	if (tstamp) {
		if (tde->de.tstamp_support)
			*tstamp = LINE_TSTAMP_GET(fc);
		else
			*tstamp = 0;
	}
}

static void scmi_telemetry_scan_update(struct telemetry_info *ti, u64 ts)
{
	struct telemetry_de *tde;

	/* Scan all SHMTIs ... */
	for (int id = 0; id < ti->num_shmti; id++) {
		int ret;

		ret = scmi_telemetry_shmti_scan(ti, id, ts, SCAN_LOOKUP);
		if (ret)
			dev_warn(ti->ph->dev,
				 "Failed update-scan of SHMTI ID:%d\n", id);
	}

	if (!ti->info.fc_support)
		return;

	/* Need to enumerate resources to access fastchannels */
	ti->res_get(ti);
	list_for_each_entry(tde, &ti->fcs_des, item) {
		u64 val, tstamp;

		if (!tde->de.enabled)
			continue;

		scmi_telemetry_de_data_fc_read(tde, &tstamp, &val);

		guard(mutex)(&tde->mtx);
		tde->last_val = val;
		if (tde->de.tstamp_enabled)
			tde->last_ts = tstamp;
		else
			tde->last_ts = 0;
	}
}

/*
 * TDCF and TS Line Management Notes
 * ---------------------------------
 *  (from a chat with ATG)
 *
 * TCDF Payload Metadata notable bits:
 *  - Bit[3]: USE BLK Tstamp
 *  - Bit[2]: USE LINE Tstamp
 *  - Bit[1]: Tstamp VALID
 *
 * CASE_1:
 * -------
 *	+ A DE is enabled with timestamp disabled, so the TS fields are
 *	  NOT present
 *	  -> BIT[3:1] = 000b
 *
 *	  - 1/A LINE_TSTAMP
 *	  ------------------
 *	     + that DE is then 're-enabled' with TS: so it was ON, it remains
 *	       ON but using DE_CONFIGURE I now enabled also TS, so the
 *	       platform relocates it at the end of the SHMTI and return the
 *	       new offset
 *	       -> BIT[3:1] = 011b
 *
 *	  - 1/B BLK_TSTAMP
 *	  ------------------
 *	     + that DE is then 're-enabled' with BLK TS: so it was ON, it
 *	       remains ON but using DE_CONFIGURE, we now also enabled the TS,
 *	       so the platform will:
 *	       - IF a preceding BLK_TS line exist (with same clock freq)
 *	         it relocates the DE at the end of the SHMTI and return the
 *	         new offset (if there is enough room, if not in another SHMTI)
 *	       - IF a preceding BLK_TS line DOES NOT exist (same clock freq)
 *	         it creates a new BLK_TS line at the end of the SHMTI and then
 *	         relocates the DE after the new BLK_TS and return the
 *	         new offset (if there is enough room, if not in another SHMTI)
 *	       -> BIT[3:1] = 101b
 *
 *	+ the hole left from the relocated DE can be reused by the platform
 *	to fit another equally sized DE. (i.e. without shuffling around any
 *	other enabled DE, since that would cause a change of the known offset)
 *
 * CASE_2:
 * -------
 *	+ A DE is enabled with LINE timestamp enabled, so the TS_Line is there
 *	  -> BIT[3:1] = 011b
 *	+ that DE has its timestamp disabled: again, you can do this without
 *	  disabling it fully but just disabling the TS, so now that TS_line
 *	  fields are still physiclly there but NOT valid
 *	  -> BIT[3:1] = 010b
 *	+ the hole from the timestamp remain there unused until
 *		- you enable again the TS so the hole is used again
 *		  -> BIT[3:1] = 011b
 *			OR
 *		- you disable fully the DE and then re-enable it with the TS
 *		  -> potentially CASE_1 the DE is relocated on enable
 *	+ same kind of dynamic applies if the DE had a BLK_TS line
 */
static int scmi_telemetry_tdcf_de_parse(struct telemetry_de *tde,
					u64 *tstamp, u64 *val)
{
	struct tdcf __iomem *tdcf = tde->base;
	u64 startm, endm;
	int retries = SCMI_TLM_TDCF_MAX_RETRIES;

	if (!tdcf)
		return -ENODEV;

	do {
		struct payload __iomem *payld;

		/* A bit of exponential backoff between retries */
		fsleep((SCMI_TLM_TDCF_MAX_RETRIES - retries) * 1000);

		startm = TDCF_START_SEQ_GET(tdcf);
		if (IS_BAD_START_SEQ(startm))
			continue;

		/* Has anything changed at all at the SHMTI level ? */
		scoped_guard(mutex, &tde->mtx) {
			if (tde->last_magic == startm) {
				*val = tde->last_val;
				if (tstamp)
					*tstamp = tde->last_ts;
				return 0;
			}
		}

		payld = tde->base + tde->offset;
		if (DATA_INVALID(payld))
			return -EINVAL;

		if (IS_BLK_TS(payld))
			return -EINVAL;

		if (PAYLD_ID(payld) != tde->de.info->id)
			return -EINVAL;

		/* Data is always valid since NOT handling BLK TS lines here */
		*val = LINE_DATA_GET(&payld->l);
		/* Collect the right TS */
		if (tstamp) {
			if (!TS_VALID(payld)) {
				*tstamp = 0;
			} else if (USE_LINE_TS(payld)) {
				*tstamp = LINE_TSTAMP_GET(&payld->tsl);
			} else if (USE_BLK_TS(payld)) {
				/*
				 * A valid line using BLK_TS should have been
				 * initialized with the related BLK_TS when
				 * enabled.
				 */
				if (WARN_ON(!tde->bts))
					return -EPROTO;
				*tstamp = scmi_telemetry_blkts_read(startm,
								    tde->bts);
			}
		}

		endm = TDCF_END_SEQ_GET(tde->eplg);
	} while (startm != endm && --retries);

	if (startm != endm)
		return -EPROTO;

	guard(mutex)(&tde->mtx);
	tde->last_magic = startm;
	tde->last_val = *val;
	if (tstamp)
		tde->last_ts = *tstamp;

	return 0;
}

static int scmi_telemetry_de_read(struct telemetry_de *tde, u64 *tstamp, u64 *val)
{
	if (!tde->de.fc_support)
		return scmi_telemetry_tdcf_de_parse(tde, tstamp, val);

	scmi_telemetry_de_data_fc_read(tde, tstamp, val);

	return 0;
}

static int scmi_telemetry_de_collect(struct telemetry_info *ti,
				     struct scmi_telemetry_de *de,
				     u64 *tstamp, u64 *val)
{
	struct telemetry_de *tde = to_tde(de);

	if (!de->enabled)
		return -EINVAL;

	/*
	 * DE readings returns cached values when:
	 *  - DE data value was retrieved via notification
	 */
	scoped_guard(mutex, &tde->mtx) {
		if (tde->cached) {
			*val = tde->last_val;
			if (tstamp)
				*tstamp = tde->last_ts;
			return 0;
		}
	}

	return scmi_telemetry_de_read(tde, tstamp, val);
}

static int scmi_telemetry_de_cached_read(struct telemetry_info *ti,
					 struct scmi_telemetry_de *de,
					 u64 *tstamp, u64 *val)
{
	struct telemetry_de *tde = to_tde(de);

	if (!de->enabled)
		return -EINVAL;

	guard(mutex)(&tde->mtx);
	*val = tde->last_val;
	if (tstamp)
		*tstamp = tde->last_ts;

	return 0;
}

static int scmi_telemetry_de_data_read(const struct scmi_protocol_handle *ph,
				       struct scmi_telemetry_de_sample *sample)
{
	struct telemetry_info *ti = ph->get_priv(ph);
	struct scmi_telemetry_de *de;

	if (!ti->info.enabled || !sample)
		return -EINVAL;

	de = xa_load(&ti->xa_des, sample->id);
	if (!de)
		return -ENODEV;

	return scmi_telemetry_de_collect(ti, de, &sample->tstamp, &sample->val);
}

static int
scmi_telemetry_samples_collect(struct telemetry_info *ti, int grp_id,
			       int *num_samples,
			       struct scmi_telemetry_de_sample *samples)
{
	struct scmi_telemetry_res_info *rinfo;
	int max_samples;

	max_samples = *num_samples;
	*num_samples = 0;

	rinfo = ti->res_get(ti);
	for (int i = 0; i < rinfo->num_des; i++) {
		struct scmi_telemetry_de *de;
		u64 val, tstamp;
		int ret;

		de = rinfo->des[i];
		if (grp_id != SCMI_TLM_GRP_INVALID &&
		    (!de->grp || de->grp->info->id != grp_id))
			continue;

		ret = scmi_telemetry_de_cached_read(ti, de, &tstamp, &val);
		if (ret)
			continue;

		if (*num_samples == max_samples)
			return -ENOSPC;

		samples[*num_samples].tstamp = tstamp;
		samples[*num_samples].val = val;
		samples[*num_samples].id = de->info->id;

		(*num_samples)++;
	}

	return 0;
}

static int scmi_telemetry_des_bulk_read(const struct scmi_protocol_handle *ph,
					int grp_id, int *num_samples,
					struct scmi_telemetry_de_sample *samples)
{
	struct telemetry_info *ti = ph->get_priv(ph);

	if (!ti->info.enabled || !num_samples || !samples)
		return -EINVAL;

	/* Trigger a full SHMTIs & FCs scan */
	scmi_telemetry_scan_update(ti, 0);

	/* Collect all last cached values */
	return scmi_telemetry_samples_collect(ti, grp_id, num_samples, samples);
}

static void
scmi_telemetry_msg_payld_process(struct telemetry_info *ti,
				 unsigned int num_dwords, u32 *dwords,
				 ktime_t timestamp)
{
	struct scmi_telemetry_res_info *rinfo;
	u32 next = 0;

	rinfo = ti->res_get(ti);
	if (!rinfo->fully_enumerated) {
		dev_warn_once(ti->ph->dev,
			      "Cannot process DEs in message payload. Drop.\n");
		return;
	}

	while (next < num_dwords) {
		struct payload *payld = (struct payload *)&dwords[next];
		struct scmi_telemetry_de *de;
		struct telemetry_de *tde;
		u32 de_id;

		next += USE_LINE_TS(payld) ?
			TS_LINE_DATA_PAYLD_WORDS : LINE_DATA_PAYLD_WORDS;

		if (DATA_INVALID(payld)) {
			dev_err(ti->ph->dev, "MSG - Received INVALID DATA line\n");
			continue;
		}

		de_id = le32_to_cpu(payld->id);
		de = xa_load(&ti->xa_des, de_id);
		if (!de || !de->enabled) {
			dev_err(ti->ph->dev,
				"MSG - Received INVALID DE - ID:%u  enabled:%d\n",
				de_id, de ? (de->enabled ? 'Y' : 'N') : 'X');
			continue;
		}

		tde = to_tde(de);
		guard(mutex)(&tde->mtx);
		tde->cached = true;
		tde->last_val = LINE_DATA_GET(&payld->tsl);
		/* TODO BLK_TS in notification payloads */
		if (USE_LINE_TS(payld) && TS_VALID(payld))
			tde->last_ts = LINE_TSTAMP_GET(&payld->tsl);
		else
			tde->last_ts = 0;
	}
}

static int scmi_telemetry_des_sample_get(const struct scmi_protocol_handle *ph,
					 int grp_id, int *num_samples,
					 struct scmi_telemetry_de_sample *samples)
{
	struct telemetry_info *ti = ph->get_priv(ph);
	struct scmi_msg_telemetry_config_set *msg;
	struct scmi_xfer *t;
	bool grp_ignore;
	int ret;

	if (!ti->info.enabled || !num_samples || !samples)
		return -EINVAL;

	grp_ignore = grp_id == SCMI_TLM_GRP_INVALID ? true : false;
	if (!grp_ignore && grp_id >= ti->info.base.num_groups)
		return -EINVAL;

	ret = ph->xops->xfer_get_init(ph, TELEMETRY_CONFIG_SET,
				      sizeof(*msg), 0, &t);
	if (ret)
		return ret;

	msg = t->tx.buf;
	msg->grp_id = grp_id;
	msg->control = TELEMETRY_ENABLE;
	msg->control |= grp_ignore ? TELEMETRY_SET_SELECTOR_ALL :
		TELEMETRY_SET_SELECTOR_GROUP;
	msg->control |= TELEMETRY_MODE_SINGLE;
	msg->sampling_rate = 0;

	ret = ph->xops->do_xfer_with_response(ph, t);
	if (!ret) {
		struct scmi_msg_resp_telemetry_reading_complete *r = t->rx.buf;

		/* Update cached DEs values from payload */
		if (r->num_dwords)
			scmi_telemetry_msg_payld_process(ti, r->num_dwords,
							 r->dwords, 0);
		/* Scan and update SMHTIs and FCs */
		scmi_telemetry_scan_update(ti, 0);

		/* Collect all last cached values */
		ret = scmi_telemetry_samples_collect(ti, grp_id, num_samples,
						     samples);
	}

	ph->xops->xfer_put(ph, t);

	return ret;
}

static int scmi_telemetry_config_get(const struct scmi_protocol_handle *ph,
				     bool *enabled, int *mode,
				     u32 *update_interval)
{
	return -EOPNOTSUPP;
}

static void scmi_telemetry_local_resources_reset(struct telemetry_info *ti)
{
	struct scmi_telemetry_res_info *rinfo;

	/* Get rinfo as it is...without triggering an enumeration */
	rinfo = __scmi_telemetry_resources_get(ti);
	/* Clear all local state...*/
	for (int i = 0; i < rinfo->num_des; i++) {
		rinfo->des[i]->enabled = false;
		rinfo->des[i]->tstamp_enabled = false;
	}
	for (int i = 0; i < rinfo->num_groups; i++) {
		rinfo->grps[i].enabled = false;
		rinfo->grps[i].tstamp_enabled = false;
		rinfo->grps[i].current_mode = SCMI_TLM_ONDEMAND;
		rinfo->grps[i].active_update_interval = 0;
	}
}

static int scmi_telemetry_reset(const struct scmi_protocol_handle *ph)
{
	struct scmi_xfer *t;
	int ret;

	ret = ph->xops->xfer_get_init(ph, TELEMETRY_RESET, sizeof(u32), 0, &t);
	if (ret)
		return ret;

	put_unaligned_le32(0, t->tx.buf);
	ret = ph->xops->do_xfer(ph, t);
	if (!ret) {
		struct telemetry_info *ti = ph->get_priv(ph);

		scmi_telemetry_local_resources_reset(ti);
		/* Fetch agaon states state from platform.*/
		ret = scmi_telemetry_initial_state_lookup(ti);
		if (ret)
			dev_warn(ph->dev,
				 FW_BUG "Cannot retrieve initial state after reset.\n");
	}

	ph->xops->xfer_put(ph, t);

	return ret;
}

static const struct scmi_telemetry_proto_ops tlm_proto_ops = {
	.info_get = scmi_telemetry_info_get,
	.de_lookup = scmi_telemetry_de_lookup,
	.res_get = scmi_telemetry_resources_get,
	.state_get = scmi_telemetry_state_get,
	.state_set = scmi_telemetry_state_set,
	.all_disable = scmi_telemetry_all_disable,
	.collection_configure = scmi_telemetry_collection_configure,
	.de_data_read = scmi_telemetry_de_data_read,
	.des_bulk_read = scmi_telemetry_des_bulk_read,
	.des_sample_get = scmi_telemetry_des_sample_get,
	.config_get = scmi_telemetry_config_get,
	.reset = scmi_telemetry_reset,
};

static bool
scmi_telemetry_notify_supported(const struct scmi_protocol_handle *ph,
				u8 evt_id, u32 src_id)
{
	struct telemetry_info *ti = ph->get_priv(ph);

	return ti->info.continuos_update_support;
}

static int
scmi_telemetry_set_notify_enabled(const struct scmi_protocol_handle *ph,
				  u8 evt_id, u32 src_id, bool enable)
{
	return 0;
}

static void *
scmi_telemetry_fill_custom_report(const struct scmi_protocol_handle *ph,
				  u8 evt_id, ktime_t timestamp,
				  const void *payld, size_t payld_sz,
				  void *report, u32 *src_id)
{
	const struct scmi_telemetry_update_notify_payld *p = payld;
	struct scmi_telemetry_update_report *r = report;

	/* At least sized as an empty notification */
	if (payld_sz < sizeof(*p))
		return NULL;

	r->timestamp = timestamp;
	r->agent_id = le32_to_cpu(p->agent_id);
	r->status = le32_to_cpu(p->status);
	r->num_dwords = le32_to_cpu(p->num_dwords);
	/*
	 * Allocated dwords and report are sized as max_msg_size, so as
	 * to allow for the maximum payload permitted by the configured
	 * transport. Overflow is not possible since out-of-size messages
	 * are dropped at the transport layer.
	 */
	if (r->num_dwords)
		memcpy(r->dwords, p->array, r->num_dwords * sizeof(u32));

	*src_id = 0;

	return r;
}

static const struct scmi_event tlm_events[] = {
	{
		.id = SCMI_EVENT_TELEMETRY_UPDATE,
		.max_payld_sz = 0,
		.max_report_sz = 0,
	},
};

static const struct scmi_event_ops tlm_event_ops = {
	.is_notify_supported = scmi_telemetry_notify_supported,
	.set_notify_enabled = scmi_telemetry_set_notify_enabled,
	.fill_custom_report = scmi_telemetry_fill_custom_report,
};

static const struct scmi_protocol_events tlm_protocol_events = {
	.queue_sz = SCMI_PROTO_QUEUE_SZ,
	.ops = &tlm_event_ops,
	.evts = tlm_events,
	.num_events = ARRAY_SIZE(tlm_events),
	.num_sources = 1,
};

static int scmi_telemetry_notifier(struct notifier_block *nb,
				   unsigned long event, void *data)
{
	struct scmi_telemetry_update_report *er = data;
	struct telemetry_info *ti = telemetry_nb_to_info(nb);

	if (er->status) {
		dev_err(ti->ph->dev, "Bad Telemetry update notification - ret: %dn",
			er->status);
		return NOTIFY_DONE;
	}

	/* Lookup the embedded DEs in the notification payload ... */
	if (er->num_dwords)
		scmi_telemetry_msg_payld_process(ti, er->num_dwords,
						 er->dwords, er->timestamp);

	/* ...scan the SHMTI/FCs for any other DE updates. */
	if (ti->streaming_mode)
		scmi_telemetry_scan_update(ti, er->timestamp);

	return NOTIFY_OK;
}

static int scmi_telemetry_resources_alloc(struct telemetry_info *ti)
{
	/* Array to hold pointers to discovered DEs */
	struct scmi_telemetry_de **des __free(kfree) =
		kcalloc(ti->info.base.num_des, sizeof(*des), GFP_KERNEL);
	if (!des)
		return -ENOMEM;

	/* The allocated DE descriptors */
	struct telemetry_de *tdes __free(kfree) =
		kcalloc(ti->info.base.num_des, sizeof(*tdes), GFP_KERNEL);
	if (!tdes)
		return -ENOMEM;

	/* Allocate a set of contiguous DE info descriptors. */
	struct scmi_tlm_de_info *dei_store __free(kfree) =
		kcalloc(ti->info.base.num_des, sizeof(*dei_store), GFP_KERNEL);
	if (!dei_store)
		return -ENOMEM;

	/* Array to hold descriptors of discovered GROUPs */
	struct scmi_telemetry_group *grps __free(kfree) =
		kcalloc(ti->info.base.num_groups, sizeof(*grps), GFP_KERNEL);
	if (!grps)
		return -ENOMEM;

	/* Allocate a set of contiguous Group info descriptors. */
	struct scmi_tlm_grp_info *grps_store __free(kfree) =
		kcalloc(ti->info.base.num_groups, sizeof(*grps_store), GFP_KERNEL);
	if (!grps_store)
		return -ENOMEM;

	struct scmi_telemetry_res_info *rinfo __free(kfree) =
		kzalloc(sizeof(*rinfo), GFP_KERNEL);
	if (!rinfo)
		return -ENOMEM;

	mutex_init(&ti->free_mtx);
	INIT_LIST_HEAD(&ti->free_des);
	for (int i = 0; i < ti->info.base.num_des; i++) {
		mutex_init(&tdes[i].mtx);
		/* Bind contiguous DE info structures */
		tdes[i].de.info = &dei_store[i];
		list_add_tail(&tdes[i].item, &ti->free_des);
	}

	for (int i = 0; i < ti->info.base.num_groups; i++) {
		grps_store[i].id = i;
		/* Bind contiguous Group info struct */
		grps[i].info = &grps_store[i];
	}

	INIT_LIST_HEAD(&ti->fcs_des);

	ti->tdes = no_free_ptr(tdes);

	rinfo->des = no_free_ptr(des);
	rinfo->dei_store = no_free_ptr(dei_store);
	rinfo->grps = no_free_ptr(grps);
	rinfo->grps_store = no_free_ptr(grps_store);

	ti->rinfo = no_free_ptr(rinfo);

	return 0;
}

static void scmi_telemetry_resources_free(void *arg)
{
	struct telemetry_info *ti = arg;

	kfree(ti->tdes);
	kfree(ti->rinfo->des);
	kfree(ti->rinfo->dei_store);
	kfree(ti->rinfo->grps);
	kfree(ti->rinfo->grps_store);

	kfree(ti->rinfo);
}

static struct scmi_telemetry_res_info *
__scmi_telemetry_resources_get(struct telemetry_info *ti)
{
	return ACCESS_PRIVATE(ti, rinfo);
}

static struct scmi_telemetry_res_info *
scmi_telemetry_resources_enumerate(struct telemetry_info *ti)
{
	struct device *dev = ti->ph->dev;
	int ret;

	/*
	 * Ensure this init function can be called only once and
	 * handles properly concurrent calls.
	 */
	if (atomic_cmpxchg(&ti->rinfo_initializing, 0, 1)) {
		if (!completion_done(&ti->rinfo_initdone))
			wait_for_completion(&ti->rinfo_initdone);
		goto out;
	}

	ret = scmi_telemetry_de_descriptors_get(ti);
	if (ret) {
		dev_err(dev, FW_BUG "Cannot enumerate DEs resources. Carry-on.\n");
		goto done;
	}

	ret = scmi_telemetry_enumerate_groups_intervals(ti);
	if (ret) {
		dev_err(dev, FW_BUG "Cannot enumerate group intervals. Carry-on.\n");
		goto done;
	}

	ti->rinfo->fully_enumerated = true;
done:
	/* Disable initialization permanently */
	smp_store_mb(ti->res_get, __scmi_telemetry_resources_get);
	complete_all(&ti->rinfo_initdone);

out:
	return ti->rinfo;
}

static int scmi_telemetry_instance_init(struct telemetry_info *ti)
{
	int ret;

	/* Allocate and Initialize on first call... */
	ret = scmi_telemetry_resources_alloc(ti);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(ti->ph->dev,
				       scmi_telemetry_resources_free, ti);
	if (ret)
		return ret;

	xa_init(&ti->xa_des);
	/* Setup resources lazy initialization */
	atomic_set(&ti->rinfo_initializing, 0);
	init_completion(&ti->rinfo_initdone);
	/* Ensure the new res_get() operation is visible after this point */
	smp_store_mb(ti->res_get, scmi_telemetry_resources_enumerate);

	return 0;
}

static int scmi_telemetry_protocol_init(const struct scmi_protocol_handle *ph)
{
	struct device *dev = ph->dev;
	struct telemetry_info *ti;
	u32 version;
	int ret;

	ret = ph->xops->version_get(ph, &version);
	if (ret)
		return ret;

	dev_dbg(dev, "Telemetry Version %d.%d\n",
		PROTOCOL_REV_MAJOR(version), PROTOCOL_REV_MINOR(version));

	ti = devm_kzalloc(dev, sizeof(*ti), GFP_KERNEL);
	if (!ti)
		return -ENOMEM;

	ti->ph = ph;

	ret = scmi_telemetry_protocol_attributes_get(ti);
	if (ret) {
		dev_err(dev, FW_BUG "Cannot retrieve protocol attributes. Abort\n");
		return ret;
	}

	ret = scmi_telemetry_instance_init(ti);
	if (ret) {
		dev_err(dev, "Cannot initialize instance. Abort.\n");
		return ret;
	}

	ret = scmi_telemetry_enumerate_common_intervals(ti);
	if (ret)
		dev_warn(dev, FW_BUG "Cannot enumerate update intervals. Carry-on.\n");

	ret = scmi_telemetry_enumerate_shmti(ti);
	if (ret)
		dev_warn(dev, FW_BUG "Cannot enumerate SHMTIs. Carry-on.\n");

	ret = scmi_telemetry_initial_state_lookup(ti);
	if (ret)
		dev_warn(dev, FW_BUG "Cannot retrieve initial state. Carry-on.\n");

	ti->info.base.version = version;

	ret = ph->set_priv(ph, ti, version);
	if (ret)
		return ret;

	/*
	 * Register a notifier anyway straight upon protocol initialization
	 * since there could be some DEs that are ONLY reported by notifications
	 * even though the chosen collection method was SHMTI/FCs.
	 */
	if (ti->info.continuos_update_support) {
		ti->telemetry_nb.notifier_call = &scmi_telemetry_notifier;
		ret = ph->notifier_register(ph, SCMI_EVENT_TELEMETRY_UPDATE,
					    NULL, &ti->telemetry_nb);
		if (ret)
			dev_warn(ph->dev,
				 "Could NOT register Telemetry notifications\n");
	}

	return ret;
}

static const struct scmi_protocol scmi_telemetry = {
	.id = SCMI_PROTOCOL_TELEMETRY,
	.owner = THIS_MODULE,
	.instance_init = &scmi_telemetry_protocol_init,
	.ops = &tlm_proto_ops,
	.events = &tlm_protocol_events,
	.supported_version = SCMI_PROTOCOL_SUPPORTED_VERSION,
};

DEFINE_SCMI_PROTOCOL_REGISTER_UNREGISTER(telemetry, scmi_telemetry)
