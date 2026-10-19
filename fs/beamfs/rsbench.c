// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs -- Reed-Solomon micro-benchmark
 *
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * What the codec costs, with no block layer underneath it.
 *
 * Every performance figure the harness produces so far is a
 * filesystem figure: it contains the medium, the emulator and the
 * codec together, and cannot say which of the three it is measuring.
 * On the lab hardware that ambiguity is total -- beamfs writes at
 * 1.8 MB/s on a USB stick and 1.5 MB/s on a SSD capable of 700, so
 * the medium is plainly not the limit, but nothing said whether the
 * remainder was TCG emulation or Reed-Solomon.
 *
 * This encodes and decodes in memory, in a loop, and reports
 * nanoseconds per block. Subtracting it from the filesystem figure
 * leaves everything else. A reviewer asking "how much of the overhead
 * is the error correction" gets a number rather than an argument.
 *
 * Reading /sys/kernel/debug/beamfs/rs_bench runs the benchmark and
 * returns the result; the iteration count is settable alongside it.
 * Running on read is unusual for debugfs, and deliberate: a benchmark
 * that needs a write to arm and a read to collect has a window in
 * which two callers interleave, and this has no state to protect.
 */

#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/random.h>
#include "beamfs.h"

/* Blocks per run. Enough to swamp timer granularity under TCG, where
 * one block takes tens of microseconds; small enough that the read
 * returns while an operator is still watching.
 */
#define BEAMFS_RSBENCH_DEFAULT_ITERS 1000
#define BEAMFS_RSBENCH_MAX_ITERS     1000000

static struct dentry *beamfs_debugfs_root;
static u32 beamfs_rsbench_iters = BEAMFS_RSBENCH_DEFAULT_ITERS;

/*
 * One encode and one decode over a block's payload: 3824 bytes, as
 * BEAMFS_DATA_INLINE_SUBBLOCKS full-length RS(255,239) codewords.
 *
 * The decode runs on undamaged data. That is the case that matters:
 * every read pays it, whether or not anything is wrong, because the
 * syndromes have to be computed before the codec can say there is
 * nothing to correct. Correction itself is rarer and costs more, but
 * it is not what sets the steady-state read cost.
 */
static int beamfs_rsbench_run(u32 iters, u64 *enc_ns, u64 *dec_ns)
{
	u8 *data, *parity;
	int *results;
	u8 *positions;
	ktime_t t0;
	u32 i;
	int ret = 0;

	data = kvmalloc(BEAMFS_DATA_INLINE_BYTES, GFP_KERNEL);
	parity = kvmalloc((size_t)BEAMFS_DATA_INLINE_SUBBLOCKS * BEAMFS_RS_PARITY,
			  GFP_KERNEL);
	results = kcalloc(BEAMFS_DATA_INLINE_SUBBLOCKS, sizeof(*results),
			  GFP_KERNEL);
	positions = kcalloc((size_t)BEAMFS_DATA_INLINE_SUBBLOCKS *
			    (BEAMFS_RS_PARITY / 2), sizeof(*positions),
			    GFP_KERNEL);
	if (!data || !parity || !results || !positions) {
		ret = -ENOMEM;
		goto out;
	}

	get_random_bytes(data, BEAMFS_DATA_INLINE_BYTES);

	t0 = ktime_get();
	for (i = 0; i < iters; i++) {
		ret = beamfs_rs_encode_region(data, BEAMFS_SUBBLOCK_DATA,
					      parity, BEAMFS_RS_PARITY,
					      BEAMFS_SUBBLOCK_DATA,
					      BEAMFS_DATA_INLINE_SUBBLOCKS);
		if (ret)
			goto out;
		cond_resched();
	}
	*enc_ns = ktime_to_ns(ktime_sub(ktime_get(), t0));

	t0 = ktime_get();
	for (i = 0; i < iters; i++) {
		ret = beamfs_rs_decode_region(data, BEAMFS_SUBBLOCK_DATA,
					      parity, BEAMFS_RS_PARITY,
					      BEAMFS_SUBBLOCK_DATA,
					      BEAMFS_DATA_INLINE_SUBBLOCKS,
					      results, positions,
					      BEAMFS_RS_PARITY / 2,
				"bench");
		if (ret < 0)
			goto out;
		cond_resched();
	}
	*dec_ns = ktime_to_ns(ktime_sub(ktime_get(), t0));
	ret = 0;

out:
	kfree(positions);
	kfree(results);
	kvfree(parity);
	kvfree(data);
	return ret;
}

/*
 * How long a burst each layout survives.
 *
 * Reported rather than asserted: a number in a file anyone can read
 * beats a BUG_ON nobody triggers, and the answer is a property of the
 * code, not of this volume.
 */
static void beamfs_rsbench_burst(struct seq_file *m)
{
	unsigned int subs = BEAMFS_DATA_INLINE_SUBBLOCKS;
	size_t dlen = BEAMFS_SUBBLOCK_DATA;
	u8 *blk, *par, *saved;
	unsigned int burst;
	unsigned int plain_limit = 0, woven_limit = 0;
	size_t i;

	blk     = kmalloc(dlen * subs, GFP_KERNEL);
	saved   = kmalloc(dlen * subs, GFP_KERNEL);
	par     = kmalloc((size_t)subs * BEAMFS_RS_PARITY, GFP_KERNEL);
	if (!blk || !saved || !par)
		goto out;

	for (i = 0; i < dlen * subs; i++)
		saved[i] = (u8)(i * 31 + 7);

	/*
	 * Walk the burst length up until each layout stops correcting.
	 * The first failure is the limit.
	 */
	for (burst = 1; burst <= 160; burst++) {
		int rc;

		/* Contiguous. */
		memcpy(blk, saved, dlen * subs);
		beamfs_rs_encode_region(blk, dlen, par, BEAMFS_RS_PARITY,
					dlen, subs);
		for (i = 0; i < burst; i++)
			blk[100 + i] ^= 0xff;
		rc = beamfs_rs_decode_region(blk, dlen, par, BEAMFS_RS_PARITY,
					     dlen, subs, NULL, NULL, 0,
					     "rsbench");
		if (rc >= 0 && plain_limit == burst - 1)
			plain_limit = burst;

		/* Interleaved. */
		memcpy(blk, saved, dlen * subs);
		beamfs_rs_encode_woven(blk, par, BEAMFS_RS_PARITY, dlen,
				       subs);
		for (i = 0; i < burst; i++)
			blk[100 + i] ^= 0xff;
		rc = beamfs_rs_decode_woven(blk, par, BEAMFS_RS_PARITY, dlen,
					    subs, NULL, "rsbench");
		if (rc >= 0 && woven_limit == burst - 1)
			woven_limit = burst;
	}

	seq_printf(m, "BURST_BYTES_CONTIGUOUS=%u\n", plain_limit);
	seq_printf(m, "BURST_BYTES_INTERLEAVED=%u\n", woven_limit);

out:
	kfree(par);
	kfree(saved);
	kfree(blk);
}

static int beamfs_rsbench_show(struct seq_file *m, void *v)
{
	u64 enc_ns = 0, dec_ns = 0;
	u32 iters = READ_ONCE(beamfs_rsbench_iters);
	u64 bytes;
	int ret;

	ret = beamfs_rsbench_run(iters, &enc_ns, &dec_ns);
	if (ret) {
		seq_printf(m, "error %d\n", ret);
		return 0;
	}

	bytes = (u64)iters * BEAMFS_DATA_INLINE_BYTES;

	/*
	 * Key=value, one per line: the harness parses this, and a
	 * format that needs a parser written for it is a format that
	 * will be parsed wrongly.
	 */
	seq_printf(m, "ITERS=%u\n", iters);
	seq_printf(m, "BLOCK_BYTES=%u\n", BEAMFS_DATA_INLINE_BYTES);
	seq_printf(m, "SUBBLOCKS=%u\n", BEAMFS_DATA_INLINE_SUBBLOCKS);
	seq_printf(m, "ENCODE_NS_TOTAL=%llu\n", enc_ns);
	seq_printf(m, "DECODE_NS_TOTAL=%llu\n", dec_ns);
	seq_printf(m, "ENCODE_NS_PER_BLOCK=%llu\n", enc_ns / iters);
	seq_printf(m, "DECODE_NS_PER_BLOCK=%llu\n", dec_ns / iters);

	/*
	 * And what a burst costs, either way.
	 *
	 * Nine consecutive bad bytes laid out contiguously all land in
	 * one codeword and it is lost; interleaved they land one per
	 * codeword and the block comes back whole. Asked here of the
	 * kernel's own codec, the one that decodes the volume.
	 */
	beamfs_rsbench_burst(m);

	/*
	 * Throughput in KiB/s, computed here rather than left to the
	 * caller: integer division in a shell script is where units go
	 * wrong.
	 */
	if (enc_ns)
		seq_printf(m, "ENCODE_KIB_PER_SEC=%llu\n",
			   div64_u64(bytes * 1000000ULL, enc_ns * 1024ULL / 1000ULL));
	if (dec_ns)
		seq_printf(m, "DECODE_KIB_PER_SEC=%llu\n",
			   div64_u64(bytes * 1000000ULL, dec_ns * 1024ULL / 1000ULL));

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(beamfs_rsbench);

/*
 * beamfs_debugfs_init -- create /sys/kernel/debug/beamfs.
 *
 * Module-wide rather than per-superblock: the codec is a property of
 * the build, not of a mounted volume, and the number it produces does
 * not change with what is mounted.
 */
void beamfs_debugfs_init(void)
{
	beamfs_debugfs_root = debugfs_create_dir("beamfs", NULL);
	if (IS_ERR(beamfs_debugfs_root)) {
		beamfs_debugfs_root = NULL;
		return;
	}

	debugfs_create_file("rs_bench", 0444, beamfs_debugfs_root, NULL,
			    &beamfs_rsbench_fops);
	debugfs_create_u32("rs_bench_iters", 0644, beamfs_debugfs_root,
			   &beamfs_rsbench_iters);
}

void beamfs_debugfs_exit(void)
{
	debugfs_remove_recursive(beamfs_debugfs_root);
	beamfs_debugfs_root = NULL;
}
