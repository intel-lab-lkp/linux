/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __F2FS_NODE_CACHE_POLICY_H__
#define __F2FS_NODE_CACHE_POLICY_H__

#include <linux/types.h>

#define F2FS_NC_BUCKET_256_SIZE		256U
#define F2FS_NC_BUCKET_512_SIZE		512U
#define F2FS_NC_BUCKET_1024_SIZE	1024U
#define F2FS_NC_MAX_OBJECT_SIZE		F2FS_NC_BUCKET_1024_SIZE
#define F2FS_NC_PERCENT_MAX		100U

/*
 * Keep compressed queues in the same order as f2fs_nc_bucket_sizes[].
 * Raw entries remain on NODE_CACHE(sbi)->lru_list.
 */
enum f2fs_nc_queue {
	F2FS_NC_RAW,
	F2FS_NC_256,
	F2FS_NC_512,
	F2FS_NC_1024,
	F2FS_NC_NR_QUEUES,
};

u64 f2fs_nc_effective_count(const unsigned long nr[F2FS_NC_NR_QUEUES],
			    u32 blocksize);
void f2fs_nc_scan_quotas(const unsigned long nr[F2FS_NC_NR_QUEUES],
			 u32 blocksize, unsigned long requested,
		s64 credit[F2FS_NC_NR_QUEUES],
		unsigned long quota[F2FS_NC_NR_QUEUES]);

#endif /* __F2FS_NODE_CACHE_POLICY_H__ */
