// SPDX-License-Identifier: GPL-2.0
/*
 * System Control and Management Interface (SCMI) Telemetry Protocol
 *
 * Copyright (C) 2025 ARM Ltd.
 *
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
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
	__le32 de_implementation_rev_dword[SCMI_TLM_MAX_DWORD];
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

#define BLK_TSTAMP(l)	((((u64)(l)->data_high) << 32) | (l)->data_low)

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

#define LINE_DATA_GET(f)	(TO_CPU_64((f)->data_high, (f)->data_low))
#define LINE_TSTAMP_GET(f)	(TO_CPU_64((f)->ts_high, (f)->ts_low))

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

#define LINE_DATA_PAYLD_WORDS						       \
	((sizeof(u32) + sizeof(u32) + sizeof(struct line)) / sizeof(u32))
#define TS_LINE_DATA_PAYLD_WORDS					       \
	((sizeof(u32) + sizeof(u32) + sizeof(struct tsline)) / sizeof(u32))

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
};

#define SHMTI_EPLG(s)						\
	({							\
		struct telemetry_shmti *_s = (s);		\
		void *_eplg;					\
								\
		_eplg = _s->base + _s->len;			\
		(_eplg);					\
	})

struct telemetry_info {
	bool streaming_mode;
	int num_shmti;
	struct device *dev;
	struct telemetry_shmti *shmti;
	struct xarray xa_des;
	struct scmi_telemetry_info info;
	struct notifier_block telemetry_nb;
};

#define telemetry_nb_to_info(x)	\
	container_of(x, struct telemetry_info, telemetry_nb)

struct telemetry_de {
	bool cached;
	char name[SCMI_SHORT_NAME_MAX_SIZE];
	void __iomem *base;
	void __iomem *eplg;
	u32 offset;
	/* NOTE THAT DE data_sz is registered in scmi_telemetry_de */
	u32 fc_size;
	struct scmi_telemetry_de de;
	u64 last_val;
	u64 last_ts;
	/* Protect last_val/ts accesses  */
	struct mutex mtx;
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

		ti->info.num_de = le32_to_cpu(resp->de_num);
		ti->info.num_groups = le32_to_cpu(resp->groups_num);
		for (int i = 0; i < SCMI_TLM_MAX_DWORD; i++)
			ti->info.de_impl_version[i] =
				le32_to_cpu(resp->de_implementation_rev_dword[i]);
		ti->info.single_read_support = SUPPORTS_SINGLE_READ(attr);
		ti->info.continuos_update_support = SUPPORTS_CONTINUOS_UPDATE(attr);
		ti->info.per_group_config_support = SUPPORTS_PER_GROUP_CONFIG(attr);
		ti->info.reset_support = SUPPORTS_RESET(attr);
		ti->info.fc_support = SUPPORTS_FC(attr);
		ti->num_shmti = le32_get_bits(attr, GENMASK(15, 0));
		/* Allocate DEs descriptors */
		ti->info.des = devm_kcalloc(ph->dev, ti->info.num_de,
					    sizeof(*ti->info.des), GFP_KERNEL);
		if (!ti->info.des)
			ret = -ENOMEM;

		/* Allocate DE GROUPS descriptors */
		ti->info.des_groups = devm_kcalloc(ph->dev, ti->info.num_groups,
						   sizeof(*ti->info.des_groups),
						   GFP_KERNEL);
		if (!ti->info.des_groups)
			ret = -ENOMEM;

		for (int i = 0; i < ti->info.num_groups; i++)
			ti->info.des_groups[i].id = i;
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

	tde->de.id = le32_to_cpu(desc->id);
	grp_id = le32_to_cpu(desc->grp_id);
	if (grp_id != SCMI_TLM_GRP_INVALID) {
		if (grp_id >= p->ti->info.num_groups)
			return -EINVAL;

		/* Link to parent group */
		tde->de.grp = &p->ti->info.des_groups[grp_id];
	}
	tde->de.data_sz = le32_to_cpu(desc->data_sz);
	tde->de.type = GET_DE_TYPE(desc);
	tde->de.unit = GET_DE_UNIT(desc);
	tde->de.unit_exp = GET_DE_UNIT_EXP(desc);
	tde->de.tstamp_exp = GET_DE_TSTAMP_EXP(desc);
	tde->de.instance_id = GET_DE_INSTA_ID(desc);
	tde->de.compo_instance_id = GET_COMPO_INSTA_ID(desc);
	tde->de.compo_type = GET_COMPO_TYPE(desc);
	tde->de.persistent = IS_PERSISTENT(desc);
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

		strscpy(tde->name, de_name, SCMI_SHORT_NAME_MAX_SIZE);
		tde->de.name = tde->name;

		/* Variably sized depending on name support */
		p->next += SCMI_SHORT_NAME_MAX_SIZE;
	}

	/* Store DE pointer by de_id */
	ret = xa_insert(&p->ti->xa_des, tde->de.id, &tde->de, GFP_KERNEL);
	if (ret)
		return ret;

	/* Account for this DE in group num_de counter */
	if (tde->de.grp)
		tde->de.grp->num_de++;

	return 0;
}

static int
scmi_telemetry_de_groups_init(struct device *dev, struct telemetry_info *ti)
{
	/* Allocate all groups DEs IDs arrays at first ... */
	for (int i = 0; i < ti->info.num_groups; i++) {
		struct scmi_telemetry_group *grp = &ti->info.des_groups[i];

		grp->des = devm_kcalloc(dev, grp->num_de, sizeof(unsigned int),
					GFP_KERNEL);
		if (!grp->des)
			return -ENOMEM;

		/*
		 * Max size 32bit ID string in Hex: 0xCAFECAFE
		 *  - 10 digits + ' '/'\n' = 11 bytes per  number
		 *  - terminating NUL character
		 */
		grp->des_str_sz = grp->num_de * 11 + 1;
		grp->des_str = devm_kzalloc(dev, grp->des_str_sz, GFP_KERNEL);
		if (!grp->des_str)
			return -ENOMEM;

		/* Reset group DE counter */
		grp->num_de = 0;
	}

	/* Scan DEs and populate group DE IDs arrays */
	for (int i = 0; i < ti->info.num_de; i++) {
		struct scmi_telemetry_group *grp = ti->info.des[i]->grp;

		if (!grp)
			continue;

		/* Note that, at this point, num_de is guaranteed to be
		 * sane (in-bounds) by construction.
		 */
		grp->des[grp->num_de++] = i;
	}

	/* Build compsing DES string */
	for (int i = 0; i < ti->info.num_groups; i++) {
		struct scmi_telemetry_group *grp = &ti->info.des_groups[i];
		char *buf = grp->des_str;
		size_t bufsize = grp->des_str_sz;

		for (int j = 0; j < grp->num_de; j++) {
			char term = j != (grp->num_de - 1) ? ' ' : '\0';
			int len;

			len = snprintf(buf, bufsize, "0x%04X%c",
				       ti->info.des[grp->des[j]]->id, term);

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
	iter = ph->hops->iter_response_init(ph, &ops, ti->info.num_de,
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

	if (!ti->info.num_de)
		return 0;

	for (int i = 0; i < ti->info.num_de; i++) {
		struct telemetry_de *tde;

		tde = devm_kzalloc(ph->dev, sizeof(*tde), GFP_KERNEL);
		if (!tde)
			return -ENOMEM;

		mutex_init(&tde->mtx);
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
	struct scmi_telemetry_update_interval *intrvs;
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

		p->intrvs->discrete = INTERVALS_DISCRETE(r->flags);
		/* Check consistency on first call */
		if (!p->intrvs->discrete &&
		    (st->num_returned != 3 || st->num_remaining != 0))
			return -EINVAL;

		p->intrvs->num = st->num_returned + st->num_remaining;
		p->intrvs->update_intervals =
			devm_kcalloc(p->dev, p->intrvs->num,
				     sizeof(*p->intrvs->update_intervals),
				     GFP_KERNEL);
		if (!p->intrvs->update_intervals) {
			p->intrvs->num = 0;
			return -ENOMEM;
		}

		st->max_resources = p->intrvs->num;
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
	unsigned int idx = st->loop_idx;

	p->intrvs->update_intervals[st->desc_index + idx] = r->intervals[idx];

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
			&ti->info.des_groups[grp_id].intervals,
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

	if (ti->info.num_groups && ti->info.per_group_config_support) {
		flags = SPECIFIC_GROUP_DES;
		for (int grp_id = 0; grp_id < ti->info.num_groups; grp_id++) {
			ret = scmi_tlm_enumerate_update_intervals(ph, ti, grp_id,
								  flags);
			if (ret)
				break;
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

	iter = ph->hops->iter_response_init(ph, &ops, ti->info.num_de,
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

static int scmi_telemetry_tdcf_parse_one(struct telemetry_info *ti,
					 struct payload __iomem *payld,
					 struct telemetry_shmti *shmti)
{
	struct scmi_telemetry_de *de;
	struct telemetry_de *tde;
	u64 val, tstamp;
	int used_qwords;

	de = xa_load(&ti->xa_des, le32_to_cpu(payld->id));
	if (!de || DATA_INVALID(payld))
		return -EINVAL;

	used_qwords = 4;

	tde = to_tde(de);
	if (shmti) {
		tde->base = shmti->base;
		tde->eplg = shmti->base + shmti->len - TDCF_EPLG_SZ;
		tde->offset = (void *)payld - (void *)shmti->base;
	}

	//TODO BLK_TS
	if (USE_LINE_TS(payld)) {
		used_qwords += 2;
		if (TS_VALID(payld))
			tstamp = LINE_TSTAMP_GET(&payld->tsl);
	}
	val = LINE_DATA_GET(&payld->tsl);

	guard(mutex)(&tde->mtx);
	tde->last_val = val;
	if (de->tstamp_enabled)
		tde->last_ts = tstamp;
	else
		tde->last_ts = 0;

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

		qwords = tdcf->prlg.num_qwords;
		next = tdcf->payld;
		while (qwords) {
			int used_qwords;

			used_qwords = scmi_telemetry_tdcf_parse_one(ti, next,
								    update ? shmti : NULL);
			if (qwords < used_qwords)
				return -EINVAL;

			next += used_qwords * 4;
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

	for (int i = 0; i < grp->num_de; i++) {
		de = ti->info.des[grp->des[i]];

		if (enable)
			de->enabled = *enable;
		if (tstamp)
			de->tstamp_enabled = *tstamp;
	}

	return 0;
}

static int __scmi_telemetry_state_set(const struct scmi_protocol_handle *ph,
				      bool is_group, bool *enable,
				      bool *enabled_state, bool *tstamp,
				      bool *tstamp_enabled_state, void *priv)
{
	struct scmi_msg_resp_telemetry_de_configure *resp;
	struct scmi_msg_telemetry_de_configure *msg;
	struct telemetry_info *ti = ph->get_priv(ph);
	struct scmi_telemetry_group *grp = priv;
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

	ret = ph->xops->xfer_get_init(ph, TELEMETRY_DE_CONFIGURE,
				      sizeof(*msg), sizeof(*resp), &t);
	if (ret)
		return ret;

	msg = t->tx.buf;
	/* Note that BOTH DE and GROUPS have a first ID field.. */
	msg->id = cpu_to_le32(grp->id);
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
		u32 id = le32_to_cpu(resp->shmti_id);

		/* Update DE SHMTI and offset, if applicable */
		if (enable && IS_SHMTI_ID_VALID(id)) {
			if (id >= ti->num_shmti) {
				ret = -EPROTO;
				goto out;
			}

			/*
			 * Update SHMTI/offset while skipping non-SHMTI-DEs like
			 * FCs and notif-only.
			 */
			if (!is_group) {
				struct scmi_telemetry_de *de = priv;
				struct telemetry_de *tde;
				u32 offs;

				offs = le32_to_cpu(resp->tdcf_de_offset);
				if (offs >= ti->shmti[id].len - de->data_sz) {
					ret = -EPROTO;
					goto out;
				}

				tde = to_tde(de);
				tde->base = ti->shmti[id].base;
				tde->offset = offs;
				/* A handy reference to the Epilogue updated */
				tde->eplg = tde->base + ti->shmti[id].len -
					TDCF_EPLG_SZ;
			} else {
				/*
				 * A full SHMTI scan is needed when enabling a
				 * group in order to retrieve offsets.
				 */
				scmi_telemetry_shmti_scan(ti, id, 0, true);
			}
		}

		/* Update cached state on success */
		if (enable)
			*enabled_state = *enable;
		if (tstamp)
			*tstamp_enabled_state = *tstamp;

		if (is_group)
			scmi_telemetry_group_state_update(ti, grp, enable,
							  tstamp);
	}

out:
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

		if (id >= ti->info.num_groups)
			return -EINVAL;

		grp = &ti->info.des_groups[id];

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

		for (int i = 0; i < ti->info.num_de; i++)
			ti->info.des[i]->enabled = false;

		if (is_group) {
			for (int i = 0; i < ti->info.num_groups; i++)
				ti->info.des_groups[i].enabled = false;
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
	struct scmi_telemetry_update_interval *intervals;
	struct telemetry_info *ti = ph->get_priv(ph);
	enum scmi_telemetry_collection *current_mode, next_mode;
	struct scmi_msg_telemetry_config_set *msg;
	struct scmi_xfer *t;
	bool tlm_enable;
	u32 interval;
	int ret;

	if (mode && *mode == SCMI_TLM_NOTIFICATION &&
	    !ti->info.continuos_update_support)
		return -EINVAL;

	if (res_id != SCMI_TLM_GRP_INVALID && res_id >= ti->info.num_groups)
		return -EINVAL;

	if (res_id == SCMI_TLM_GRP_INVALID || grp_ignore) {
		intervals = &ti->info.intervals;
		current_mode = &ti->info.current_mode;
	} else {
		intervals = &ti->info.des_groups[res_id].intervals;
		current_mode = &ti->info.des_groups[res_id].current_mode;
	}

	if (!enable && !update_interval_ms && (!mode || *mode == *current_mode))
		return 0;

	ret = ph->xops->xfer_get_init(ph, TELEMETRY_CONFIG_SET,
				      sizeof(*msg), 0, &t);
	if (ret)
		return ret;

	if (!update_interval_ms) {
		interval = cpu_to_le32(intervals->active_update_interval);
	} else {
		//TODO Fix .. not only ms...review API
		interval = FIELD_PREP(GENMASK(20, 5),
				      cpu_to_le32(*update_interval_ms));
		interval |= FIELD_PREP(GENMASK(4, 0), (-3 & 0x1f));
	}

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
			intervals->active_update_interval = interval;
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

/*
 * TDCF and TS Line Management Notes
 * ---------------------------------
 *  (from a chat with ATG)
 *
 * TCDF Payload Metadata notable bits:
 *  - Bit[2]: USE Tstamp
 *  - Bit[1]: Tstamp VALID
 *
 * CASE_1:
 * -------
 *	+ A DE is enabled with timestamp disabled, so the TS_Line is NOT present
 *	  -> BIT[2:1] = 00b
 *	+ that DE is then 're-enabled' with TS_line: so it was ON, it remains ON
 *	  but using DE_CONFIGURE I now enabled also TS, so the platform
 *	  relocates it at the end of the SHMTI and return the new offset
 *	  -> BIT[2:1]: 11b
 *	+ the hole left from the relocated DE can be reused by the platform to
 *	  fit another equally sized DE. (i.e. without shuffling around any
 *	  other enabled DE, since that would cause a change of the known
 *	  offset)
 *
 * CASE_2:
 * -------
 *	+ A DE is enabled with timestamp enabled, so the TS_Line is there
 *	  -> BIT[2:1]:11b
 *	+ that DE has its timestamp disabled: again, you can do this without
 *	  disabling it fully but just disabling the TS, so now that TS_line
 *	  fields are still physiclly there but NOT valid
 *	  -> BIT[2:1]= 10b
 *	+ the hole from the timestamp remain there unused until
 *		- you enable again the TS so the hole is used again
 *		  -> BIT[2:1]=11b
 *			OR
 *		- you disable fully the DE and then re-enable it with the TS
 *		  -> potentially CASE_1 the DE is relocated on enable
 */
static int scmi_telemetry_de_tdcf_parse(struct telemetry_de *tde,
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

		payld = tde->base + tde->offset;
		if (le32_to_cpu(payld->id) != tde->de.id || DATA_INVALID(payld))
			return -EINVAL;

		//TODO BLK_TS
		if (tstamp && USE_LINE_TS(payld) && TS_VALID(payld))
			*tstamp = LINE_TSTAMP_GET(&payld->tsl);

		*val = LINE_DATA_GET(&payld->tsl);

		endm = TDCF_END_SEQ_GET(tde->eplg);
	} while (startm != endm && --retries);

	if (startm != endm)
		return -EPROTO;

	return 0;
}

static int scmi_telemetry_de_lookup(struct telemetry_de *tde,
				    u64 *tstamp, u64 *val)
{
	if (!tde->de.fc_support)
		return scmi_telemetry_de_tdcf_parse(tde, tstamp, val);

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

	for (int i = 0; i < ti->info.num_de; i++) {
		struct scmi_telemetry_de *de;
		u64 val, tstamp;
		int ret;

		de = ti->info.des[i];
		if (grp_id != SCMI_TLM_GRP_INVALID &&
		    (!de->grp || de->grp->id != grp_id))
			continue;

		ret = scmi_telemetry_de_collect(ti, de, &tstamp, &val);
		if (ret)
			continue;

		if (*num_samples == max_samples)
			return -ENOSPC;

		samples[*num_samples].tstamp = tstamp;
		samples[*num_samples].val = val;
		samples[*num_samples].id = de->id;

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
		//TODO BLK_TS ?
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
	if (!grp_ignore && grp_id >= ti->info.num_groups)
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
	///TODO
	return 0;
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
		ti->info.intervals.active_update_interval = 0;

		for (int i = 0; i < ti->info.num_de; i++) {
			ti->info.des[i]->enabled = false;
			ti->info.des[i]->tstamp_enabled = false;
		}

		for (int i = 0; i < ti->info.num_groups; i++) {
			ti->info.des_groups[i].enabled = false;
			ti->info.des_groups[i].tstamp_enabled = false;
			ti->info.des_groups[i].current_mode = SCMI_TLM_ONDEMAND;
			ti->info.des_groups[i].intervals.active_update_interval = 0;
		}
	}
	ph->xops->xfer_put(ph, t);

	return ret;
}

static const struct scmi_telemetry_proto_ops tlm_proto_ops = {
	.info_get = scmi_telemetry_info_get,
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

static void scmi_telemetry_scan_update(struct telemetry_info *ti, u64 ts)
{
	/* Scan all SHMTIs ... */
	for (int id = 0; id < ti->num_shmti; id++)
		scmi_telemetry_shmti_scan(ti, id, ts, false);

	/* ... then scan all FCs ... XXX Use a list */
	for (int i = 0; i < ti->info.num_de; i++) {
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

		//TODO Repoet errors
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

	ti->info.version = version;

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
