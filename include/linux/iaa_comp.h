/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2021 Intel Corporation. All rights rsvd. */

#ifndef __IAA_COMP_H__
#define __IAA_COMP_H__

#if IS_ENABLED(CONFIG_CRYPTO_DEV_IAA_CRYPTO)

#include <linux/scatterlist.h>

#define IAA_COMP_MODES_MAX  IAA_MODE_NONE

enum iaa_mode {
	IAA_MODE_FIXED = 0,
	IAA_MODE_DYNAMIC = 1,
	IAA_MODE_NONE = 2,
};

struct iaa_req {
	struct scatterlist *src;
	struct scatterlist *dst;
	struct scatterlist sg_src;
	unsigned int slen;
	unsigned int dlen;
	u32 flags;
	u32 compression_crc;
	void *drv_data; /* for driver internal use */
	int **dlens;
};

extern bool iaa_comp_enabled(void);

extern enum iaa_mode iaa_comp_get_compressor_mode(const char *compressor_name);

extern bool iaa_comp_mode_is_registered(enum iaa_mode mode);

extern u8 iaa_comp_get_modes(char **iaa_mode_names, enum iaa_mode *iaa_modes);

extern void iaa_comp_put_modes(char **iaa_mode_names, enum iaa_mode *iaa_modes, u8 nr_modes);

extern unsigned int iaa_comp_get_max_batch_size(void);

extern int iaa_comp_compress(enum iaa_mode mode, struct iaa_req *req);

extern int iaa_comp_decompress(enum iaa_mode mode, struct iaa_req *req);

extern int iaa_comp_compress_batch(
	enum iaa_mode mode,
	struct iaa_req *parent_req,
	unsigned int unit_size);

extern int iaa_comp_decompress_batch(
	enum iaa_mode mode,
	struct iaa_req *parent_req,
	unsigned int unit_size);

#else /* CONFIG_CRYPTO_DEV_IAA_CRYPTO */

enum iaa_mode {
	IAA_MODE_NONE = 2,
};

struct iaa_req {};

static inline bool iaa_comp_enabled(void)
{
	return false;
}

static inline enum iaa_mode iaa_comp_get_compressor_mode(const char *compressor_name)
{
	return IAA_MODE_NONE;
}

static inline bool iaa_comp_mode_is_registered(enum iaa_mode mode)
{
	return false;
}

static inline u8 iaa_comp_get_modes(char **iaa_mode_names, enum iaa_mode *iaa_modes)
{
	return 0;
}

static inline void iaa_comp_put_modes(char **iaa_mode_names, enum iaa_mode *iaa_modes, u8 nr_modes)
{
}

static inline unsigned int iaa_comp_get_max_batch_size(void)
{
	return 0;
}

static inline int iaa_comp_compress(enum iaa_mode mode, struct iaa_req *req)
{
	return -EINVAL;
}

static inline int iaa_comp_decompress(enum iaa_mode mode, struct iaa_req *req)
{
	return -EINVAL;
}

static inline int iaa_comp_compress_batch(
	enum iaa_mode mode,
	struct iaa_req *parent_req,
	unsigned int unit_size)
{
	return -EINVAL;
}

static inline int iaa_comp_decompress_batch(
	enum iaa_mode mode,
	struct iaa_req *parent_req,
	unsigned int unit_size)
{
	return -EINVAL;
}

#endif /* CONFIG_CRYPTO_DEV_IAA_CRYPTO */

#endif
