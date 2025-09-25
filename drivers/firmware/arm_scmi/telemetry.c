// SPDX-License-Identifier: GPL-2.0
/*
 * System Control and Management Interface (SCMI) Telemetry Protocol
 *
 * Copyright (C) 2025 ARM Ltd.
 *
 */

#include <linux/bitfield.h>
#include <linux/device.h>
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
	TELEMETRY_DE_ENABLED_LIST = 0x7, //TODO IMPLEMENT
	TELEMETRY_CONFIG_SET = 0x8,
	TELEMETRY_READING_COMPLETE = TELEMETRY_CONFIG_SET,
	TELEMETRY_CONFIG_GET = 0x9, //TODO IMPLEMENT !
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
	__le32 array[];
};

struct scmi_shmti_desc {
	__le32 id;
	__le32 addr_low;
	__le32 addr_high;
	__le32 length;
};

struct scmi_msg_resp_telemetry_shmti_list {
	__le32 num_shmti;
	struct scmi_shmti_desc desc[];
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
		int __signed_exp =					\
			le32_get_bits((d)->attr_1, GENMASK(20, 13));	\
									\
		if (__signed_exp & BIT(7))				\
			__signed_exp |= GENMASK(31, 8);			\
		__signed_exp;						\
	})
#define	GET_DE_UNIT(d)		(le32_get_bits((d)->attr_1, GENMASK(12, 5)))

#define GET_DE_TSTAMP_EXP(d)						\
	({								\
		int __signed_exp =					\
			FIELD_GET(GENMASK(4, 1), (d)->attr_1);		\
									\
		if (__signed_exp & BIT(3))				\
			__signed_exp |= GENMASK(31, 4);			\
		__signed_exp;						\
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
	struct scmi_de_desc desc[];
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

#define TELEMETRY_MODE(x)		(FIELD_PREP(GENMASK(4, 1), (x)))
#define TELEMETRY_MODE_ONDEMAND		TELEMETRY_MODE(0)
#define TELEMETRY_MODE_NOTIFS		TELEMETRY_MODE(1)
#define TELEMETRY_MODE_SINGLE		TELEMETRY_MODE(2)

#define TELEMETRY_SELECTOR(x)		(FIELD_PREP(GENMASK(8, 5), (x)))
#define	TELEMETRY_SELECTOR_ORPHANS	TELEMETRY_SELECTOR(0)
#define	TELEMETRY_SELECTOR_GROUP	TELEMETRY_SELECTOR(1)
#define	TELEMETRY_SELECTOR_ALL		TELEMETRY_SELECTOR(2)
	__le32 sampling_rate;
};

struct scmi_msg_resp_telemetry_reading_complete {
	__le32 num_dwords;
	__le32 dwords[];
};

/* TDCF */

#define TO_CPU_64(h, l)	(((u64)le32_to_cpu((h)) << 32) | le32_to_cpu((l)))

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

#define LINE_DATA_GET(f)				\
({							\
	typeof(f) _f = (f);				\
							\
	(TO_CPU_64((_f)->data_high, (_f)->data_low));	\
})

#define LINE_TSTAMP_GET(f)				\
({							\
	typeof(f) _f = (f);				\
							\
	(TO_CPU_64((_f)->ts_high, (_f)->ts_low));	\
})

#define BLK_TSTAMP_GET(f)	LINE_TSTAMP_GET(f)

struct payload {
	u32 meta;
#define IS_BLK_TS(x)	((x)->meta & BIT(4))
#define USE_BLK_TS(x)	((x)->meta & BIT(3))
#define USE_LINE_TS(x)	((x)->meta & BIT(2))
#define TS_VALID(x)	((x)->meta & BIT(1))
#define	DATA_INVALID(x) ((x)->meta & BIT(0))
	u32 id;
	union {
		struct line l;
		struct tsline tsl;
		struct blk_tsline blk_tsl;
	};
};

#define PAYLD_ID(x)	(le32_to_cpu(((struct payload *)(x))->id))

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

#define SHMTI_MIN_SIZE	(sizeof(struct tdcf) + TDCF_EPLG_SZ)

#define TDCF_START_SEQ_GET(x)					\
	({							\
		u64 _val;					\
		struct prlg *_p = &((x)->prlg);			\
								\
		_val = TO_CPU_64(_p->seq_high, _p->seq_low);	\
		(_val);						\
	})

#define IS_BAD_START_SEQ(s)	((s) & 0x1)

#define	TDCF_END_SEQ_GET(e)					\
	({							\
		u64 _val;					\
		struct eplg *_e = (e);				\
								\
		_val = TO_CPU_64(_e->seq_high, _e->seq_low);	\
		(_val);						\
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

struct telemetry_info {
	bool streaming_mode;
	int num_shmti;
	struct device *dev;
	struct telemetry_shmti *shmti;
	struct xarray xa_des;
	struct xarray xa_bts;
	struct scmi_telemetry_info info;
	struct notifier_block telemetry_nb;
};

#define telemetry_nb_to_info(x)	\
	container_of(x, struct telemetry_info, telemetry_nb)

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
	struct telemetry_block_ts *bts;
	struct scmi_telemetry_de de;
};

#define to_tde(d)	container_of(d, struct telemetry_de, de)

struct scmi_tlm_de_priv {
	struct telemetry_info *ti;
	void *next;
};

static int
scmi_telemetry_protocol_attributes_get(const struct scmi_protocol_handle *ph,
				       struct telemetry_info *ti)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_msg_resp_telemetry_protocol_attributes *resp;

	ret = ph->xops->xfer_get_init(ph, PROTOCOL_ATTRIBUTES,
				      0, sizeof(*resp), &t);
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
		/* Allocate DEs descriptors */
		ti->info.des = devm_kcalloc(ph->dev, ti->info.base.num_des,
					    sizeof(*ti->info.des), GFP_KERNEL);
		if (!ti->info.des) {
			ret = -ENOMEM;
			goto out;
		}

		/* Allocate a set of contiguous DE info descriptors. */
		ti->info.des_store = devm_kcalloc(ph->dev, ti->info.base.num_des,
						  sizeof(*ti->info.des_store),
						  GFP_KERNEL);
		if (!ti->info.des_store) {
			ret = -ENOMEM;
			goto out;
		}

		/* Allocate DE GROUPS descriptors */
		ti->info.groups = devm_kcalloc(ph->dev, ti->info.base.num_groups,
					       sizeof(*ti->info.groups), GFP_KERNEL);
		if (!ti->info.groups) {
			ret = -ENOMEM;
			goto out;
		}

		/* Allocate a set of contiguous Group info descriptors. */
		ti->info.grps_store = devm_kcalloc(ph->dev, ti->info.base.num_groups,
						   sizeof(*ti->info.grps_store),
						   GFP_KERNEL);
		if (!ti->info.grps_store) {
			ret = -ENOMEM;
			goto out;
		}

		for (int i = 0; i < ti->info.base.num_groups; i++) {
			ti->info.grps_store[i].id = i;
			/* Bind contiguous Group info struct */
			ti->info.groups[i].info = &ti->info.grps_store[i];
		}
	}

out:
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

static int iter_de_descr_process_response(const struct scmi_protocol_handle *ph,
					  const void *response,
					  struct scmi_iterator_state *st,
					  void *priv)
{
	struct telemetry_de *tde;
	struct scmi_tlm_de_priv *p = priv;
	const struct scmi_de_desc *desc = p->next;
	unsigned int grp_id;
	int ret;

	tde = to_tde(p->ti->info.des[st->desc_index + st->loop_idx]);

	tde->de.info->id = le32_to_cpu(desc->id);
	grp_id = le32_to_cpu(desc->grp_id);
	if (grp_id != SCMI_TLM_GRP_INVALID) {
		/* Group descriptors are empty but allocated at this point */
		if (grp_id >= p->ti->info.base.num_groups)
			return -EINVAL;

		/* Link to parent group */
		tde->de.info->grp_id = grp_id;
		tde->de.grp = &p->ti->info.groups[grp_id];
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
	p->next += sizeof(*desc);
	if (tde->de.fc_support) {
		u32 size;
		u64 phys_addr;
		void __iomem *addr;
		struct de_desc_fc *dfc;

		dfc = p->next;
		phys_addr = le32_to_cpu(dfc->addr_low);
		phys_addr |= (u64)le32_to_cpu(dfc->addr_high) << 32;

		size = le32_to_cpu(dfc->size);
		addr = devm_ioremap(ph->dev, phys_addr, size);
		if (!addr)
			return -EADDRNOTAVAIL;

		tde->base = addr;
		tde->offset = 0;
		tde->fc_size = size;

		/* Variably sized depending on FC support */
		p->next += sizeof(*dfc);
	}

	if (tde->de.name_support) {
		const char *de_name = p->next;

		strscpy(tde->de.info->name, de_name, SCMI_SHORT_NAME_MAX_SIZE);
		//tde->de.name = tde->name;

		/* Variably sized depending on name support */
		p->next += SCMI_SHORT_NAME_MAX_SIZE;
	}

	/* Store DE pointer by de_id */
	ret = xa_insert(&p->ti->xa_des, tde->de.info->id, &tde->de, GFP_KERNEL);
	if (ret)
		return ret;

	/* Account for this DE in group num_de counter */
	if (tde->de.grp)
		tde->de.grp->info->num_des++;

	return 0;
}

static int
scmi_telemetry_de_groups_init(struct device *dev, struct telemetry_info *ti)
{
	/* Allocate all groups DEs IDs arrays at first ... */
	for (int i = 0; i < ti->info.base.num_groups; i++) {
		struct scmi_telemetry_group *grp = &ti->info.groups[i];

		grp->des = devm_kcalloc(dev, grp->info->num_des,
					sizeof(unsigned int), GFP_KERNEL);
		if (!grp->des)
			return -ENOMEM;

		/*
		 * Max size 32bit ID string in Hex: 0xCAFECAFE
		 *  - 10 digits + ' '/'\n' = 11 bytes per  number
		 *  - terminating NUL character
		 */
		grp->des_str_sz = grp->info->num_des * 11 + 1;
		grp->des_str = devm_kzalloc(dev, grp->des_str_sz, GFP_KERNEL);
		if (!grp->des_str)
			return -ENOMEM;

		/* Reset group DE counter */
		grp->info->num_des = 0;
	}

	/* Scan DEs and populate DE IDs arrays for all groups */
	for (int i = 0; i < ti->info.base.num_des; i++) {
		struct scmi_telemetry_group *grp = ti->info.des[i]->grp;

		if (!grp)
			continue;

		/*
		 * Note that, at this point, num_des is guaranteed to be
		 * sane (in-bounds) by construction.
		 */
		grp->des[grp->info->num_des++] = i;
	}

	/* Build compsing DES string */
	for (int i = 0; i < ti->info.base.num_groups; i++) {
		struct scmi_telemetry_group *grp = &ti->info.groups[i];
		char *buf = grp->des_str;
		size_t bufsize = grp->des_str_sz;

		for (int j = 0; j < grp->info->num_des; j++) {
			char term = j != (grp->info->num_des - 1) ? ' ' : '\0';
			int len;

			len = scnprintf(buf, bufsize, "0x%04X%c",
					ti->info.des[grp->des[j]]->info->id, term);

			buf += len;
			bufsize -= len;
		}
	}

	return 0;
}

static int
scmi_telemetry_de_descriptors_get(const struct scmi_protocol_handle *ph,
				  struct telemetry_info *ti)
{
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

	xa_init(&ti->xa_des);
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

static int scmi_telemetry_enumerate_de(const struct scmi_protocol_handle *ph,
				       struct telemetry_info *ti)
{
	int ret;

	if (!ti->info.base.num_des)
		return 0;

	for (int i = 0; i < ti->info.base.num_des; i++) {
		struct telemetry_de *tde;

		tde = devm_kzalloc(ph->dev, sizeof(*tde), GFP_KERNEL);
		if (!tde)
			return -ENOMEM;

		mutex_init(&tde->mtx);

		/* Bind contiguous DE info structures */
		tde->de.info = &ti->info.des_store[i];
		ti->info.des[i] = &tde->de;
	}

	ret = scmi_telemetry_de_descriptors_get(ph, ti);
	if (ret) {
		dev_err(ph->dev, "Cannot get DE descriptors");
		return ret;
	}

	return 0;
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
		struct scmi_tlm_intervals *intrvs;
		int inum;

		inum = st->num_returned + st->num_remaining;
		intrvs = devm_kzalloc(p->dev,
				      sizeof(*intrvs) + inum * sizeof(__u32),
				      GFP_KERNEL);
		if (!intrvs)
			return -ENOMEM;

		intrvs->discrete = INTERVALS_DISCRETE(r->flags);
		/* Check consistency on first call */
		if (!intrvs->discrete &&
		    (st->num_returned != 3 || st->num_remaining != 0))
			return -EINVAL;

		intrvs->num = inum;
		st->max_resources = intrvs->num;

		*p->intrvs = intrvs;
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
scmi_tlm_enumerate_update_intervals(const struct scmi_protocol_handle *ph,
				    struct telemetry_info *ti, int grp_id,
				    unsigned int flags)
{
	struct scmi_iterator_ops ops = {
		.prepare_message = iter_intervals_prepare_message,
		.update_state = iter_intervals_update_state,
		.process_response = iter_intervals_process_response,
	};
	struct scmi_tlm_ivl_priv ipriv = {
		.dev = ph->dev,
		.grp_id = grp_id,
		.intrvs = (grp_id == SCMI_TLM_GRP_INVALID) ?
			&ti->info.intervals :
			&ti->info.groups[grp_id].intervals,
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
scmi_telemetry_enumerate_update_intervals(const struct scmi_protocol_handle *ph,
					  struct telemetry_info *ti)
{
	int ret;
	unsigned int flags;

	flags = !ti->info.per_group_config_support ?
		ALL_DES_ANY_GROUP : ALL_DES_NO_GROUP;

	ret = scmi_tlm_enumerate_update_intervals(ph, ti, SCMI_TLM_GRP_INVALID,
						  flags);
	if (ret)
		return ret;

	/* A copy for UAPI access... */
	ti->info.base.num_intervals = ti->info.intervals->num;
	if (ti->info.base.num_groups && ti->info.per_group_config_support) {
		flags = SPECIFIC_GROUP_DES;
		for (int id = 0; id < ti->info.base.num_groups; id++) {
			ret = scmi_tlm_enumerate_update_intervals(ph, ti, id,
								  flags);
			if (ret)
				break;

			ti->info.grps_store[id].num_intervals =
				ti->info.groups[id].intervals->num;
		}
	}

	return ret;
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

static int scmi_telemetry_enumerate_shmti(const struct scmi_protocol_handle *ph,
					  struct telemetry_info *ti)
{
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

static const struct scmi_tlm_de_info *
scmi_telemetry_de_info_get(const struct scmi_protocol_handle *ph, u32 id)
{
	struct telemetry_info *ti = ph->get_priv(ph);

	return xa_load(&ti->xa_des, id);
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
	bts = scmi_telemetry_blkts_lookup(ti->dev, &ti->xa_bts, payld);
	if (WARN_ON(!bts))
		return;

	/* Update the descriptor with the lastest TS*/
	scmi_telemetry_blkts_update(shmti->last_magic, bts);
}

static void scmi_telemetry_tdcf_data_parse(struct telemetry_info *ti,
					   struct payload __iomem *payld,
					   struct telemetry_shmti *shmti,
					   bool update)
{
	bool ts_valid = TS_VALID(payld);
	struct scmi_telemetry_de *de;
	struct telemetry_de *tde;
	u64 val, tstamp = 0;
	u32 id;

	id = PAYLD_ID(payld);
	de = xa_load(&ti->xa_des, id);
	if (!de)
		return;

	tde = to_tde(de);
	/* Update DE location refs if requested: normally done only on enable */
	if (update) {
		tde->base = shmti->base;
		tde->eplg = SHMTI_EPLG(shmti);
		tde->offset = (void *)payld - (void *)shmti->base;
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
				tde->bts = scmi_telemetry_blkts_bind(ti->dev,
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
	if (de->tstamp_enabled)
		tde->last_ts = tstamp;
	else
		tde->last_ts = 0;
}

static int scmi_telemetry_tdcf_line_parse(struct telemetry_info *ti,
					  struct payload __iomem *payld,
					  struct telemetry_shmti *shmti,
					  bool update)
{
	int used_qwords;

	used_qwords = (USE_LINE_TS(payld) && TS_VALID(payld)) ?
		QWORDS_TS_LINE_DATA_PAYLD : QWORDS_LINE_DATA_PAYLD;

	/*Invalid lines are not an error, could simply be disabled DEs */
	if (DATA_INVALID(payld))
		return used_qwords;

	if (!IS_BLK_TS(payld))
		scmi_telemetry_tdcf_data_parse(ti, payld, shmti, update);
	else
		scmi_telemetry_tdcf_blkts_parse(ti, payld, shmti);

	return used_qwords;
}

static int scmi_telemetry_shmti_scan(struct telemetry_info *ti,
				     unsigned int shmti_id, u64 ts,
				     bool update)
{
	struct telemetry_shmti *shmti = &ti->shmti[shmti_id];
	struct tdcf __iomem *tdcf = shmti->base;
	int retries = SCMI_TLM_TDCF_MAX_RETRIES;
	u64 startm = 0, endm = 0xffffffffffffffff;
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

		qwords = tdcf->prlg.num_qwords;
		next = tdcf->payld;
		while (qwords) {
			int used_qwords;

			used_qwords = scmi_telemetry_tdcf_line_parse(ti, next,
								     shmti, update);
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
	struct scmi_telemetry_de *de;

	for (int i = 0; i < grp->info->num_des; i++) {
		de = ti->info.des[grp->des[i]];

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
				tde->bts = scmi_telemetry_blkts_bind(ti->dev,
								     &ti->shmti[sid],
								     payld,
								     &ti->xa_bts);
				if (WARN_ON(!tde->bts))
					return -EPROTO;
			}
		} else {
			/*
			 * A full SHMTI scan is needed when enabling a
			 * group or its timestamps in order to retrieve
			 * offsets: node that when group-timestamp is
			 * enabled for composing DEs a re-scan is needed
			 * since some DEs could have been relocated due
			 * to lack of space in the TDCF.
			 */
			scmi_telemetry_shmti_scan(ti, sid, 0, true);
		}
	} else if (!is_group) {
		struct telemetry_de *tde;

		tde = to_tde(de);
		if (tde->bts) {
			/* Unlink the related BLK_TS on disable */
			scmi_telemetry_blkts_put(ti->dev, tde->bts);
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
	void *obj;
	bool *enabled_state, *tstamp_enabled_state;
	struct telemetry_info *ti = ph->get_priv(ph);

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

		grp = &ti->info.groups[id];

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
	struct scmi_msg_telemetry_de_configure *msg;
	struct scmi_xfer *t;
	int ret;

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
		struct telemetry_info *ti = ph->get_priv(ph);

		for (int i = 0; i < ti->info.base.num_des; i++)
			ti->info.des[i]->enabled = false;

		if (is_group) {
			for (int i = 0; i < ti->info.base.num_groups; i++)
				ti->info.groups[i].enabled = false;
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
		active_update_interval =
			&ti->info.groups[res_id].active_update_interval;
		current_mode = &ti->info.groups[res_id].current_mode;
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
	msg->control |= grp_ignore ? TELEMETRY_SELECTOR_ALL :
		TELEMETRY_SELECTOR_GROUP;
	msg->control |= TELEMETRY_MODE(next_mode);
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

static int scmi_telemetry_de_data_fc_read(struct telemetry_de *tde,
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

	return 0;
}

static void scmi_telemetry_scan_update(struct telemetry_info *ti, u64 ts)
{
	/* Scan all SHMTIs ... */
	for (int id = 0; id < ti->num_shmti; id++)
		scmi_telemetry_shmti_scan(ti, id, ts, false);

	/* ... then scan all FCs ... XXX Use a list */
	for (int i = 0; i < ti->info.base.num_des; i++) {
		struct scmi_telemetry_de *de;
		struct telemetry_de *tde;
		u64 val, tstamp;
		int ret;

		de = ti->info.des[i];
		if (!de->enabled)
			continue;

		tde = to_tde(de);
		if (!tde->de.fc_support)
			continue;

		//TODO Report errors
		ret = scmi_telemetry_de_data_fc_read(tde, &tstamp, &val);
		if (ret)
			return;

		guard(mutex)(&tde->mtx);
		tde->last_val = val;
		if (de->tstamp_enabled)
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

		if (le32_to_cpu(payld->id) != tde->de.info->id)
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

static int scmi_telemetry_de_lookup(struct telemetry_de *tde,
				    u64 *tstamp, u64 *val)
{
	if (!tde->de.fc_support)
		return scmi_telemetry_tdcf_de_parse(tde, tstamp, val);

	return scmi_telemetry_de_data_fc_read(tde, tstamp, val);
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

	return scmi_telemetry_de_lookup(tde, tstamp, val);
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
	int max_samples;

	max_samples = *num_samples;
	*num_samples = 0;

	for (int i = 0; i < ti->info.base.num_des; i++) {
		struct scmi_telemetry_de *de;
		u64 val, tstamp;
		int ret;

		de = ti->info.des[i];
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
				 unsigned int num_dwords, unsigned int *dwords,
				 ktime_t timestamp)
{
	u32 next = 0;

	while (next < num_dwords) {
		struct payload *payld = (struct payload *)&dwords[next];
		struct scmi_telemetry_de *de;
		struct telemetry_de *tde;
		u32 de_id;

		next += USE_LINE_TS(payld) ?
			TS_LINE_DATA_PAYLD_WORDS : LINE_DATA_PAYLD_WORDS;

		if (DATA_INVALID(payld)) {
			dev_err(ti->dev, "MSG - Received INVALID DATA line\n");
			continue;
		}

		de_id = le32_to_cpu(payld->id);
		de = xa_load(&ti->xa_des, de_id);
		if (!de || !de->enabled) {
			dev_err(ti->dev,
				"MSG - Received INVALID DE - ID:%u  enabled:%d\n",
				de_id, de ? (de->enabled ? 'Y' : 'N') : 'X');
			continue;
		}

		tde = to_tde(de);
		guard(mutex)(&tde->mtx);
		tde->cached = true;
		tde->last_val = LINE_DATA_GET(&payld->tsl);
		//TODO BLK_TS in notification payloads
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
	msg->control |= grp_ignore ? TELEMETRY_SELECTOR_ALL :
		TELEMETRY_SELECTOR_GROUP;
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

static int scmi_telemetry_reset(const struct scmi_protocol_handle *ph)
{
	int ret;
	struct scmi_xfer *t;

	ret = ph->xops->xfer_get_init(ph, TELEMETRY_RESET, sizeof(u32), 0, &t);
	if (ret)
		return ret;

	put_unaligned_le32(0, t->tx.buf);
	ret = ph->xops->do_xfer(ph, t);
	if (!ret) {
		struct telemetry_info *ti = ph->get_priv(ph);

		//XXX Better would be to read back from platform
		//	CONFIG_GET + DE_ENABLED_LIST
		ti->info.enabled = false;
		ti->info.notif_enabled = false;
		ti->info.current_mode = SCMI_TLM_ONDEMAND;
		ti->info.active_update_interval = 0;

		for (int i = 0; i < ti->info.base.num_des; i++) {
			ti->info.des[i]->enabled = false;
			ti->info.des[i]->tstamp_enabled = false;
		}

		for (int i = 0; i < ti->info.base.num_groups; i++) {
			ti->info.groups[i].enabled = false;
			ti->info.groups[i].tstamp_enabled = false;
			ti->info.groups[i].current_mode = SCMI_TLM_ONDEMAND;
			ti->info.groups[i].active_update_interval = 0;
		}
	}
	ph->xops->xfer_put(ph, t);

	return ret;
}

static const struct scmi_telemetry_proto_ops tlm_proto_ops = {
	.info_get = scmi_telemetry_info_get,
	.de_info_get = scmi_telemetry_de_info_get,
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
		dev_err(ti->dev, "Bad Telemetry update notification - ret: %dn",
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

static int scmi_telemetry_protocol_init(const struct scmi_protocol_handle *ph)
{
	struct telemetry_info *ti;
	u32 version;
	int ret;

	ret = ph->xops->version_get(ph, &version);
	if (ret)
		return ret;

	dev_dbg(ph->dev, "Telemetry Version %d.%d\n",
		PROTOCOL_REV_MAJOR(version), PROTOCOL_REV_MINOR(version));

	ti = devm_kzalloc(ph->dev, sizeof(*ti), GFP_KERNEL);
	if (!ti)
		return -ENOMEM;

	ti->dev = ph->dev;
	xa_init(&ti->xa_bts);

	ret = scmi_telemetry_protocol_attributes_get(ph, ti);
	if (ret)
		return ret;

	ret = scmi_telemetry_enumerate_de(ph, ti);
	if (ret)
		return ret;

	ret = scmi_telemetry_enumerate_update_intervals(ph, ti);
	if (ret)
		return ret;

	ret = scmi_telemetry_enumerate_shmti(ph, ti);
	if (ret)
		return ret;

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
