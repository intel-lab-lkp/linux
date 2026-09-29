// SPDX-License-Identifier: GPL-2.0

#include <linux/debugfs.h>

#include "nfsd.h"

static struct dentry *nfsd_top_dir __read_mostly;

/*
 * /sys/kernel/debug/nfsd/disable-splice-read
 *
 * Contents:
 *   %0: NFS READ is allowed to use page splicing
 *   %1: NFS READ uses only iov iter read
 *
 * The default value of this setting is zero (page splicing is
 * allowed). This setting takes immediate effect for all NFS
 * versions, all exports, and in all NFSD net namespaces.
 */

static int nfsd_dsr_get(void *data, u64 *val)
{
	*val = nfsd_disable_splice_read ? 1 : 0;
	return 0;
}

static int nfsd_dsr_set(void *data, u64 val)
{
	nfsd_disable_splice_read = (val > 0);
	if (!nfsd_disable_splice_read) {
		/*
		 * Must use buffered I/O if splice_read is enabled.
		 */
		nfsd_io_cache_read = NFSD_IO_BUFFERED;
	}
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(nfsd_dsr_fops, nfsd_dsr_get, nfsd_dsr_set, "%llu\n");

/*
 * /sys/kernel/debug/nfsd/io_cache_read
 *
 * Contents:
 *   %0: NFS READ will use buffered IO
 *   %1: NFS READ will use dontcache (buffered IO w/ dropbehind)
 *   %2: NFS READ will use direct IO
 *
 * This setting takes immediate effect for all NFS versions,
 * all exports, and in all NFSD net namespaces.
 */

static int nfsd_io_cache_read_get(void *data, u64 *val)
{
	*val = nfsd_io_cache_read;
	return 0;
}

static int nfsd_io_cache_read_set(void *data, u64 val)
{
	int ret = 0;

	switch (val) {
	case NFSD_IO_BUFFERED:
		nfsd_io_cache_read = NFSD_IO_BUFFERED;
		break;
	case NFSD_IO_DONTCACHE:
	case NFSD_IO_DIRECT:
		/*
		 * Must disable splice_read when enabling
		 * NFSD_IO_DONTCACHE.
		 */
		nfsd_disable_splice_read = true;
		nfsd_io_cache_read = val;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

DEFINE_DEBUGFS_ATTRIBUTE(nfsd_io_cache_read_fops, nfsd_io_cache_read_get,
			 nfsd_io_cache_read_set, "%llu\n");

/*
 * /sys/kernel/debug/nfsd/io_cache_write
 *
 * Contents:
 *   %0: NFS WRITE will use buffered IO
 *   %1: NFS WRITE will use dontcache (buffered IO w/ dropbehind)
 *   %2: NFS WRITE will use direct IO with stable_how=NFS_UNSTABLE
 *   %3: NFS WRITE will use direct IO with stable_how=NFS_DATA_SYNC
 *   %4: NFS WRITE will use direct IO with stable_how=NFS_FILE_SYNC
 *
 * This setting takes immediate effect for all NFS versions,
 * all exports, and in all NFSD net namespaces.
 */

static int nfsd_io_cache_write_get(void *data, u64 *val)
{
	*val = nfsd_io_cache_write;
	return 0;
}

static int nfsd_io_cache_write_set(void *data, u64 val)
{
	int ret = 0;

	switch (val) {
	case NFSD_IO_BUFFERED:
	case NFSD_IO_DONTCACHE:
	case NFSD_IO_DIRECT:
	case NFSD_IO_DIRECT_WRITE_DATA_SYNC:
	case NFSD_IO_DIRECT_WRITE_FILE_SYNC:
		nfsd_io_cache_write = val;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

DEFINE_DEBUGFS_ATTRIBUTE(nfsd_io_cache_write_fops, nfsd_io_cache_write_get,
			 nfsd_io_cache_write_set, "%llu\n");

void nfsd_debugfs_exit(void)
{
	debugfs_remove_recursive(nfsd_top_dir);
	nfsd_top_dir = NULL;
}

/*
 * /sys/kernel/debug/nfsd/direct_misaligned_num_pages
 *
 * The smallest DIO-aligned middle segment, in pages, that is worth
 * splitting a misaligned direct-mode WRITE into three segments for.  A
 * WRITE whose middle is smaller than this, and which has a misaligned
 * start or end, is issued as a single buffered segment instead.
 *
 * Default 2.  Not yet tuned by benchmarking.
 */

/*
 * /sys/kernel/debug/nfsd/direct_misaligned_dontcache
 *
 * How a direct-mode WRITE issues the I/O that cannot be direct: the
 * misaligned start and end of a split WRITE, and the whole WRITE when it
 * is not split.
 *
 * Contents:
 *   Y: DONTCACHE when the filesystem supports it, with the boundary page
 *      of a split kept in the page cache only until both WRITEs sharing
 *      it have written it
 *   N: ordinary cached buffered IO, left in the page cache until
 *      reclaim, for A/B comparison against the DONTCACHE path
 *
 * Sampled once per WRITE, so it takes effect immediately.  The direct
 * middle segment is unaffected.
 */

void nfsd_debugfs_init(void)
{
	nfsd_top_dir = debugfs_create_dir("nfsd", NULL);

	debugfs_create_file("disable-splice-read", S_IWUSR | S_IRUGO,
			    nfsd_top_dir, NULL, &nfsd_dsr_fops);

	debugfs_create_file("io_cache_read", 0644, nfsd_top_dir, NULL,
			    &nfsd_io_cache_read_fops);

	debugfs_create_file("io_cache_write", 0644, nfsd_top_dir, NULL,
			    &nfsd_io_cache_write_fops);

	debugfs_create_u32("direct_misaligned_num_pages", 0644, nfsd_top_dir,
			   &nfsd_direct_misaligned_num_pages);

	debugfs_create_bool("direct_misaligned_dontcache", 0644, nfsd_top_dir,
			    &nfsd_direct_misaligned_dontcache);
#ifdef CONFIG_NFSD_V4
	debugfs_create_bool("delegated_timestamps", 0644, nfsd_top_dir,
			    &nfsd_delegts_enabled);
#endif
}
