/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __TSM_H
#define __TSM_H

#include <linux/sizes.h>
#include <linux/types.h>
#include <linux/device.h>

#define TSM_INBLOB_MAX 64
#define TSM_OUTBLOB_MAX SZ_32K

/*
 * Privilege level is a nested permission concept to allow confidential
 * guests to partition address space, 4-levels are supported.
 */
#define TSM_PRIVLEVEL_MAX 3

/**
 * struct tsm_desc - option descriptor for generating tsm report blobs
 * @privlevel: optional privilege level to associate with @outblob
 * @inblob_len: sizeof @inblob
 * @inblob: arbitrary input data
 */
struct tsm_desc {
	unsigned int privlevel;
	size_t inblob_len;
	u8 inblob[TSM_INBLOB_MAX];
};

/**
 * struct tsm_report - track state of report generation relative to options
 * @desc: report generation options / cached report state
 * @outblob: generated evidence to provider to the attestation agent
 * @outblob_len: sizeof(outblob)
 * @write_generation: conflict detection, and report regeneration tracking
 * @read_generation: cached report invalidation tracking
 * @cfg: configfs interface
 */
struct tsm_report {
	struct tsm_desc desc;
	size_t outblob_len;
	u8 *outblob;
	size_t certs_len;
	u8 *certs;
};

/*
 * arch specific ops, only one is expected to be registered at a time
 * i.e. only one of SEV, TDX, COVE, etc.
 */
struct tsm_ops {
	const char *name;
	const int privlevel_floor;
	int (*report_new)(struct tsm_report *desc, void *data);
};

extern const struct config_item_type tsm_report_ext_type;
extern const struct config_item_type tsm_report_default_type;

int tsm_register(const struct tsm_ops *ops, void *priv,
		 const struct config_item_type *type);
int tsm_unregister(const struct tsm_ops *ops);
#endif /* __TSM_H */
