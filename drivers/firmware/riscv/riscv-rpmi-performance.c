// SPDX-License-Identifier: GPL-2.0
/*
 * RISC-V RPMI performance service group core
 *
 * Copyright (C) 2026 Shanghai StarFive Technology Co., Ltd.
 *
 * Owns the MPXY mailbox channel of the RPMI performance service group and the
 * enumeration of the performance domains behind it, and hands both to the
 * front-ends that drive a domain. The channel cannot be shared, so it has a
 * single owner here and the front-ends sit on top of it.
 */

#define pr_fmt(fmt) "riscv-rpmi-performance: " fmt

#include <linux/bitfield.h>
#include <linux/bsearch.h>
#include <linux/firmware/riscv/riscv-rpmi-performance.h>
#include <linux/io.h>
#include <linux/mailbox/riscv-rpmi-message.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>
#include <linux/sort.h>

#define RPMI_PERF_DOMAIN_NAME_LEN	16

struct rpmi_fc_db_info {
	int width;
	u32 set;
	void __iomem *addr;
};

struct rpmi_fc_info {
	void __iomem *set_addr;
	void __iomem *get_addr;
	struct rpmi_fc_db_info *set_db;
};

struct rpmi_ctx {
	struct mbox_chan *chan;
	struct mbox_client client;
	/* serialises RPMI transactions on @chan */
	struct mutex lock;
	u32 max_msg_size;
};

struct rpmi_fast_channel {
	u64 addr;
	u64 size;
};

struct rpmi_perf_domain {
	u32 id;
	bool enumerated;
	bool set_limit;
	bool set_level;
	bool has_fastchannels;
	u32 level_count;
	u32 rate_limit_us;
	char name[RPMI_PERF_DOMAIN_NAME_LEN];
	struct rpmi_perf_level *level;
	struct rpmi_fc_info *fc_info;
	struct rpmi_ctx *mpxy_ctx;
	struct device *dev;
	struct rpmi_perf *perf;
};

struct rpmi_perf {
	u32 num_domains;
	struct rpmi_fast_channel fast_channel;
	struct device *dev;
	struct rpmi_perf_domain *domain;
};

enum {
	RPMI_PERF_FC_LEVEL,
	RPMI_PERF_FC_MAX,
};

/* Service: ENABLE_NOTIFICATION */
struct rpmi_perf_enable_notification_tx {
	__le32 event_id;
};

struct rpmi_perf_enable_notification_rx {
	__le32 status;
};

/* Service: GET_NUM_DOMAINS */
struct rpmi_perf_get_num_domain_rx {
	__le32 status;
	__le32 num_domains;
};

/* Service: GET_ATTRS */
struct rpmi_perf_get_attrs_tx {
	__le32 domain_id;
};

struct rpmi_perf_get_attrs_rx {
	__le32 status;
	__le32 flags;
#define PERF_LIMIT_SETTING(f)		(FIELD_GET(BIT(2), (f)))
#define PERF_LEVEL_SETTING(f)		(FIELD_GET(BIT(1), (f)))
#define FAST_CHANNEL_SUPPORT(f)		(FIELD_GET(BIT(0), (f)))
	__le32 num_levels;
	__le32 trans_latency_us;
	char name[RPMI_PERF_DOMAIN_NAME_LEN];
};

/* Service: GET_SUPPORTED_LEVELS */
struct rpmi_perf_get_supported_level_tx {
	__le32 domain_id;
	__le32 level_index;
};

struct rpmi_perf_supported_level {
	__le32 index;
	__le32 clock_freq;
	__le32 power_cost;
	__le32 trans_latency_us;
};

struct rpmi_perf_get_supported_level_rx {
	__le32 status;
	__le32 flags;
	__le32 remaining;
	__le32 returned;
	struct rpmi_perf_supported_level level[];
};

/* Service: GET_LEVEL */
struct rpmi_perf_get_level_tx {
	__le32 domain_id;
};

struct rpmi_perf_get_level_rx {
	__le32 status;
	__le32 level_index;
};

/* Service: SET_LEVEL */
struct rpmi_perf_set_level_tx {
	__le32 domain_id;
	__le32 level_index;
};

struct rpmi_perf_set_level_rx {
	__le32 status;
};

/* Service: GET_LIMIT */
struct rpmi_perf_get_limit_tx {
	__le32 domain_id;
};

struct rpmi_perf_get_limit_rx {
	__le32 status;
	__le32 max_level;
	__le32 min_level;
};

/* Service: SET_LIMIT */
struct rpmi_perf_set_limit_tx {
	__le32 domain_id;
	__le32 max_level;
	__le32 min_level;
};

struct rpmi_perf_set_limit_rx {
	__le32 status;
};

/* Service: GET_FAST_CHANNEL_REGION */
struct rpmi_perf_get_fast_channel_region_rx {
	__le32 status;
	__le32 addr_low;
	__le32 addr_high;
	__le32 size_low;
	__le32 size_high;
};

/* Service: GET_FAST_CHANNEL_ATTRIBUTES */
struct rpmi_perf_get_fast_channel_attributes_tx {
	__le32 domain_id;
	__le32 service_id;
};

struct rpmi_perf_get_fast_channel_attributes_rx {
	__le32 status;
	__le32 flags;
#define DOORBELL_REG_WIDTH(f)		(FIELD_GET(GENMASK(2, 1), (f)))
#define DOORBELL_REG_WIDTH_RESERVED	0x3
#define SUPPORTS_DOORBELL(f)		(FIELD_GET(BIT(0), (f)))
	__le32 offset_low;
	__le32 offset_high;
	__le32 size;
	__le32 db_addr_low;
	__le32 db_addr_high;
	__le32 db_id;
};

static void rpmi_perf_fastchannel_db_ring(struct rpmi_fc_db_info *db)
{
	if (!db || !db->addr)
		return;

	switch (db->width) {
	case 8:
		iowrite8((u8)db->set, db->addr);
		break;
	case 16:
		iowrite16((u16)db->set, db->addr);
		break;
	case 32:
		iowrite32(db->set, db->addr);
		break;
	}
}

static int rpmi_perf_send_message(struct rpmi_ctx *mpxy_ctx,
				  struct rpmi_mbox_message *msg)
{
	guard(mutex)(&mpxy_ctx->lock);

	return rpmi_mbox_send_message(mpxy_ctx->chan, msg);
}

static int rpmi_perf_get_num_domains(struct rpmi_ctx *mpxy_ctx, u32 *domain)
{
	struct rpmi_perf_get_num_domain_rx rx = { };
	struct rpmi_mbox_message msg;
	int ret;

	rpmi_mbox_init_send_with_response(&msg, RPMI_PERF_SRV_GET_NUM_DOMAINS,
					  NULL, 0, &rx, sizeof(rx));
	ret = rpmi_perf_send_message(mpxy_ctx, &msg);
	if (ret)
		return ret;

	if (rx.status)
		return rpmi_to_linux_error(le32_to_cpu(rx.status));

	if (msg.data.out_response_len < sizeof(rx))
		return -EPROTO;

	*domain = le32_to_cpu(rx.num_domains);

	return 0;
}

static int rpmi_perf_get_attrs(struct rpmi_perf *mpxy_perf, struct rpmi_perf_domain *perf_domain)
{
	struct rpmi_perf_get_attrs_tx tx;
	struct rpmi_perf_get_attrs_rx rx = { };
	struct rpmi_mbox_message msg;
	u32 flags;
	int ret;

	tx.domain_id = cpu_to_le32(perf_domain->id);

	rpmi_mbox_init_send_with_response(&msg, RPMI_PERF_SRV_GET_ATTRIBUTES,
					  &tx, sizeof(tx), &rx, sizeof(rx));
	ret = rpmi_perf_send_message(perf_domain->mpxy_ctx, &msg);
	if (ret)
		return ret;

	if (rx.status)
		return rpmi_to_linux_error(le32_to_cpu(rx.status));

	if (msg.data.out_response_len < sizeof(rx))
		return -EPROTO;

	flags = le32_to_cpu(rx.flags);

	perf_domain->set_limit = PERF_LIMIT_SETTING(flags);
	perf_domain->set_level = PERF_LEVEL_SETTING(flags);
	perf_domain->has_fastchannels = mpxy_perf->fast_channel.size &&
					(perf_domain->set_limit || perf_domain->set_level) ?
					FAST_CHANNEL_SUPPORT(flags) : false;
	perf_domain->level_count = le32_to_cpu(rx.num_levels);
	perf_domain->rate_limit_us = le32_to_cpu(rx.trans_latency_us);
	strscpy(perf_domain->name, rx.name, RPMI_PERF_DOMAIN_NAME_LEN);

	if (!perf_domain->level_count)
		return -EINVAL;

	perf_domain->level = devm_kcalloc(mpxy_perf->dev, perf_domain->level_count,
					  sizeof(struct rpmi_perf_level), GFP_KERNEL);
	if (!perf_domain->level)
		return -ENOMEM;

	return 0;
}

static int rpmi_perf_get_supported_levels(struct rpmi_perf_domain *perf_domain)
{
	struct rpmi_perf_get_supported_level_tx tx;
	struct rpmi_perf_get_supported_level_rx *rx;
	struct rpmi_mbox_message msg;
	struct rpmi_perf_level *level;
	u32 index = 0, remaining, returned;
	int ret = 0;

	/*
	 * A reply carries as many levels as fit in one message, so size it
	 * for the largest message the channel carries. max_msg_size is in
	 * bytes, and was checked to hold at least one level.
	 */
	rx = kzalloc(perf_domain->mpxy_ctx->max_msg_size, GFP_KERNEL);
	if (!rx)
		return -ENOMEM;

	do {
		tx.domain_id = cpu_to_le32(perf_domain->id);
		tx.level_index = cpu_to_le32(index);

		rpmi_mbox_init_send_with_response(&msg, RPMI_PERF_SRV_GET_SUPPORTED_LEVELS,
						  &tx, sizeof(tx), rx,
						  perf_domain->mpxy_ctx->max_msg_size);
		ret = rpmi_perf_send_message(perf_domain->mpxy_ctx, &msg);
		if (ret)
			goto exit;

		if (rx->status) {
			ret = rpmi_to_linux_error(le32_to_cpu(rx->status));
			goto exit;
		}

		if (msg.data.out_response_len < sizeof(*rx)) {
			ret = -EPROTO;
			goto exit;
		}

		remaining = le32_to_cpu(rx->remaining);
		returned = le32_to_cpu(rx->returned);

		if (!returned || returned > perf_domain->level_count - index ||
		    returned > (msg.data.out_response_len - sizeof(*rx)) /
			       sizeof(rx->level[0]) ||
		    remaining != perf_domain->level_count - index - returned) {
			ret = -EPROTO;
			goto exit;
		}

		for (u32 i = 0; i < returned; i++) {
			level = &perf_domain->level[index + i];
			level->index = le32_to_cpu(rx->level[i].index);
			level->clock_freq = le32_to_cpu(rx->level[i].clock_freq);
			level->power_cost = le32_to_cpu(rx->level[i].power_cost);
			level->trans_latency_us = le32_to_cpu(rx->level[i].trans_latency_us);
		}

		index += returned;

	} while (remaining);

exit:
	kfree(rx);

	return ret;
}

static int rpmi_perf_get_fast_channel_regions(struct rpmi_ctx *mpxy_ctx, struct rpmi_perf *perf)
{
	struct rpmi_perf_get_fast_channel_region_rx rx = { };
	struct rpmi_mbox_message msg;
	int ret;

	rpmi_mbox_init_send_with_response(&msg, RPMI_PERF_SRV_GET_FAST_CHANNEL_REGION,
					  NULL, 0, &rx, sizeof(rx));
	ret = rpmi_perf_send_message(mpxy_ctx, &msg);
	if (ret)
		return ret;

	if (rx.status)
		return rpmi_to_linux_error(le32_to_cpu(rx.status));

	if (msg.data.out_response_len < sizeof(rx))
		return -EPROTO;

	perf->fast_channel.addr = le32_to_cpu(rx.addr_low) |
				  ((u64)le32_to_cpu(rx.addr_high) << 32);
	perf->fast_channel.size = le32_to_cpu(rx.size_low) |
				  ((u64)le32_to_cpu(rx.size_high) << 32);

	/* Every fast channel is checked against this region, so it must not wrap. */
	if (perf->fast_channel.addr + perf->fast_channel.size < perf->fast_channel.addr) {
		perf->fast_channel.size = 0;
		return -EPROTO;
	}

	return 0;
}

static int rpmi_perf_extract_attributes(struct rpmi_perf_get_fast_channel_attributes_rx *rx,
					struct rpmi_perf_domain *domain,
					struct rpmi_fc_info *fc, u32 service_id)
{
	u64 region_size = domain->perf->fast_channel.size;
	u64 phys_offset, phys_addr, db_phys_addr;
	struct rpmi_fc_db_info *db = NULL;
	void __iomem *addr, *db_addr;
	u32 flags, size;
	u8 db_addr_width;
	int ret;

	flags = le32_to_cpu(rx->flags);
	phys_offset = le32_to_cpu(rx->offset_low);
	phys_offset |= (u64)le32_to_cpu(rx->offset_high) << 32;
	size = le32_to_cpu(rx->size);

	/*
	 * The fast channels all lie in the region PERF_GET_FAST_CHANNEL_REGION
	 * describes, at an offset from its base, so refuse one that does not
	 * fit in it or is too small for its payload, a 32-bit level.
	 */
	if (size < sizeof(u32) || phys_offset > region_size ||
	    size > region_size - phys_offset)
		return -EPROTO;

	phys_addr = domain->perf->fast_channel.addr + phys_offset;

	addr = devm_ioremap(domain->dev, phys_addr, size);
	if (!addr) {
		dev_err(domain->dev,
			"failed to get fastchannel virtual addr in domain: %d\n",
			domain->id);
		return -EADDRNOTAVAIL;
	}

	switch (service_id) {
	case RPMI_PERF_SRV_GET_LEVEL:
		fc->get_addr = addr;
		break;
	case RPMI_PERF_SRV_SET_LEVEL:
		if (SUPPORTS_DOORBELL(flags)) {
			db = devm_kzalloc(domain->dev,
					  sizeof(struct rpmi_fc_db_info),
					  GFP_KERNEL);
			if (!db) {
				ret = -ENOMEM;
				goto err_unmap;
			}

			db_addr_width = 1 << (DOORBELL_REG_WIDTH(flags) + 3);
			db_phys_addr = le32_to_cpu(rx->db_addr_low);
			db_phys_addr |= (u64)le32_to_cpu(rx->db_addr_high) << 32;

			/* The doorbell can be any register; map only its width. */
			db_addr = devm_ioremap(domain->dev, db_phys_addr,
					       db_addr_width / BITS_PER_BYTE);
			if (!db_addr) {
				devm_kfree(domain->dev, db);
				ret = -EADDRNOTAVAIL;
				goto err_unmap;
			}

			db->addr = db_addr;
			db->width = db_addr_width;
			db->set = le32_to_cpu(rx->db_id);
			fc->set_db = db;
		}
		fc->set_addr = addr;

		break;
	}

	return 0;

err_unmap:
	/*
	 * The platform asked for a doorbell, so without one a request would
	 * never reach it. Leave the service on the mailbox.
	 */
	devm_iounmap(domain->dev, addr);

	return ret;
}

static int rpmi_perf_get_fast_channel_attributes(struct rpmi_perf_domain *perf_domain,
						 struct rpmi_fc_info *fc,
						 u32 service_id)
{
	struct rpmi_perf_get_fast_channel_attributes_tx tx;
	struct rpmi_perf_get_fast_channel_attributes_rx rx = { };
	struct rpmi_mbox_message msg;
	int ret;

	tx.domain_id = cpu_to_le32(perf_domain->id);
	tx.service_id = cpu_to_le32(service_id);

	rpmi_mbox_init_send_with_response(&msg, RPMI_PERF_SRV_GET_FAST_CHANNEL_ATTRS,
					  &tx, sizeof(tx), &rx, sizeof(rx));
	ret = rpmi_perf_send_message(perf_domain->mpxy_ctx, &msg);
	if (ret)
		return ret;

	if (rx.status)
		return rpmi_to_linux_error(le32_to_cpu(rx.status));

	/*
	 * The doorbell words are unused without a doorbell, and a platform
	 * may leave them out of the reply, so only require them with one.
	 */
	if (msg.data.out_response_len < offsetof(typeof(rx), db_addr_low) ||
	    (SUPPORTS_DOORBELL(le32_to_cpu(rx.flags)) &&
	     msg.data.out_response_len < sizeof(rx)))
		return -EPROTO;

	/* A doorbell of the reserved width could not be rung at all. */
	if (SUPPORTS_DOORBELL(le32_to_cpu(rx.flags)) &&
	    DOORBELL_REG_WIDTH(le32_to_cpu(rx.flags)) == DOORBELL_REG_WIDTH_RESERVED)
		return -EPROTO;

	ret = rpmi_perf_extract_attributes(&rx, perf_domain, fc, service_id);

	return ret;
}

/*
 * A service without a fast channel goes through the mailbox, so a fast channel
 * that cannot be set up only costs speed. The platform may say it implements
 * none; any other failure is reported, since nothing else would show it.
 */
static void rpmi_perf_setup_fast_channel(struct rpmi_perf_domain *domain,
					 struct rpmi_fc_info *fc, u32 service_id)
{
	int ret;

	ret = rpmi_perf_get_fast_channel_attributes(domain, fc, service_id);
	if (ret && ret != -EOPNOTSUPP)
		dev_warn(domain->dev,
			 "perf domain #%u: no fast channel for service %#x: %d\n",
			 domain->id, service_id, ret);
}

static int rpmi_perf_cmp_freq(const void *a, const void *b)
{
	const struct rpmi_perf_level *la = a, *lb = b;

	return (la->clock_freq > lb->clock_freq) - (la->clock_freq < lb->clock_freq);
}

static int rpmi_perf_enumerate(struct rpmi_perf *mpxy_perf, struct rpmi_perf_domain *domain)
{
	struct rpmi_fc_info *fc;
	int ret, id;

	id = domain->id;

	ret = rpmi_perf_get_attrs(mpxy_perf, domain);
	if (ret) {
		dev_err(domain->dev,
			"Failed to get attributes of perf domain: #%u\n", id);
		return ret;
	}

	ret = rpmi_perf_get_supported_levels(domain);
	if (ret) {
		dev_err(domain->dev,
			"Failed to get supported level of perf domain: #%u\n", id);
		return ret;
	}

	/*
	 * The platform may return its levels in any order. A level index is a
	 * point on the platform's performance scale, so a higher index is a
	 * higher level, but the indices need not be contiguous. Keep the table
	 * in ascending order of frequency, which the frequency lookup and every
	 * front-end walking the levels rely on.
	 */
	sort(domain->level, domain->level_count, sizeof(domain->level[0]),
	     rpmi_perf_cmp_freq, NULL);

	if (domain->has_fastchannels) {
		fc = devm_kcalloc(domain->dev, RPMI_PERF_FC_MAX, sizeof(*fc), GFP_KERNEL);
		if (!fc)
			return -ENOMEM;

		if (domain->set_level)
			rpmi_perf_setup_fast_channel(domain, &fc[RPMI_PERF_FC_LEVEL],
						     RPMI_PERF_SRV_SET_LEVEL);

		rpmi_perf_setup_fast_channel(domain, &fc[RPMI_PERF_FC_LEVEL],
					     RPMI_PERF_SRV_GET_LEVEL);

		/*
		 * Limits always go through the mailbox, whose reply says
		 * whether the platform took them, so their fast channels are
		 * not set up.
		 */

		domain->fc_info = fc;
	}

	return 0;
}

/*
 * Domain accessors. A consumer only ever sees an opaque handle, so the
 * enumeration above stays private to this file.
 */

const char *rpmi_perf_domain_name(struct rpmi_perf_domain *pd)
{
	return pd->name;
}
EXPORT_SYMBOL_GPL(rpmi_perf_domain_name);

u32 rpmi_perf_domain_level_count(struct rpmi_perf_domain *pd)
{
	return pd->level_count;
}
EXPORT_SYMBOL_GPL(rpmi_perf_domain_level_count);

/**
 * rpmi_perf_domain_level_info() - get one level of a performance domain
 * @pd: performance domain
 * @idx: position in the level table, from 0 to rpmi_perf_domain_level_count() - 1
 * @level: filled in with the level
 *
 * The table is in ascending order of frequency. @idx is a position in it, not
 * an RPMI level index, which is in @level->index.
 *
 * Return: 0 on success, -EINVAL if @idx is out of range.
 */
int rpmi_perf_domain_level_info(struct rpmi_perf_domain *pd, u32 idx,
				struct rpmi_perf_level *level)
{
	if (idx >= pd->level_count)
		return -EINVAL;

	*level = pd->level[idx];

	return 0;
}
EXPORT_SYMBOL_GPL(rpmi_perf_domain_level_info);

bool rpmi_perf_domain_can_set_level(struct rpmi_perf_domain *pd)
{
	return pd->set_level;
}
EXPORT_SYMBOL_GPL(rpmi_perf_domain_can_set_level);

bool rpmi_perf_domain_has_fast_channel(struct rpmi_perf_domain *pd)
{
	return pd->fc_info && pd->fc_info[RPMI_PERF_FC_LEVEL].set_addr;
}
EXPORT_SYMBOL_GPL(rpmi_perf_domain_has_fast_channel);

u32 rpmi_perf_domain_trans_latency_us(struct rpmi_perf_domain *pd)
{
	return pd->rate_limit_us;
}
EXPORT_SYMBOL_GPL(rpmi_perf_domain_trans_latency_us);

/**
 * rpmi_perf_domain_by_id() - look up a performance domain
 * @perf: performance service group
 * @id: RPMI domain ID
 *
 * Return: the domain, or NULL if @id is out of range or the domain could not
 * be enumerated. Such a domain has no level table to work from, so no
 * consumer is ever handed one.
 */
struct rpmi_perf_domain *rpmi_perf_domain_by_id(struct rpmi_perf *perf, u32 id)
{
	if (!perf || id >= perf->num_domains || !perf->domain[id].enumerated)
		return NULL;

	return &perf->domain[id];
}
EXPORT_SYMBOL_GPL(rpmi_perf_domain_by_id);

u32 rpmi_perf_num_domains(struct rpmi_perf *perf)
{
	return perf ? perf->num_domains : 0;
}
EXPORT_SYMBOL_GPL(rpmi_perf_num_domains);

/**
 * rpmi_perf_domain_get_level() - read the level a domain is running at
 * @pd: performance domain
 * @level: where the level index is stored
 *
 * Reads through the fast channel when the domain has one, and over the
 * mailbox otherwise. Either way the value comes from the platform
 * microcontroller rather than from a cached copy.
 *
 * Return: 0 on success, a negative errno otherwise.
 */
int rpmi_perf_domain_get_level(struct rpmi_perf_domain *pd, u32 *level)
{
	struct rpmi_perf_get_level_tx tx;
	struct rpmi_perf_get_level_rx rx = { };
	struct rpmi_mbox_message msg;
	int ret;

	if (pd->fc_info && pd->fc_info[RPMI_PERF_FC_LEVEL].get_addr) {
		*level = ioread32(pd->fc_info[RPMI_PERF_FC_LEVEL].get_addr);
		return 0;
	}

	tx.domain_id = cpu_to_le32(pd->id);

	rpmi_mbox_init_send_with_response(&msg, RPMI_PERF_SRV_GET_LEVEL,
					  &tx, sizeof(tx), &rx, sizeof(rx));
	ret = rpmi_perf_send_message(pd->mpxy_ctx, &msg);
	if (ret)
		return ret;

	if (rx.status)
		return rpmi_to_linux_error(le32_to_cpu(rx.status));

	if (msg.data.out_response_len < sizeof(rx))
		return -EPROTO;

	*level = le32_to_cpu(rx.level_index);

	return 0;
}
EXPORT_SYMBOL_GPL(rpmi_perf_domain_get_level);

/**
 * rpmi_perf_domain_set_level() - ask a domain to run at a level
 * @pd: performance domain
 * @level: level index, as advertised by the domain
 *
 * May sleep, because it goes over the mailbox. Use
 * rpmi_perf_domain_set_level_fast() from a context that must not.
 *
 * Return: 0 on success, a negative errno otherwise.
 */
int rpmi_perf_domain_set_level(struct rpmi_perf_domain *pd, u32 level)
{
	struct rpmi_perf_set_level_tx tx;
	struct rpmi_perf_set_level_rx rx = { };
	struct rpmi_mbox_message msg;
	int ret;

	if (!pd->set_level)
		return -EOPNOTSUPP;

	tx.domain_id = cpu_to_le32(pd->id);
	tx.level_index = cpu_to_le32(level);

	rpmi_mbox_init_send_with_response(&msg, RPMI_PERF_SRV_SET_LEVEL,
					  &tx, sizeof(tx), &rx, sizeof(rx));
	ret = rpmi_perf_send_message(pd->mpxy_ctx, &msg);
	if (ret)
		return ret;

	if (rx.status)
		return rpmi_to_linux_error(le32_to_cpu(rx.status));

	if (msg.data.out_response_len < sizeof(rx))
		return -EPROTO;

	return 0;
}
EXPORT_SYMBOL_GPL(rpmi_perf_domain_set_level);

/**
 * rpmi_perf_domain_set_level_fast() - set a level without sleeping
 * @pd: performance domain
 * @level: level index, as advertised by the domain
 *
 * Writes the level through the domain's fast channel and rings its doorbell.
 * The platform microcontroller applies it asynchronously and there is no
 * status to read back, so this only reports whether the request was posted.
 *
 * Return: 0 on success, -EOPNOTSUPP if the domain has no fast channel.
 */
int rpmi_perf_domain_set_level_fast(struct rpmi_perf_domain *pd, u32 level)
{
	if (!rpmi_perf_domain_has_fast_channel(pd))
		return -EOPNOTSUPP;

	iowrite32(level, pd->fc_info[RPMI_PERF_FC_LEVEL].set_addr);
	rpmi_perf_fastchannel_db_ring(pd->fc_info[RPMI_PERF_FC_LEVEL].set_db);

	return 0;
}
EXPORT_SYMBOL_GPL(rpmi_perf_domain_set_level_fast);

int rpmi_perf_domain_get_limit(struct rpmi_perf_domain *pd, u32 *min, u32 *max)
{
	struct rpmi_perf_get_limit_tx tx;
	struct rpmi_perf_get_limit_rx rx = { };
	struct rpmi_mbox_message msg;
	int ret;

	tx.domain_id = cpu_to_le32(pd->id);

	rpmi_mbox_init_send_with_response(&msg, RPMI_PERF_SRV_GET_LIMIT,
					  &tx, sizeof(tx), &rx, sizeof(rx));
	ret = rpmi_perf_send_message(pd->mpxy_ctx, &msg);
	if (ret)
		return ret;

	if (rx.status)
		return rpmi_to_linux_error(le32_to_cpu(rx.status));

	if (msg.data.out_response_len < sizeof(rx))
		return -EPROTO;

	*max = le32_to_cpu(rx.max_level);
	*min = le32_to_cpu(rx.min_level);

	return 0;
}
EXPORT_SYMBOL_GPL(rpmi_perf_domain_get_limit);

int rpmi_perf_domain_set_limit(struct rpmi_perf_domain *pd, u32 min, u32 max)
{
	struct rpmi_perf_set_limit_tx tx;
	struct rpmi_perf_set_limit_rx rx = { };
	struct rpmi_mbox_message msg;
	int ret;

	if (!pd->set_limit)
		return -EOPNOTSUPP;

	tx.domain_id = cpu_to_le32(pd->id);
	tx.max_level = cpu_to_le32(max);
	tx.min_level = cpu_to_le32(min);

	rpmi_mbox_init_send_with_response(&msg, RPMI_PERF_SRV_SET_LIMIT,
					  &tx, sizeof(tx), &rx, sizeof(rx));
	ret = rpmi_perf_send_message(pd->mpxy_ctx, &msg);
	if (ret)
		return ret;

	if (rx.status)
		return rpmi_to_linux_error(le32_to_cpu(rx.status));

	if (msg.data.out_response_len < sizeof(rx))
		return -EPROTO;

	return 0;
}
EXPORT_SYMBOL_GPL(rpmi_perf_domain_set_limit);

static int rpmi_perf_search_freq(const void *id, const void *level)
{
	u32 khz = *(const u32 *)id;
	u32 freq = ((const struct rpmi_perf_level *)level)->clock_freq;

	return (khz > freq) - (khz < freq);
}

int rpmi_perf_domain_level_to_freq(struct rpmi_perf_domain *pd, u32 level, u32 *khz)
{
	/* The table is sorted by frequency, not by level index. */
	for (u32 i = 0; i < pd->level_count; i++) {
		if (pd->level[i].index == level) {
			*khz = pd->level[i].clock_freq;
			return 0;
		}
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(rpmi_perf_domain_level_to_freq);

int rpmi_perf_domain_freq_to_level(struct rpmi_perf_domain *pd, u32 khz, u32 *level)
{
	struct rpmi_perf_level *found;

	found = bsearch(&khz, pd->level, pd->level_count,
			sizeof(pd->level[0]), rpmi_perf_search_freq);
	if (!found)
		return -EINVAL;

	*level = found->index;

	return 0;
}
EXPORT_SYMBOL_GPL(rpmi_perf_domain_freq_to_level);

/**
 * rpmi_perf_domain_opps_add() - give @dev an OPP for every level of @pd
 * @pd: performance domain
 * @dev: device that runs in the domain
 *
 * The levels come from the platform microcontroller, so there is no
 * operating-points-v2 table in the device tree to parse. The OPP frequency is
 * the level's clock frequency in Hz and the OPP level is the RPMI level
 * index, which is what the set and get services take.
 *
 * Return: 0 on success, a negative errno otherwise.
 */
int rpmi_perf_domain_opps_add(struct rpmi_perf_domain *pd, struct device *dev)
{
	struct dev_pm_opp_data data = {};
	struct rpmi_perf_level *level;
	int idx, ret;

	for (level = pd->level, idx = 0; idx < pd->level_count; idx++, level++) {
		/* Frequency from RPMI is in kHz */
		data.freq = (unsigned long)level->clock_freq * 1000;
		data.level = level->index;

		ret = dev_pm_opp_add_dynamic(dev, &data);
		if (ret) {
			dev_warn(dev, "failed to add opp %luHz\n", data.freq);

			while (idx-- > 0) {
				/* Frequency from RPMI is in kHz */
				dev_pm_opp_remove(dev,
						  (unsigned long)(--level)->clock_freq * 1000);
			}
			return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(rpmi_perf_domain_opps_add);

static int rpmi_perf_attr_setup(struct device *dev, struct rpmi_ctx *mpxy_ctx)
{
	struct rpmi_mbox_message msg;
	int ret;

	/* Validate RPMI specification version */
	rpmi_mbox_init_get_attribute(&msg, RPMI_MBOX_ATTR_SPEC_VERSION);
	ret = rpmi_perf_send_message(mpxy_ctx, &msg);
	if (ret) {
		dev_dbg(dev, "Failed to get spec version\n");
		return ret;
	}

	if (msg.attr.value < RPMI_MKVER(1, 0)) {
		dev_dbg(dev,
			"msg protocol version mismatch, expected 0x%x, found 0x%x\n",
			RPMI_MKVER(1, 0), msg.attr.value);
		return -EINVAL;
	}

	/* Validate performance service group ID */
	rpmi_mbox_init_get_attribute(&msg, RPMI_MBOX_ATTR_SERVICEGROUP_ID);
	ret = rpmi_perf_send_message(mpxy_ctx, &msg);
	if (ret) {
		dev_dbg(dev, "Failed to get service group ID\n");
		return ret;
	}

	if (msg.attr.value != RPMI_SRVGRP_PERFORMANCE) {
		dev_dbg(dev,
			"service group match failed, expected 0x%x, found 0x%x\n",
			RPMI_SRVGRP_PERFORMANCE, msg.attr.value);
		return -EINVAL;
	}

	/* Validate performance service group version */
	rpmi_mbox_init_get_attribute(&msg, RPMI_MBOX_ATTR_SERVICEGROUP_VERSION);
	ret = rpmi_perf_send_message(mpxy_ctx, &msg);
	if (ret) {
		dev_dbg(dev, "Failed to get service group version\n");
		return ret;
	}

	if (msg.attr.value < RPMI_MKVER(1, 0)) {
		dev_dbg(dev,
			"service group version failed, expected 0x%x, found 0x%x\n",
			RPMI_MKVER(1, 0), msg.attr.value);
		return -EINVAL;
	}

	/* Get max message size */
	rpmi_mbox_init_get_attribute(&msg, RPMI_MBOX_ATTR_MAX_MSG_DATA_SIZE);
	ret = rpmi_perf_send_message(mpxy_ctx, &msg);
	if (ret) {
		dev_dbg(dev, "Failed to get max message data size\n");
		return ret;
	}

	if (msg.attr.value < sizeof(struct rpmi_perf_get_supported_level_rx) +
			     sizeof(struct rpmi_perf_supported_level)) {
		dev_dbg(dev, "max message data size %u too small\n",
			msg.attr.value);
		return -EINVAL;
	}
	mpxy_ctx->max_msg_size = msg.attr.value;

	return 0;
}

static void rpmi_perf_mbox_chan_release(void *data)
{
	mbox_free_channel((struct mbox_chan *)data);
}

/*
 * cpufreq has no device tree node of its own: a CPU names its domain through
 * "performance-domains" on the CPU node. Create the device it binds to, but
 * only when some CPU actually points back here.
 */
static bool rpmi_perf_cpus_present(struct device *dev)
{
	struct device_node *cpu_np;
	struct of_phandle_args args;
	int ret;

	for_each_of_cpu_node(cpu_np) {
		ret = of_parse_phandle_with_args(cpu_np, "performance-domains",
						 "#performance-domain-cells", 0,
						 &args);
		if (ret)
			continue;

		if (args.np == dev_of_node(dev)) {
			of_node_put(args.np);
			of_node_put(cpu_np);
			return true;
		}
		of_node_put(args.np);
	}

	return false;
}

static void rpmi_perf_frontend_unregister(void *data)
{
	platform_device_unregister((struct platform_device *)data);
}

static int rpmi_perf_cpufreq_register(struct device *dev, struct rpmi_perf *perf)
{
	struct platform_device *pdev;

	if (!IS_ENABLED(CONFIG_RISCV_RPMI_CPUFREQ) || !rpmi_perf_cpus_present(dev))
		return 0;

	pdev = platform_device_register_data(dev, "riscv-rpmi-cpufreq",
					     PLATFORM_DEVID_AUTO, &perf,
					     sizeof(perf));
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);

	return devm_add_action_or_reset(dev, rpmi_perf_frontend_unregister, pdev);
}

/*
 * A device that is not a CPU names its domain through "power-domains", which
 * the power domain front-end serves on this node. Create the device it binds
 * to, but only when the node is a power domain provider at all.
 */
static int rpmi_perf_genpd_register(struct device *dev, struct rpmi_perf *perf)
{
	struct platform_device *pdev;

	if (!IS_ENABLED(CONFIG_RISCV_RPMI_PERF_DOMAIN) ||
	    !of_property_present(dev_of_node(dev), "#power-domain-cells"))
		return 0;

	pdev = platform_device_register_data(dev, "riscv-rpmi-perf-domain",
					     PLATFORM_DEVID_AUTO, &perf,
					     sizeof(perf));
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);

	return devm_add_action_or_reset(dev, rpmi_perf_frontend_unregister, pdev);
}

static int rpmi_perf_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rpmi_perf *mpxy_perf;
	struct rpmi_ctx *mpxy_ctx;
	u32 num_domains = 0;
	int ret;
	u32 i;

	mpxy_ctx = devm_kzalloc(dev, sizeof(*mpxy_ctx), GFP_KERNEL);
	if (!mpxy_ctx)
		return -ENOMEM;

	ret = devm_mutex_init(dev, &mpxy_ctx->lock);
	if (ret)
		return ret;

	/* Setup mailbox client */
	mpxy_ctx->client.dev		= dev;
	mpxy_ctx->client.rx_callback	= NULL;
	mpxy_ctx->client.tx_block	= false;
	mpxy_ctx->client.knows_txdone	= true;
	mpxy_ctx->client.tx_tout	= 0;

	/* Request mailbox channel */
	mpxy_ctx->chan = mbox_request_channel(&mpxy_ctx->client, 0);
	if (IS_ERR(mpxy_ctx->chan))
		return PTR_ERR(mpxy_ctx->chan);

	ret = devm_add_action_or_reset(dev, rpmi_perf_mbox_chan_release,
				       mpxy_ctx->chan);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to add rpmi mbox channel cleanup\n");

	ret = rpmi_perf_attr_setup(dev, mpxy_ctx);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to verify RPMI attribute\n");

	/* Get number of performance domain */
	ret = rpmi_perf_get_num_domains(mpxy_ctx, &num_domains);
	if (ret)
		return dev_err_probe(dev, ret,
				     "invalid number of perf domains\n");

	if (!num_domains)
		return dev_err_probe(dev, -EINVAL, "No perf domains found!\n");

	mpxy_perf = devm_kzalloc(dev, sizeof(*mpxy_perf), GFP_KERNEL);
	if (!mpxy_perf)
		return -ENOMEM;

	mpxy_perf->domain = devm_kcalloc(dev, num_domains,
					 sizeof(struct rpmi_perf_domain),
					 GFP_KERNEL);
	if (!mpxy_perf->domain)
		return -ENOMEM;

	mpxy_perf->num_domains = num_domains;
	mpxy_perf->dev = dev;

	/*
	 * Without a fast-channel region every domain goes through the mailbox.
	 * The platform may implement none; any other failure is reported.
	 */
	ret = rpmi_perf_get_fast_channel_regions(mpxy_ctx, mpxy_perf);
	if (ret && ret != -EOPNOTSUPP)
		dev_warn(dev, "no fast channel region, using the mailbox: %d\n", ret);

	for (i = 0; i < num_domains; i++) {
		struct rpmi_perf_domain *domain = &mpxy_perf->domain[i];

		domain->id = i;
		domain->mpxy_ctx = mpxy_ctx;
		domain->dev = dev;
		domain->perf = mpxy_perf;

		domain->enumerated = !rpmi_perf_enumerate(mpxy_perf, domain);
	}

	dev_set_drvdata(dev, mpxy_perf);

	ret = rpmi_perf_cpufreq_register(dev, mpxy_perf);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register cpufreq device\n");

	ret = rpmi_perf_genpd_register(dev, mpxy_perf);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register power domain device\n");

	dev_info(dev, "%d MPXY performance domains registered\n", num_domains);

	return 0;
}

static const struct of_device_id rpmi_perf_of_match[] = {
	{ .compatible = "riscv,rpmi-performance" },
	{ },
};

MODULE_DEVICE_TABLE(of, rpmi_perf_of_match);

static struct platform_driver rpmi_perf_platdrv = {
	.driver = {
		.name = "riscv-rpmi-performance",
		.of_match_table = rpmi_perf_of_match,
	},
	.probe = rpmi_perf_probe,
};

module_platform_driver(rpmi_perf_platdrv);

MODULE_AUTHOR("Joshua Yeong <joshua.yeong@starfivetech.com>");
MODULE_DESCRIPTION("RISC-V RPMI performance service group core");
MODULE_LICENSE("GPL");
