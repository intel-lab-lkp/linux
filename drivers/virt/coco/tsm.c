// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. All rights reserved. */

#include <linux/tsm.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/rwsem.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/cleanup.h>
#include <linux/configfs.h>
#include <linux/ctype.h>
#include <crypto/hash_info.h>
#include <crypto/hash.h>

static struct tsm_provider {
	const struct tsm_ops *ops;
	void *data;
} provider;
static DECLARE_RWSEM(tsm_rwsem);

/**
 * DOC: Trusted Security Module (TSM) Attestation Report Interface
 *
 * The TSM report interface is a common provider of blobs that facilitate
 * attestation of a TVM (confidential computing guest) by an attestation
 * service. A TSM report combines a user-defined blob (likely a public-key with
 * a nonce for a key-exchange protocol) with a signed attestation report. That
 * combined blob is then used to obtain secrets provided by an agent that can
 * validate the attestation report. The expectation is that this interface is
 * invoked infrequently, however configfs allows for multiple agents to
 * own their own report generation instances to generate reports as
 * often as needed.
 *
 * The attestation report format is TSM provider specific, when / if a standard
 * materializes that can be published instead of the vendor layout. Until then
 * the 'provider' attribute indicates the format of 'outblob', and optionally
 * 'auxblob' and 'manifestblob'.
 */

struct tsm_report_state {
	struct tsm_report report;
	unsigned long write_generation;
	unsigned long read_generation;
	struct config_item cfg;
};

enum tsm_data_select {
	TSM_REPORT,
	TSM_CERTS,
	TSM_MANIFEST,
};

static struct tsm_report *to_tsm_report(struct config_item *cfg)
{
	struct tsm_report_state *state =
		container_of(cfg, struct tsm_report_state, cfg);

	return &state->report;
}

static struct tsm_report_state *to_state(struct tsm_report *report)
{
	return container_of(report, struct tsm_report_state, report);
}

static int try_advance_write_generation(struct tsm_report *report)
{
	struct tsm_report_state *state = to_state(report);

	lockdep_assert_held_write(&tsm_rwsem);

	/*
	 * Malicious or broken userspace has written enough times for
	 * read_generation == write_generation by modular arithmetic without an
	 * interim read. Stop accepting updates until the current report
	 * configuration is read.
	 */
	if (state->write_generation == state->read_generation - 1)
		return -EBUSY;
	state->write_generation++;
	return 0;
}

static ssize_t tsm_report_privlevel_store(struct config_item *cfg,
					  const char *buf, size_t len)
{
	struct tsm_report *report = to_tsm_report(cfg);
	unsigned int val;
	int rc;

	rc = kstrtouint(buf, 0, &val);
	if (rc)
		return rc;

	/*
	 * The valid privilege levels that a TSM might accept, if it accepts a
	 * privilege level setting at all, are a max of TSM_PRIVLEVEL_MAX (see
	 * SEV-SNP GHCB) and a minimum of a TSM selected floor value no less
	 * than 0.
	 */
	if (provider.ops->privlevel_floor > val || val > TSM_PRIVLEVEL_MAX)
		return -EINVAL;

	guard(rwsem_write)(&tsm_rwsem);
	rc = try_advance_write_generation(report);
	if (rc)
		return rc;
	report->desc.privlevel = val;

	return len;
}
CONFIGFS_ATTR_WO(tsm_report_, privlevel);

static ssize_t tsm_report_privlevel_floor_show(struct config_item *cfg,
					       char *buf)
{
	guard(rwsem_read)(&tsm_rwsem);
	return sysfs_emit(buf, "%u\n", provider.ops->privlevel_floor);
}
CONFIGFS_ATTR_RO(tsm_report_, privlevel_floor);

static ssize_t tsm_report_service_provider_store(struct config_item *cfg,
						 const char *buf, size_t len)
{
	struct tsm_report *report = to_tsm_report(cfg);
	size_t sp_len;
	char *sp;
	int rc;

	guard(rwsem_write)(&tsm_rwsem);
	rc = try_advance_write_generation(report);
	if (rc)
		return rc;

	sp_len = (buf[len - 1] != '\n') ? len : len - 1;

	sp = kstrndup(buf, sp_len, GFP_KERNEL);
	if (!sp)
		return -ENOMEM;
	kfree(report->desc.service_provider);

	report->desc.service_provider = sp;

	return len;
}
CONFIGFS_ATTR_WO(tsm_report_, service_provider);

static ssize_t tsm_report_service_guid_store(struct config_item *cfg,
					     const char *buf, size_t len)
{
	struct tsm_report *report = to_tsm_report(cfg);
	int rc;

	guard(rwsem_write)(&tsm_rwsem);
	rc = try_advance_write_generation(report);
	if (rc)
		return rc;

	report->desc.service_guid = guid_null;

	rc = guid_parse(buf, &report->desc.service_guid);
	if (rc)
		return rc;

	return len;
}
CONFIGFS_ATTR_WO(tsm_report_, service_guid);

static ssize_t
tsm_report_service_manifest_version_store(struct config_item *cfg,
					  const char *buf, size_t len)
{
	struct tsm_report *report = to_tsm_report(cfg);
	unsigned int val;
	int rc;

	rc = kstrtouint(buf, 0, &val);
	if (rc)
		return rc;

	guard(rwsem_write)(&tsm_rwsem);
	rc = try_advance_write_generation(report);
	if (rc)
		return rc;
	report->desc.service_manifest_version = val;

	return len;
}
CONFIGFS_ATTR_WO(tsm_report_, service_manifest_version);

static ssize_t tsm_report_inblob_write(struct config_item *cfg, const void *buf,
				       size_t count)
{
	struct tsm_report *report = to_tsm_report(cfg);
	int rc;

	guard(rwsem_write)(&tsm_rwsem);
	rc = try_advance_write_generation(report);
	if (rc)
		return rc;

	report->desc.inblob_len = count;
	memcpy(report->desc.inblob, buf, count);
	return count;
}
CONFIGFS_BIN_ATTR_WO(tsm_report_, inblob, NULL, TSM_INBLOB_MAX);

static ssize_t tsm_report_generation_show(struct config_item *cfg, char *buf)
{
	struct tsm_report *report = to_tsm_report(cfg);
	struct tsm_report_state *state = to_state(report);

	guard(rwsem_read)(&tsm_rwsem);
	return sysfs_emit(buf, "%lu\n", state->write_generation);
}
CONFIGFS_ATTR_RO(tsm_report_, generation);

static ssize_t tsm_report_provider_show(struct config_item *cfg, char *buf)
{
	guard(rwsem_read)(&tsm_rwsem);
	return sysfs_emit(buf, "%s\n", provider.ops->name);
}
CONFIGFS_ATTR_RO(tsm_report_, provider);

static ssize_t __read_report(struct tsm_report *report, void *buf, size_t count,
			     enum tsm_data_select select)
{
	loff_t offset = 0;
	ssize_t len;
	u8 *out;

	if (select == TSM_REPORT) {
		out = report->outblob;
		len = report->outblob_len;
	} else if (select == TSM_MANIFEST) {
		out = report->manifestblob;
		len = report->manifestblob_len;
	} else {
		out = report->auxblob;
		len = report->auxblob_len;
	}

	/*
	 * Recall that a NULL @buf is configfs requesting the size of
	 * the buffer.
	 */
	if (!buf)
		return len;
	return memory_read_from_buffer(buf, count, &offset, out, len);
}

static ssize_t read_cached_report(struct tsm_report *report, void *buf,
				  size_t count, enum tsm_data_select select)
{
	struct tsm_report_state *state = to_state(report);

	guard(rwsem_read)(&tsm_rwsem);
	if (!report->desc.inblob_len)
		return -EINVAL;

	/*
	 * A given TSM backend always fills in ->outblob regardless of
	 * whether the report includes an auxblob/manifestblob or not.
	 */
	if (!report->outblob ||
	    state->read_generation != state->write_generation)
		return -EWOULDBLOCK;

	return __read_report(report, buf, count, select);
}

static ssize_t tsm_report_read(struct tsm_report *report, void *buf,
			       size_t count, enum tsm_data_select select)
{
	struct tsm_report_state *state = to_state(report);
	const struct tsm_ops *ops;
	ssize_t rc;

	/* try to read from the existing report if present and valid... */
	rc = read_cached_report(report, buf, count, select);
	if (rc >= 0 || rc != -EWOULDBLOCK)
		return rc;

	/* slow path, report may need to be regenerated... */
	guard(rwsem_write)(&tsm_rwsem);
	ops = provider.ops;
	if (!ops)
		return -ENOTTY;
	if (!report->desc.inblob_len)
		return -EINVAL;

	/* did another thread already generate this report? */
	if (report->outblob &&
	    state->read_generation == state->write_generation)
		goto out;

	kvfree(report->outblob);
	kvfree(report->auxblob);
	kvfree(report->manifestblob);
	report->outblob = NULL;
	report->auxblob = NULL;
	report->manifestblob = NULL;
	rc = ops->report_new(report, provider.data);
	if (rc < 0)
		return rc;
	state->read_generation = state->write_generation;
out:
	return __read_report(report, buf, count, select);
}

static ssize_t tsm_report_outblob_read(struct config_item *cfg, void *buf,
				       size_t count)
{
	struct tsm_report *report = to_tsm_report(cfg);

	return tsm_report_read(report, buf, count, TSM_REPORT);
}
CONFIGFS_BIN_ATTR_RO(tsm_report_, outblob, NULL, TSM_OUTBLOB_MAX);

static ssize_t tsm_report_auxblob_read(struct config_item *cfg, void *buf,
				       size_t count)
{
	struct tsm_report *report = to_tsm_report(cfg);

	return tsm_report_read(report, buf, count, TSM_CERTS);
}
CONFIGFS_BIN_ATTR_RO(tsm_report_, auxblob, NULL, TSM_OUTBLOB_MAX);

static ssize_t tsm_report_manifestblob_read(struct config_item *cfg, void *buf,
					    size_t count)
{
	struct tsm_report *report = to_tsm_report(cfg);

	return tsm_report_read(report, buf, count, TSM_MANIFEST);
}
CONFIGFS_BIN_ATTR_RO(tsm_report_, manifestblob, NULL, TSM_OUTBLOB_MAX);

static struct configfs_attribute *tsm_report_attrs[] = {
	[TSM_REPORT_GENERATION] = &tsm_report_attr_generation,
	[TSM_REPORT_PROVIDER] = &tsm_report_attr_provider,
	[TSM_REPORT_PRIVLEVEL] = &tsm_report_attr_privlevel,
	[TSM_REPORT_PRIVLEVEL_FLOOR] = &tsm_report_attr_privlevel_floor,
	[TSM_REPORT_SERVICE_PROVIDER] = &tsm_report_attr_service_provider,
	[TSM_REPORT_SERVICE_GUID] = &tsm_report_attr_service_guid,
	[TSM_REPORT_SERVICE_MANIFEST_VER] =
		&tsm_report_attr_service_manifest_version,
	NULL,
};

static struct configfs_bin_attribute *tsm_report_bin_attrs[] = {
	[TSM_REPORT_INBLOB] = &tsm_report_attr_inblob,
	[TSM_REPORT_OUTBLOB] = &tsm_report_attr_outblob,
	[TSM_REPORT_AUXBLOB] = &tsm_report_attr_auxblob,
	[TSM_REPORT_MANIFESTBLOB] = &tsm_report_attr_manifestblob,
	NULL,
};

static void tsm_report_item_release(struct config_item *cfg)
{
	struct tsm_report *report = to_tsm_report(cfg);
	struct tsm_report_state *state = to_state(report);

	kvfree(report->manifestblob);
	kvfree(report->auxblob);
	kvfree(report->outblob);
	kfree(report->desc.service_provider);
	kfree(state);
}

static struct configfs_item_operations tsm_report_item_ops = {
	.release = tsm_report_item_release,
};

static bool tsm_report_is_visible(struct config_item *item,
				  struct configfs_attribute *attr, int n)
{
	guard(rwsem_read)(&tsm_rwsem);
	if (!provider.ops)
		return false;

	if (!provider.ops->report_attr_visible)
		return true;

	return provider.ops->report_attr_visible(n);
}

static bool tsm_report_is_bin_visible(struct config_item *item,
				      struct configfs_bin_attribute *attr,
				      int n)
{
	guard(rwsem_read)(&tsm_rwsem);
	if (!provider.ops)
		return false;

	if (!provider.ops->report_bin_attr_visible)
		return true;

	return provider.ops->report_bin_attr_visible(n);
}

static struct configfs_group_operations tsm_report_attr_group_ops = {
	.is_visible = tsm_report_is_visible,
	.is_bin_visible = tsm_report_is_bin_visible,
};

static const struct config_item_type tsm_report_type = {
	.ct_owner = THIS_MODULE,
	.ct_bin_attrs = tsm_report_bin_attrs,
	.ct_attrs = tsm_report_attrs,
	.ct_item_ops = &tsm_report_item_ops,
	.ct_group_ops = &tsm_report_attr_group_ops,
};

static struct config_item *tsm_report_make_item(struct config_group *group,
						const char *name)
{
	struct tsm_report_state *state;

	guard(rwsem_read)(&tsm_rwsem);
	if (!provider.ops)
		return ERR_PTR(-ENXIO);

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return ERR_PTR(-ENOMEM);

	config_item_init_type_name(&state->cfg, name, &tsm_report_type);
	return &state->cfg;
}

static struct configfs_group_operations tsm_report_group_ops = {
	.make_item = tsm_report_make_item,
};

static const struct config_item_type tsm_reports_type = {
	.ct_owner = THIS_MODULE,
	.ct_group_ops = &tsm_report_group_ops,
};

static const struct config_item_type tsm_root_group_type = {
	.ct_owner = THIS_MODULE,
};

static struct configfs_subsystem tsm_configfs = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "tsm",
			.ci_type = &tsm_root_group_type,
		},
	},
	.su_mutex = __MUTEX_INITIALIZER(tsm_configfs.su_mutex),
};

int tsm_register(const struct tsm_ops *ops, void *priv)
{
	const struct tsm_ops *conflict;

	guard(rwsem_write)(&tsm_rwsem);
	conflict = provider.ops;
	if (conflict) {
		pr_err("\"%s\" ops already registered\n", conflict->name);
		return -EBUSY;
	}

	provider.ops = ops;
	provider.data = priv;
	return 0;
}
EXPORT_SYMBOL_GPL(tsm_register);

int tsm_unregister(const struct tsm_ops *ops)
{
	guard(rwsem_write)(&tsm_rwsem);
	if (ops != provider.ops)
		return -EBUSY;
	provider.ops = NULL;
	provider.data = NULL;
	return 0;
}
EXPORT_SYMBOL_GPL(tsm_unregister);

enum _rtmr_bin_attr_index {
	_RTMR_BATTR_DIGEST,
	_RTMR_BATTR_LOG,
	_RTMR_BATTR__COUNT,
};

struct _rtmr {
	struct kobject kobj;
	struct bin_attribute battrs[_RTMR_BATTR__COUNT];
	bool log_in_sync;
};

struct _mr_provider {
	struct kset kset;
	struct rw_semaphore rwsem;
	struct bin_attribute *static_mrs;
	struct tsm_measurement_provider *provider;
	bool in_sync;
};

static inline const struct tsm_measurement_register *
_rtmr_mr(const struct _rtmr *rtmr)
{
	return (struct tsm_measurement_register *)rtmr
		->battrs[_RTMR_BATTR_DIGEST]
		.private;
}

static inline char *_rtmr_log(const struct _rtmr *rtmr)
{
	return (char *)rtmr->battrs[_RTMR_BATTR_LOG].private;
}

static inline size_t _rtmr_log_size(const struct _rtmr *rtmr)
{
	return rtmr->battrs[_RTMR_BATTR_LOG].size;
}

static inline void _rtmr_log_set_buf(struct _rtmr *rtmr, char *log)
{
	rtmr->battrs[_RTMR_BATTR_LOG].private = log;
}

static inline void _rtmr_log_inc_size(struct _rtmr *rtmr, size_t size)
{
	rtmr->battrs[_RTMR_BATTR_LOG].size += size;
}

static inline int _rtmr_log_update_attribute(struct _rtmr *rtmr)
{
	struct bin_attribute *attrs_to_update[] = {
		&rtmr->battrs[_RTMR_BATTR_LOG],
		NULL,
	};
	struct attribute_group agrp = {
		.bin_attrs = attrs_to_update,
	};
	return sysfs_update_group(&rtmr->kobj, &agrp);
}

static inline struct _mr_provider *
_mr_to_provider(const struct tsm_measurement_register *mr, struct kobject *kobj)
{
	if (!(mr->mr_flags & TSM_MR_F_X))
		return container_of(kobj, struct _mr_provider, kset.kobj);
	else
		return container_of(kobj->kset, struct _mr_provider, kset);
}

static inline int _call_refresh(struct _mr_provider *pvd,
				const struct tsm_measurement_register *mr)
{
	int rc = pvd->provider->refresh(pvd->provider, mr);
	if (rc)
		pr_warn(KBUILD_MODNAME ": %s.extend(%s) failed %d\n",
			kobject_name(&pvd->kset.kobj), mr->mr_name, rc);
	return rc;
}

static inline int _call_extend(struct _mr_provider *pvd,
			       const struct tsm_measurement_register *mr,
			       const u8 *data)
{
	int rc = pvd->provider->extend(pvd->provider, mr, data);
	if (rc)
		pr_warn(KBUILD_MODNAME ": %s.extend(%s) failed %d\n",
			kobject_name(&pvd->kset.kobj), mr->mr_name, rc);
	return rc;
}

static ssize_t hash_algo_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *page)
{
	struct _rtmr *rtmr;
	rtmr = container_of(kobj, typeof(*rtmr), kobj);
	return sysfs_emit(page, "%s", hash_algo_name[_rtmr_mr(rtmr)->mr_hash]);
}

static ssize_t _mr_read(struct file *filp, struct kobject *kobj,
			struct bin_attribute *attr, char *page, loff_t off,
			size_t count)
{
	const struct tsm_measurement_register *mr;
	struct _mr_provider *pvd;
	int rc;

	if (off < 0 || off > attr->size)
		return -EINVAL;

	count = min(count, attr->size - (size_t)off);
	if (!count)
		return count;

	mr = (typeof(mr))attr->private;
	BUG_ON(mr->mr_size != attr->size);

	pvd = _mr_to_provider(mr, kobj);
	rc = down_read_interruptible(&pvd->rwsem);
	if (rc)
		return rc;

	if ((mr->mr_flags & TSM_MR_F_L) && !pvd->in_sync) {
		up_read(&pvd->rwsem);

		rc = down_write_killable(&pvd->rwsem);
		if (rc)
			return rc;

		if (!pvd->in_sync) {
			rc = _call_refresh(pvd, mr);
			pvd->in_sync = !rc;
		}

		downgrade_write(&pvd->rwsem);
	}

	if (!rc)
		memcpy(page, mr->mr_value + off, count);
	else
		pr_debug(KBUILD_MODNAME ": refresh(%s,%s)=%d\n",
			 kobject_name(&pvd->kset.kobj), mr->mr_name, rc);

	up_read(&pvd->rwsem);
	return rc ?: count;
}

#define _EVENTLOG_GRANULARITY HPAGE_SIZE

static ssize_t _log_extend_line(struct _rtmr *rtmr, const char *line,
				const char *end, int newlines,
				struct crypto_shash *tfm)
{
	struct _mr_provider *pvd;
	pvd = container_of(rtmr->kobj.kset, typeof(*pvd), kset);
	lockdep_assert_held_write(&pvd->rwsem);

	BUG_ON(line > end);

	while (line < end && isspace(line[0]))
		++line;
	while (line < end && isspace(end[-1]))
		--end;
	if (line == end)
		return 0;

	ssize_t count = end - line;
	char *log = _rtmr_log(rtmr);
	size_t needed = _rtmr_log_size(rtmr) + count + newlines;
	if (ksize(log) < needed) {
		log = krealloc(log,
			       ALIGN(needed + _EVENTLOG_GRANULARITY / 2,
				     _EVENTLOG_GRANULARITY),
			       GFP_KERNEL);
		if (!log)
			return -ENOMEM;

		_rtmr_log_set_buf(rtmr, log);
	}

	log += _rtmr_log_size(rtmr);
	for (int i = 0; i < newlines; ++i)
		*log++ = '\n';

	if (*line != '#') {
		u8 digest[SHA512_DIGEST_SIZE];
		BUG_ON(tfm == NULL);
		BUG_ON(sizeof(digest) < crypto_shash_digestsize(tfm));

		int rc = crypto_shash_tfm_digest(tfm, line, count, digest);
		if (!rc)
			rc = _call_extend(pvd, _rtmr_mr(rtmr), digest);
		if (rc)
			return rc;
	}

	memcpy(log, line, count);
	log[count] = '\n';
	_rtmr_log_inc_size(rtmr, count += newlines + 1);

	return _rtmr_log_update_attribute(rtmr) ?: count;
}

static inline size_t snprint_hex(char *sbuf, ssize_t size, const u8 *data,
				 size_t len)
{
	BUG_ON(size < len * 2);
	size_t ret = 0;
	for (size_t i = 0; i < len; ++i)
		ret += snprintf(sbuf + ret, size - ret, "%02x", data[i]);
	return ret;
}

static ssize_t _mr_write(struct file *filp, struct kobject *kobj,
			 struct bin_attribute *attr, char *page, loff_t off,
			 size_t count)
{
	const struct tsm_measurement_register *mr;
	struct _mr_provider *pvd;
	ssize_t rc;

	if (off != 0 || count != attr->size)
		return -EINVAL;

	mr = (typeof(mr))attr->private;
	BUG_ON(mr->mr_size != attr->size);

	pvd = _mr_to_provider(mr, kobj);
	rc = down_write_killable(&pvd->rwsem);
	if (rc)
		return rc;

	if (mr->mr_flags & TSM_MR_F_X) {
		struct _rtmr *rtmr;
		rtmr = container_of(kobj, typeof(*rtmr), kobj);

		char ext_line[0x100] = "# .EXTEND ";
		size_t len = strnlen(ext_line, sizeof(ext_line));
		len += snprint_hex(ext_line + len, sizeof(ext_line) - len, page,
				   count);
		rc = _log_extend_line(rtmr, ext_line, ext_line + len,
				      rtmr->log_in_sync, NULL);
		if (!IS_ERR_VALUE(rc))
			rc = _call_extend(pvd, mr, page);
		if (!rc)
			rtmr->log_in_sync = false;
	} else {
		memcpy(mr->mr_value, page, count);
	}

	if (!rc)
		pvd->in_sync = false;
	else
		pr_warn(KBUILD_MODNAME ": extending %s/%s failed with %ld\n",
			kobject_name(&pvd->kset.kobj), mr->mr_name, rc);

	up_write(&pvd->rwsem);
	return rc ?: count;
}

static ssize_t _log_read(struct file *filp, struct kobject *kobj,
			 struct bin_attribute *attr, char *page, loff_t off,
			 size_t count)
{
	struct _mr_provider *pvd;
	int rc;

	if (unlikely(off < 0))
		return -EINVAL;

	if (unlikely(off > attr->size))
		return 0;

	count = min(count, attr->size - off);
	if (likely(count > 0)) {
		pvd = container_of(kobj->kset, typeof(*pvd), kset);
		rc = down_read_interruptible(&pvd->rwsem);
		if (rc)
			return rc;

		memcpy(page, (char *)attr->private + off, count);

		up_read(&pvd->rwsem);
	}

	return count;
}

static ssize_t _log_extend(struct _rtmr *rtmr, const char *page, size_t count,
			   struct crypto_shash *tfm)
{
	ssize_t rc = 0, sz = 0;
	for (size_t i = 0; i < count && !IS_ERR_VALUE(rc);) {
		size_t j;
		for (j = i; j < count && (page[j] != '\n' && page[j] != '\r');)
			++j;

		rc = _log_extend_line(rtmr, &page[i], &page[j], sz == 0, tfm);
		sz += rc;

		for (i = j; i < count && (page[i] == '\n' || page[i] == '\r');)
			++i;
	}

	return IS_ERR_VALUE(rc) ? rc : sz;
}

DEFINE_FREE(shash, struct crypto_shash *,
	    if (!IS_ERR(_T)) crypto_free_shash(_T));

static ssize_t append_event_store(struct kobject *kobj,
				  struct kobj_attribute *attr, const char *page,
				  size_t count)
{
	struct _rtmr *rtmr;
	rtmr = container_of(kobj, typeof(*rtmr), kobj);

	const struct tsm_measurement_register *mr;
	mr = _rtmr_mr(rtmr);

	struct crypto_shash *tfm __free(shash) =
		crypto_alloc_shash(hash_algo_name[mr->mr_hash], 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);

	struct _mr_provider *pvd;
	pvd = container_of(kobj->kset, typeof(*pvd), kset);

	ssize_t rc = down_write_killable(&pvd->rwsem);
	if (rc)
		return rc;

	if (!rtmr->log_in_sync) {
		if (mr->mr_flags & TSM_MR_F_L)
			rc = _call_refresh(pvd, mr);

		if (!IS_ERR_VALUE(rc)) {
			char sync[0x100] = "SYNC ";
			strncat(sync, hash_algo_name[mr->mr_hash],
				sizeof(sync));
			size_t len = strnlen(sync, sizeof(sync));
			sync[len++] = '/';
			len += snprint_hex(sync + len, sizeof(sync) - len,
					   mr->mr_value, mr->mr_size);
			rc = _log_extend_line(rtmr, sync, sync + len,
					      _rtmr_log_size(rtmr) > 0, tfm);
		}
	}

	if (!IS_ERR_VALUE(rc)) {
		rtmr->log_in_sync = true;
		rc = _log_extend(rtmr, page, count, tfm);
	}

	up_write(&pvd->rwsem);
	return IS_ERR_VALUE(rc) ? rc : count;
}

static void _rtmr_release(struct kobject *kobj)
{
	struct _rtmr *rtmr;
	rtmr = container_of(kobj, typeof(*rtmr), kobj);
	pr_debug("%s(%s)\n", __func__, kobject_name(kobj));
	kfree(_rtmr_log(rtmr));
	kfree(rtmr);
}

static struct kobj_type _rtmr_ktype = {
	.release = _rtmr_release,
	.sysfs_ops = &kobj_sysfs_ops,
};

static struct _rtmr *_rtmr_create(const struct tsm_measurement_register *mr,
				  struct _mr_provider *pvd)
{
	struct _rtmr *rtmr __free(kfree);
	int rc;

	BUG_ON(!(mr->mr_flags & TSM_MR_F_X));
	rtmr = kzalloc(sizeof(*rtmr), GFP_KERNEL);
	if (!rtmr)
		return ERR_PTR(-ENOMEM);

	sysfs_bin_attr_init(&rtmr->battrs[_RTMR_BATTR_DIGEST]);
	rtmr->battrs[_RTMR_BATTR_DIGEST].attr.name = "digest";
	if (mr->mr_flags & TSM_MR_F_W)
		rtmr->battrs[_RTMR_BATTR_DIGEST].attr.mode |= S_IWUSR;
	if (mr->mr_flags & TSM_MR_F_R)
		rtmr->battrs[_RTMR_BATTR_DIGEST].attr.mode |= S_IRUGO;

	rtmr->battrs[_RTMR_BATTR_DIGEST].size = mr->mr_size;
	rtmr->battrs[_RTMR_BATTR_DIGEST].read = _mr_read;
	rtmr->battrs[_RTMR_BATTR_DIGEST].write = _mr_write;
	rtmr->battrs[_RTMR_BATTR_DIGEST].private = (void *)mr;

	sysfs_bin_attr_init(&rtmr->battrs[_RTMR_BATTR_LOG]);
	rtmr->battrs[_RTMR_BATTR_LOG].attr.name = "event_log";
	rtmr->battrs[_RTMR_BATTR_LOG].attr.mode = S_IRUGO;
	rtmr->battrs[_RTMR_BATTR_LOG].read = _log_read;

	rtmr->kobj.kset = &pvd->kset;
	rc = kobject_init_and_add(&rtmr->kobj, &_rtmr_ktype, NULL, "%s",
				  mr->mr_name);
	if (rc)
		return ERR_PTR(rc);

	return_ptr(rtmr);
}

static void _mr_provider_release(struct kobject *kobj)
{
	struct _mr_provider *pvd;
	pvd = container_of(kobj, typeof(*pvd), kset.kobj);
	pr_debug("%s(%s)\n", __func__, kobject_name(kobj));
	BUG_ON(!list_empty(&pvd->kset.list));
	kfree(pvd->static_mrs);
	kfree(pvd);
}

static struct kobj_type _mr_provider_ktype = {
	.release = _mr_provider_release,
	.sysfs_ops = &kobj_sysfs_ops,
};

static struct config_group *tsm_report_group;
static struct kset *_sysfs_tsm;

static struct _mr_provider *
_mr_provider_create(struct tsm_measurement_provider *tpvd)
{
	struct _mr_provider *pvd __free(kfree);
	int rc;

	pvd = kzalloc(sizeof(*pvd), GFP_KERNEL);
	if (!pvd)
		return ERR_PTR(-ENOMEM);

	if (!tpvd->name || !tpvd->mrs || !tpvd->refresh || !tpvd->extend)
		return ERR_PTR(-EINVAL);

	rc = kobject_set_name(&pvd->kset.kobj, "%s", tpvd->name);
	if (rc)
		return ERR_PTR(rc);

	pvd->kset.kobj.kset = _sysfs_tsm;
	pvd->kset.kobj.ktype = &_mr_provider_ktype;
	pvd->provider = tpvd;

	rc = kset_register(&pvd->kset);
	if (rc)
		return ERR_PTR(rc);

	return_ptr(pvd);
}

DEFINE_FREE(_unregister_measurement_provider, struct _mr_provider *,
	    if (!IS_ERR_OR_NULL(_T))
		    tsm_unregister_measurement_provider(_T->provider));

int tsm_register_measurement_provider(struct tsm_measurement_provider *tpvd)
{
	static struct kobj_attribute _attr_hash = __ATTR_RO(hash_algo);
	static struct kobj_attribute _attr_append = __ATTR_WO(append_event);

	struct _mr_provider *pvd __free(_unregister_measurement_provider);
	int rc, nr;

	pvd = _mr_provider_create(tpvd);
	if (IS_ERR(pvd))
		return PTR_ERR(pvd);

	nr = 0;
	for (int i = 0; tpvd->mrs[i].mr_name; ++i) {
		if (!(tpvd->mrs[i].mr_flags & TSM_MR_F_X)) {
			++nr;
			continue;
		}

		struct _rtmr *rtmr = _rtmr_create(&tpvd->mrs[i], pvd);
		if (IS_ERR(rtmr))
			return PTR_ERR(rtmr);

		struct attribute *attrs[] = {
			&_attr_append.attr,
			&_attr_hash.attr,
			NULL,
		};
		struct bin_attribute *battrs[_RTMR_BATTR__COUNT + 1] = {};
		for (int j = 0; j < _RTMR_BATTR__COUNT; ++j)
			battrs[j] = &rtmr->battrs[j];
		struct attribute_group agrp = {
			.attrs = attrs,
			.bin_attrs = battrs,
		};
		rc = sysfs_create_group(&rtmr->kobj, &agrp);
		if (rc)
			return rc;
	}

	if (nr > 0) {
		struct bin_attribute *static_mrs __free(kfree);
		struct bin_attribute **battrs __free(kfree);

		static_mrs = kcalloc(sizeof(*static_mrs), nr, GFP_KERNEL);
		battrs = kcalloc(sizeof(*battrs), nr + 1, GFP_KERNEL);
		if (!battrs || !static_mrs)
			return -ENOMEM;

		for (int i = 0, j = 0; tpvd->mrs[i].mr_name; ++i) {
			if (tpvd->mrs[i].mr_flags & TSM_MR_F_X)
				continue;

			static_mrs[j].attr.name = tpvd->mrs[i].mr_name;
			if (tpvd->mrs[i].mr_flags & TSM_MR_F_R) {
				static_mrs[j].attr.mode |= S_IRUGO;
				static_mrs[j].read = _mr_read;
			}
			if (tpvd->mrs[i].mr_flags & TSM_MR_F_W) {
				static_mrs[j].attr.mode |= S_IWUSR;
				static_mrs[j].write = _mr_write;
			}
			static_mrs[j].size = tpvd->mrs[i].mr_size;
			static_mrs[j].private = (void *)&tpvd->mrs[i];

			battrs[j] = &static_mrs[j];
			++j;

			BUG_ON(j > nr);
		}

		struct attribute_group agrp = {
			.bin_attrs = battrs,
		};
		rc = sysfs_create_group(&pvd->kset.kobj, &agrp);
		if (rc)
			return rc;

		pvd->static_mrs = no_free_ptr(static_mrs);
	}

	pvd = NULL;
	return 0;
}
EXPORT_SYMBOL_GPL(tsm_register_measurement_provider);

static void _kset_put_children(struct kset *kset)
{
	struct kobject *p, *n;
	spin_lock(&kset->list_lock);
	list_for_each_entry_safe(p, n, &kset->list, entry) {
		spin_unlock(&kset->list_lock);
		kobject_put(p);
		spin_lock(&kset->list_lock);
	}
	spin_unlock(&kset->list_lock);
}

int tsm_unregister_measurement_provider(struct tsm_measurement_provider *tpvd)
{
	struct kobject *kobj = kset_find_obj(_sysfs_tsm, tpvd->name);
	if (!kobj)
		return -ENOENT;

	struct _mr_provider *pvd = container_of(kobj, typeof(*pvd), kset.kobj);
	BUG_ON(pvd->provider != tpvd);

	_kset_put_children(&pvd->kset);
	kset_unregister(&pvd->kset);
	kobject_put(kobj);
	return 0;
}
EXPORT_SYMBOL_GPL(tsm_unregister_measurement_provider);

static int __init tsm_init(void)
{
	struct config_group *root = &tsm_configfs.su_group;
	struct config_group *tsm;
	int rc;

	config_group_init(root);
	rc = configfs_register_subsystem(&tsm_configfs);
	if (rc)
		return rc;

	tsm = configfs_register_default_group(root, "report",
					      &tsm_reports_type);
	if (IS_ERR(tsm)) {
		configfs_unregister_subsystem(&tsm_configfs);
		return PTR_ERR(tsm);
	}
	tsm_report_group = tsm;

	_sysfs_tsm = kset_create_and_add("tsm", NULL, kernel_kobj);
	if (!_sysfs_tsm)
		return -ENOMEM;

	return 0;
}
module_init(tsm_init);

static void __exit tsm_exit(void)
{
	kset_unregister(_sysfs_tsm);
	configfs_unregister_default_group(tsm_report_group);
	configfs_unregister_subsystem(&tsm_configfs);
}
module_exit(tsm_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(
	"Provide Trusted Security Module attestation reports via configfs");
