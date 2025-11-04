/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2021 Intel Corporation. All rights rsvd. */

#ifndef __IAA_CRYPTO_H__
#define __IAA_CRYPTO_H__

#include <linux/crypto.h>
#include <linux/idxd.h>
#include <uapi/linux/idxd.h>

#define IDXD_SUBDRIVER_NAME		"crypto"

#define IAA_DECOMP_ENABLE		BIT(0)
#define IAA_DECOMP_FLUSH_OUTPUT		BIT(1)
#define IAA_DECOMP_CHECK_FOR_EOB	BIT(2)
#define IAA_DECOMP_STOP_ON_EOB		BIT(3)
#define IAA_DECOMP_SUPPRESS_OUTPUT	BIT(9)

#define IAA_COMP_FLUSH_OUTPUT		BIT(1)
#define IAA_COMP_APPEND_EOB		BIT(2)

#define IAA_COMPLETION_TIMEOUT		1000000

#define IAA_ALLOC_DESC_COMP_TIMEOUT	   1000
#define IAA_ALLOC_DESC_DECOMP_TIMEOUT	    500

#define IAA_ANALYTICS_ERROR		0x0a
#define IAA_ERROR_DECOMP_BUF_OVERFLOW	0x0b
#define IAA_ERROR_COMP_BUF_OVERFLOW	0x19
#define IAA_ERROR_WATCHDOG_EXPIRED	0x24

#define IAA_COMP_MODES_MAX		2

#define FIXED_HDR			0x2
#define FIXED_HDR_SIZE			3

#define IAA_COMP_FLAGS			(IAA_COMP_FLUSH_OUTPUT | \
					 IAA_COMP_APPEND_EOB)

#define IAA_DECOMP_FLAGS		(IAA_DECOMP_ENABLE |	   \
					 IAA_DECOMP_FLUSH_OUTPUT | \
					 IAA_DECOMP_CHECK_FOR_EOB | \
					 IAA_DECOMP_STOP_ON_EOB)

/* Representation of IAA workqueue */
struct iaa_wq {
	struct list_head	list;

	struct idxd_wq		*wq;
	struct percpu_ref	ref;
	bool			free;
	bool			mapped;

	struct iaa_device	*iaa_device;

	atomic64_t		comp_calls;
	atomic64_t		comp_bytes;
	atomic64_t		decomp_calls;
	atomic64_t		decomp_bytes;
};

struct iaa_device_compression_mode {
	const char			*name;

	struct aecs_comp_table_record	*aecs_comp_table;

	dma_addr_t			aecs_comp_table_dma_addr;
};

struct wq_table_entry {
	struct idxd_wq	**wqs;
	unsigned int	max_wqs;
	unsigned int	n_wqs;
	unsigned int	cur_wq;
};

/* Representation of IAA device with wqs, populated by probe */
struct iaa_device {
	struct list_head		list;
	struct idxd_device		*idxd;

	struct iaa_device_compression_mode	*compression_modes[IAA_COMP_MODES_MAX];

	int				n_wq;
	struct list_head		wqs;

	struct wq_table_entry		*generic_wq_table;
	struct wq_table_entry		*comp_wq_table;

	atomic64_t			comp_calls;
	atomic64_t			comp_bytes;
	atomic64_t			decomp_calls;
	atomic64_t			decomp_bytes;
};

#define IAA_AECS_ALIGN			32

/*
 * Analytics Engine Configuration and State (AECS) contains parameters and
 * internal state of the analytics engine.
 */
struct aecs_comp_table_record {
	u32 crc;
	u32 xor_checksum;
	u32 reserved0[5];
	u32 num_output_accum_bits;
	u8 output_accum[256];
	u32 ll_sym[286];
	u32 reserved1;
	u32 reserved2;
	u32 d_sym[30];
	u32 reserved_padding[2];
} __packed;

int iaa_aecs_init_fixed(void);
void iaa_aecs_cleanup_fixed(void);

typedef int (*iaa_dev_comp_init_fn_t) (struct iaa_device_compression_mode *mode);
typedef int (*iaa_dev_comp_free_fn_t) (struct iaa_device_compression_mode *mode);

struct iaa_compression_mode {
	const char		*name;
	u32			*ll_table;
	int			ll_table_size;
	u32			*d_table;
	int			d_table_size;
	iaa_dev_comp_init_fn_t	init;
	iaa_dev_comp_free_fn_t	free;
};

int add_iaa_compression_mode(const char *name,
			     const u32 *ll_table,
			     int ll_table_size,
			     const u32 *d_table,
			     int d_table_size,
			     iaa_dev_comp_init_fn_t init,
			     iaa_dev_comp_free_fn_t free);

void remove_iaa_compression_mode(const char *name);

enum iaa_mode {
	IAA_MODE_FIXED,
};

struct iaa_compression_ctx {
	enum iaa_mode	mode;
	u16		alloc_comp_desc_timeout;
	u16		alloc_decomp_desc_timeout;
	bool		verify_compress;
	bool		async_mode;
	bool		use_irq;
};

extern struct list_head iaa_devices;
extern struct mutex iaa_devices_lock;

#endif
