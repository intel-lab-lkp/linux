// SPDX-License-Identifier: GPL-2.0
/*
 * System Control and Management Interface (SCMI) Message Protocol Quirks
 *
 * Copyright (C) 2025 ARM Ltd.
 */

#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/export.h>
#include <linux/hashtable.h>
#include <linux/kstrtox.h>
#include <linux/static_key.h>
#include <linux/string.h>
#include <linux/types.h>

#include "quirks.h"

#define SCMI_QUIRKS_HT_SZ	4

struct scmi_quirk {
	bool enabled;
	const char *name;
	char *compatible;
	char *vendor;
	char *sub_vendor_id;
	char *impl_ver_range;
	u32 start_range;
	u32 end_range;
	struct static_key_false *key;
	struct hlist_node hash;
	unsigned int hkey;
};

#define __DEFINE_SCMI_QUIRK_ENTRY(_qn, _comp, _ven, _sub, _impl)	\
	static struct scmi_quirk scmi_quirk_entry_ ## _qn = {		\
		.name = __stringify(quirk_ ## _qn),			\
		.compatible = _comp,					\
		.vendor = _ven,						\
		.sub_vendor_id = _sub,					\
		.impl_ver_range = _impl,				\
		.key = &(scmi_quirk_ ## _qn),				\
	}

#define __DECLARE_SCMI_QUIRK_ENTRY(_qn)		(&(scmi_quirk_entry_ ## _qn))

/*
 * Define a quirk by name (_qn) and provide the matching tokens where:
 *
 *  _comp : compatible string, NULL means any.
 *  _ven : SCMI Vendor ID string, NULL means any.
 *  _sub : SCMI SubVendor ID string, NULL means any.
 *  _impl : SCMI Implementation Version string, NULL means any.
 *          This version string can express ranges using the following
 *          syntax:
 *
 *			NULL		[0, 0xFFFFFFFF]
 *			"X"		[X, X]
 *			"X-"		[X, 0xFFFFFFFF]
 *			"-X"		[0, X]
 *			"X-Y"		[X, Y]
 *
 *          where <v> in [MIN, MAX] means:
 *
 *		MIN <= <v> <= MAX  && MIN <= MAX
 *
 *  This implicitly define also a properly named global static-key that
 *  will be used to dynamically enable the quirk at initialization time.
 *
 *  Note that it is possible to associate multiple quirks to the same
 *  matching pattern, if your firmware quality is really astounding :P
 */
#define DEFINE_SCMI_QUIRK(_qn, _comp, _ven, _sub, _impl)		\
	DEFINE_STATIC_KEY_FALSE(scmi_quirk_ ## _qn);			\
	__DEFINE_SCMI_QUIRK_ENTRY(_qn, _comp, _ven, _sub, _impl)

/*
 * Same as DEFINE_SCMI_QUIRK but EXPORTED: this is meant to address quirks
 * that possibly reside in code that is included in loadable kernel modules
 * that needs to be able to access the global static keys at runtime to
 * determine if enabled or not. (see SCMI_QUIRK to understand usage)
 */
#define DEFINE_SCMI_QUIRK_EXPORTED(_qn, _comp, _ven, _sub, _impl)	\
	DEFINE_STATIC_KEY_FALSE(scmi_quirk_ ## _qn);			\
	EXPORT_SYMBOL_GPL(scmi_quirk_ ## _qn);				\
	__DEFINE_SCMI_QUIRK_ENTRY(_qn, _comp, _ven, _sub, _impl)

/* Global Quirks Definitions */

/*
 * Quirks Pointers Array
 *
 * This is filled at compile-time with the list of pointers to all the currently
 * defined quirks descriptors.
 */
static struct scmi_quirk *scmi_quirks_table[] = {
	NULL
};

/*
 * Quirks HashTable
 *
 * A run-time populated hashtable containing all the defined quirks descriptors
 * hashed by matching pattern.
 */
static DEFINE_READ_MOSTLY_HASHTABLE(scmi_quirks_ht, SCMI_QUIRKS_HT_SZ);

static unsigned int
scmi_quirk_signature(const char *compat, const char *vend, const char *sub_vend)
{
	char *signature, *p;
	unsigned int hash32;
	unsigned long hash = 0;

	/* vendor_id/sub_vendor_id guaranteed <= SCMI_SHORT_NAME_MAX_SIZE */
	signature = kasprintf(GFP_KERNEL, "|%s|%s|%s|",
			      compat ?: "", vend ?: "", sub_vend ?: "");
	if (!signature)
		return 0;

	pr_debug("SCMI Quirk Signature >>>%s<<<\n", signature);

	p = signature;
	while (*p)
		hash = partial_name_hash(tolower(*p++), hash);
	hash32 = end_name_hash(hash);

	kfree(signature);

	return hash32;
}

static int scmi_quirk_range_parse(struct scmi_quirk *quirk)
{
	const char *last, *first = quirk->impl_ver_range;
	size_t len;
	char *sep;
	int ret;

	quirk->start_range = 0;
	quirk->end_range = 0xFFFFFFFF;
	len = quirk->impl_ver_range ? strlen(quirk->impl_ver_range) : 0;
	if (!len)
		return 0;

	last = first + len - 1;
	sep = strchr(quirk->impl_ver_range, '-');
	if (sep)
		*sep = '\0';

	if (sep == first) // -X
		ret = kstrtouint(first + 1, 0, &quirk->end_range);
	else // X OR X- OR X-y
		ret = kstrtouint(first, 0, &quirk->start_range);
	if (ret)
		return ret;

	if (!sep)
		quirk->end_range = quirk->start_range;
	else if (sep != last) //x-Y
		ret = kstrtouint(sep + 1, 0, &quirk->end_range);

	if (quirk->start_range > quirk->end_range)
		return -EINVAL;

	return ret;
}

void scmi_quirks_initialize(void)
{
	struct scmi_quirk *quirk;
	int i;

	for (i = 0, quirk = scmi_quirks_table[0]; quirk;
	     i++, quirk = scmi_quirks_table[i]) {
		int ret;

		ret = scmi_quirk_range_parse(quirk);
		if (ret) {
			pr_err("SCMI skip QUIRK [%s] - BAD RANGE\n",
			       quirk->name);
			continue;
		}
		quirk->hkey = scmi_quirk_signature(quirk->compatible,
						   quirk->vendor,
						   quirk->sub_vendor_id);

		hash_add(scmi_quirks_ht, &quirk->hash, quirk->hkey);

		pr_debug("Registered SCMI QUIRK [%s] -- %p - Key [0x%08X] - %s/%s/%s/[0x%08X-0x%08X]\n",
			 quirk->name, quirk, quirk->hkey, quirk->compatible,
			 quirk->vendor, quirk->sub_vendor_id,
			 quirk->start_range, quirk->end_range);
	}

	pr_debug("SCMI Quirks initialized\n");
}

void scmi_quirks_enable(struct device *dev, const char *compat,
			const char *vend, const char *subv, const u32 impl)
{
	dev_dbg(dev, "Looking for quirks matching: %s/%s/%s/0x%08X\n",
		compat, vend, subv, impl);

	/* Lookup into scmi_quirks_ht using 2 loops: with/without compatible */
	for (int k = 1; k >= 0 ; k--) {
		const char *compat_sel = k > 0 ? compat : NULL;

		for (int i = 3; i > 0; i--) {
			struct scmi_quirk *quirk;
			unsigned int hkey;

			hkey = scmi_quirk_signature(compat_sel,
						    i > 1 ? vend : NULL,
						    i > 2 ? subv : NULL);

			/*
			 * Note that there could be multiple matches so we
			 * will enable multiple quirk part of an hash collision
			 * domain...BUT we cannot assume that ALL quirks on the
			 * same collision domain are a full match.
			 */
			hash_for_each_possible(scmi_quirks_ht, quirk, hash, hkey) {
				if (quirk->enabled || quirk->hkey != hkey ||
				    impl < quirk->start_range ||
				    impl > quirk->end_range)
					continue;

				dev_info(dev, "Enabling SCMI Quirk [%s]\n",
					 quirk->name);
				dev_dbg(dev,
					"Quirk matched on: %s/%s/%s/[0x%08X-0x%08X]\n",
					quirk->compatible, quirk->vendor,
					quirk->sub_vendor_id,
					quirk->start_range, quirk->end_range);

				static_branch_enable(quirk->key);
				quirk->enabled = true;
			}
		}
	}
}
