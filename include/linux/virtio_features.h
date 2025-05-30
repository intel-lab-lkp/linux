/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VIRTIO_FEATURES_H
#define _LINUX_VIRTIO_FEATURES_H

#include <linux/bits.h>

#if IS_ENABLED(CONFIG_ARCH_SUPPORTS_INT128)
#define VIRTIO_HAS_EXTENDED_FEATURES
#define VIRTIO_FEATURES_MAX	128
#define VIRTIO_FEATURES_WORDS	4
#define VIRTIO_BIT(b)		_BIT128(b)

typedef __uint128_t virtio_features_t;

#else
#define VIRTIO_FEATURES_MAX	64
#define VIRTIO_FEATURES_WORDS	2

static inline u64 VIRTIO_BIT(int bit)
{
	return bit >= 64 ? 0 : BIT_ULL(b);
}

typedef u64 virtio_features_t;
#endif

#endif
