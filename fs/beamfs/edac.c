// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs - EDAC layer: CRC32 + Reed-Solomon FEC
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * Reed-Solomon encoding/decoding uses the kernel's lib/reed_solomon
 * library (RS(255,239) over GF(2^8), primitive polynomial 0x187)
 * rather than a private codec: the library is already in the kernel,
 * well tested, and used by NAND MTD drivers, the RAM oops logger and
 * dm-verity FEC.
 *
 * RS parameters, as passed to init_rs():
 *   symsize = 8                  (GF(2^8))
 *   fcr     = 0                  (first consecutive root)
 *   prim    = 1                  (primitive element)
 *   nroots  = BEAMFS_RS_PARITY = 16 (parity symbols)
 *   data per subblock: BEAMFS_SUBBLOCK_DATA = 239 bytes
 *   codeword length:   255 bytes (BEAMFS_SUBBLOCK_TOTAL)
 */

#include <linux/kernel.h>
#include <linux/crc32.h>
#include <linux/slab.h>
#include <linux/rslib.h>
#include "beamfs.h"

/*
 * RS codec handles -- ONE PER POSSIBLE CPU.
 *
 * lib/reed_solomon/decode_rs.c uses scratch buffers (lambda, syn, b,
 * t, omega, root, reg, loc) stored inside rs_control->buffers[]. A
 * shared rs_control across concurrent callers races on those buffers,
 * producing spurious 'uncorrectable' results under parallel decode
 * load (reproduced May 2026: 8 parallel sha256sum on a beamfs RO
 * mount -> 160+ uncorrectable per batch).
 *
 * This is the library's documented contract: rslib.h describes
 * rs_control as per instance, and decode_rs8()'s kernel-doc says the
 * caller has to ensure that decoder invocations are serialized.
 *
 * Per-CPU allocation gives each CPU its own rs_control + buffers[].
 * Combined with preempt-disable via get_cpu_ptr/put_cpu_ptr, this
 * eliminates the race without locks.
 */
static struct rs_control * __percpu *beamfs_rs_ctrl_pcpu;

/*
 * A codeword's worth of scratch, per cpu.
 *
 * Interleaving means a codeword's symbols are not contiguous, and
 * encode_rs8 takes a contiguous buffer: lib/reed_solomon has no notion
 * of a stride and is not ours to change. So the symbols are gathered
 * into this and handed over.
 *
 * Per cpu rather than on the stack: 239 bytes is a quarter of a
 * kernel frame, and this sits under iomap and writeback, where the
 * frame is already deep. Per cpu rather than from the scratch pool:
 * this file has no super_block and should not need one -- it is the
 * codec, not the filesystem.
 *
 * Taken with get_cpu_ptr, which disables preemption for the length of
 * one codeword: 239 bytes of Galois arithmetic, microseconds.
 */
static u8 * __percpu *beamfs_rs_scratch_pcpu;

/*
 * beamfs_rs_init_tables - initialize the RS codec
 * Called once from beamfs_init() before any mount.
 *
 * Returns 0, or -ENOMEM when the per-CPU codecs cannot be set up.
 * A CPU left without a scratch buffer is not an error.
 */
int beamfs_rs_init_tables(void)
{
	unsigned int cpu, missing = 0;
	struct rs_control *ctrl;

	/*
	 * init_rs(symsize=8, gfpoly=0x187, fcr=0, prim=1, nroots=16)
	 *   -> GF(2^8), primitive poly x^8+x^7+x^2+x+1, 16 parity symbols
	 *      (corrects up to 8 errors per shortened RS(255,239) subblock).
	 *
	 * Per-CPU: alloc the per-CPU pointer slot, then init_rs once per
	 * possible CPU. Each rs_control carries its own scratch buffers[].
	 */
	beamfs_rs_scratch_pcpu = alloc_percpu(u8 *);
	if (!beamfs_rs_scratch_pcpu) {
		/*
		 * Not fatal: the contiguous paths never touch it and the
		 * interleaved ones check before use, so a volume that
		 * needs interleaving refuses rather than the module.
		 */
		pr_err("beamfs: no per-CPU RS scratch; interleaved blocks will be refused\n");
	}

	beamfs_rs_ctrl_pcpu = alloc_percpu(struct rs_control *);
	if (!beamfs_rs_ctrl_pcpu) {
		free_percpu(beamfs_rs_scratch_pcpu);
		beamfs_rs_scratch_pcpu = NULL;
		pr_err("beamfs: failed to alloc per-CPU RS ctrl array\n");
		return -ENOMEM;
	}

	for_each_possible_cpu(cpu) {
		ctrl = init_rs(8, 0x187, 0, 1, BEAMFS_RS_PARITY);
		if (!ctrl) {
			pr_err("beamfs: failed to init RS codec for CPU %u\n",
			       cpu);
			beamfs_rs_exit_tables();
			return -ENOMEM;
		}
		*per_cpu_ptr(beamfs_rs_ctrl_pcpu, cpu) = ctrl;
		/*
		 * Not fatal either way: the contiguous paths do not use
		 * the scratch, and the interleaved ones check for it.
		 * Without the per-CPU array there is nowhere to keep one.
		 */
		if (beamfs_rs_scratch_pcpu) {
			u8 *scratch = kmalloc(BEAMFS_SUBBLOCK_DATA, GFP_KERNEL);

			if (!scratch)
				missing++;
			*per_cpu_ptr(beamfs_rs_scratch_pcpu, cpu) = scratch;
		}
	}

	if (missing)
		pr_warn("beamfs: no RS scratch on %u CPU(s); interleaved blocks will be refused there\n",
			missing);

	pr_debug("beamfs: RS codec initialized per-CPU (RS(%d,%d), %u CPUs)\n",
		 BEAMFS_SUBBLOCK_TOTAL, BEAMFS_SUBBLOCK_DATA,
		 num_possible_cpus());
	return 0;
}

/*
 * beamfs_rs_exit_tables - release the RS codec, at module exit or after
 * a failed beamfs_rs_init_tables
 */
void beamfs_rs_exit_tables(void)
{
	unsigned int cpu;
	struct rs_control *ctrl;

	if (!beamfs_rs_ctrl_pcpu)
		return;

	for_each_possible_cpu(cpu) {
		ctrl = *per_cpu_ptr(beamfs_rs_ctrl_pcpu, cpu);
		if (ctrl) {
			free_rs(ctrl);
			*per_cpu_ptr(beamfs_rs_ctrl_pcpu, cpu) = NULL;
		}
	}

	if (beamfs_rs_scratch_pcpu) {
		unsigned int cpu2;

		for_each_possible_cpu(cpu2)
			kfree(*per_cpu_ptr(beamfs_rs_scratch_pcpu, cpu2));
		free_percpu(beamfs_rs_scratch_pcpu);
		beamfs_rs_scratch_pcpu = NULL;
	}

	free_percpu(beamfs_rs_ctrl_pcpu);
	beamfs_rs_ctrl_pcpu = NULL;
}

/*
 * Shannon entropy term LUT in Q16.16 fixed point.
 *
 * LUT[N][k] = round(-(k/N) * log2(k/N) * 2^16) for N in [1,8],
 *           = 0 for k == 0 (or N == 0, unused).
 *
 * N is the number of corrected positions in a codeword, at most
 * BEAMFS_RS_PARITY / 2, and k how many of them fall in one bin. The
 * entries are generated from the formula above, not typed: a change
 * to BEAMFS_RS_PARITY means regenerating the table, not editing it.
 */
static const __u32 beamfs_rs_entropy_term_q16_16[9][9] = {
	[0] = { 0, },	/* [0] unused */
	[1] = {     0,     0,     0,     0,     0,     0,     0,     0,     0 },
	[2] = {     0, 32768,     0,     0,     0,     0,     0,     0,     0 },
	[3] = {     0, 34624, 25557,     0,     0,     0,     0,     0,     0 },
	[4] = {     0, 32768, 32768, 20400,     0,     0,     0,     0,     0 },
	[5] = {     0, 30434, 34654, 28979, 16878,     0,     0,     0,     0 },
	[6] = {     0, 28235, 34624, 32768, 25557, 14365,     0,     0,     0 },
	[7] = {     0, 26283, 33842, 34333, 30235, 22724, 12493,     0,     0 },
	[8] = {     0, 24576, 32768, 34776, 32768, 27774, 20400, 11047,     0 },
};

/*
 * beamfs_rs_compute_entropy_q16_16 -- Shannon entropy estimator over the
 * spatial distribution of corrected byte positions in an RS codeword.
 *
 * @positions:      byte positions corrected (output of beamfs_rs_decode)
 * @n_positions:    number of valid entries; MUST be in [1, BEAMFS_RS_PARITY/2]
 * @code_len_bytes: data length of the codeword, in [n_positions, 239]
 *
 * Returns Shannon H = sum_{bin} LUT[N][k_bin] in Q16.16 fixed point,
 * range [0, 3*65536). Deterministic, no FPU; one u32 division per
 * position for bin index computation.
 *
 * Pre: positions != NULL, 1 <= n_positions <= BEAMFS_RS_PARITY/2,
 *      code_len_bytes >= n_positions
 *
 * Caller decides whether to set BEAMFS_RS_EVENT_FLAG_ENTROPY_VALID:
 * a single sample (n_positions == 1) yields H = 0 mathematically
 * but is forensically non-significant; beamfs_log_rs_event_flagged
 * does not call this for one and leaves the flag clear.
 */
__u32 beamfs_rs_compute_entropy_q16_16(const u8 *positions,
				       unsigned int n_positions,
				      size_t code_len_bytes)
{
	unsigned int bin_count[BEAMFS_RS_ENTROPY_BINS];
	__u32 h = 0;
	unsigned int i, b;

	if (WARN_ON_ONCE(!positions))
		return 0;
	if (WARN_ON_ONCE(n_positions == 0 ||
			 n_positions > BEAMFS_RS_PARITY / 2))
		return 0;
	if (WARN_ON_ONCE(code_len_bytes < n_positions ||
			 code_len_bytes > BEAMFS_SUBBLOCK_DATA))
		return 0;

	memset(bin_count, 0, sizeof(bin_count));

	for (i = 0; i < n_positions; i++) {
		/* bin = pos * BINS / code_len. u32 product fits since
		 * pos < 239 and BINS = 8, max product = 238 * 8 = 1904.
		 */
		unsigned int idx = (unsigned int)positions[i];
		unsigned int bin;

		if (WARN_ON_ONCE(idx >= code_len_bytes))
			return 0;
		bin = (idx * BEAMFS_RS_ENTROPY_BINS) /
		      (unsigned int)code_len_bytes;
		if (WARN_ON_ONCE(bin >= BEAMFS_RS_ENTROPY_BINS))
			return 0;
		bin_count[bin]++;
	}

	/* H = sum over bins of LUT[N][k_bin]. Bins with k=0 contribute 0
	 * by LUT convention. Total guaranteed < 3 * 65536.
	 */
	for (b = 0; b < BEAMFS_RS_ENTROPY_BINS; b++)
		h += beamfs_rs_entropy_term_q16_16[n_positions][bin_count[b]];

	return h;
}

/*
 * beamfs_rs_encode - encode @len data bytes, produce BEAMFS_RS_PARITY parity
 * @data:   input data (@len bytes, must be <= BEAMFS_SUBBLOCK_DATA)
 * @len:    number of data bytes (BEAMFS_SUBBLOCK_DATA for the bitmap path,
 *          BEAMFS_INODE_RS_DATA for inodes, etc.)
 * @parity: output parity (BEAMFS_RS_PARITY bytes)
 *
 * lib/reed_solomon supports shortened RS codes natively: passing a
 * length less than BEAMFS_SUBBLOCK_DATA produces a valid codeword,
 * mathematically equivalent to padding the data with zeros up to the
 * full subblock length. The same length must be passed to the matching
 * beamfs_rs_decode call.
 *
 * encode_rs8() takes the data as bytes and returns the parity in a u16
 * buffer, one symbol in the low byte of each entry.
 */
int beamfs_rs_encode(u8 *data, size_t len, u8 *parity)
{
	u16 par[BEAMFS_RS_PARITY];
	struct rs_control **ctrl_p;
	struct rs_control *ctrl;
	int i;

	if (!beamfs_rs_ctrl_pcpu)
		return -EINVAL;
	if (len > BEAMFS_SUBBLOCK_DATA)
		return -EINVAL;

	memset(par, 0, sizeof(par));
	ctrl_p = get_cpu_ptr(beamfs_rs_ctrl_pcpu);
	ctrl = *ctrl_p;
	if (!ctrl) {
		put_cpu_ptr(beamfs_rs_ctrl_pcpu);
		return -EINVAL;
	}
	encode_rs8(ctrl, data, len, par, 0);
	put_cpu_ptr(beamfs_rs_ctrl_pcpu);

	for (i = 0; i < BEAMFS_RS_PARITY; i++)
		parity[i] = (u8)par[i];

	return 0;
}

/*
 * beamfs_rs_decode -- decode and correct a shortened RS codeword in place,
 *                    optionally exposing the list of corrected byte
 *                    positions for forensic entropy logging.
 *
 * @data:          data bytes (@len bytes), corrected in place on success
 * @len:           number of data bytes (must match the length passed to encode)
 * @parity:        parity bytes (BEAMFS_RS_PARITY); read, never rewritten
 * @positions:     optional output, up to @max_positions entries; on a
 *                 positive return holds the corrected DATA byte positions
 *                 in [0, len), ascending. May be NULL if no entropy
 *                 logging is needed.
 * @max_positions: capacity of @positions; ignored if @positions is NULL.
 *
 * Returns:
 *   < 0  uncorrectable (-EBADMSG) or invalid input (-EINVAL)
 *   = 0  no errors detected
 *   > 0  with @positions: the number of DATA bytes corrected, which is
 *        the number of entries written to @positions; without it, the
 *        number of symbols corrected, parity included.
 *
 * decode_rs8() either applies its corrections or reports them (see
 * lib/reed_solomon/decode_rs.c: with eras_pos and corr it stores each
 * correction's position and pattern and touches nothing). Asked to
 * report, we apply the data corrections ourselves and leave the
 * parity alone: the 239-byte snapshot this function used to take, to
 * find the corrected bytes by diff, is gone from the stack. A parity
 * symbol decode_rs8 would have corrected is not counted when the
 * caller wants positions, so that the count it gets is the count of
 * entries it may read: alloc.c and the others hand that count on to
 * the journal.
 */
int beamfs_rs_decode(u8 *data, size_t len, u8 *parity,
		     u8 *positions, unsigned int max_positions,
		    const char *who)
{
	u16 par[BEAMFS_RS_PARITY];
	u16 corr[BEAMFS_RS_PARITY];
	int pos[BEAMFS_RS_PARITY];
	int i, j, nerr;
	unsigned int n_data = 0;

	if (!beamfs_rs_ctrl_pcpu)
		return -EINVAL;
	if (len > BEAMFS_SUBBLOCK_DATA)
		return -EINVAL;
	if (positions && max_positions == 0)
		return -EINVAL;

	for (i = 0; i < BEAMFS_RS_PARITY; i++)
		par[i] = parity[i];

	/*
	 * Per-CPU: get_cpu_ptr disables preemption while decode runs in
	 * the CPU-local rs_control. decode_rs8 is pure computation, no
	 * allocs, no sleeps -- preempt-disable is safe.
	 */
	{
		struct rs_control **ctrl_p = get_cpu_ptr(beamfs_rs_ctrl_pcpu);
		struct rs_control *ctrl = *ctrl_p;

		if (!ctrl) {
			put_cpu_ptr(beamfs_rs_ctrl_pcpu);
			return -EINVAL;
		}
		nerr = decode_rs8(ctrl, data, par, len,
				  NULL, 0, pos, 0, corr);
		put_cpu_ptr(beamfs_rs_ctrl_pcpu);
	}

	if (nerr < 0) {
		/*
		 * Silent here, on purpose.
		 *
		 * This function knows a length and nothing else -- not the
		 * block, not which of the sixteen subblocks, not what owns
		 * it. Every caller has results[] and its own context, and
		 * reports from there.
		 */
		return -EBADMSG;
	}
	if (nerr > BEAMFS_RS_PARITY)
		return -EBADMSG;

	/*
	 * Positions come out in locator order; the journal and its
	 * entropy estimate were written for ascending ones.
	 */
	for (i = 1; i < nerr; i++) {
		int p = pos[i];
		u16 c = corr[i];

		for (j = i - 1; j >= 0 && pos[j] > p; j--) {
			pos[j + 1] = pos[j];
			corr[j + 1] = corr[j];
		}
		pos[j + 1] = p;
		corr[j + 1] = c;
	}

	for (i = 0; i < nerr; i++) {
		if (pos[i] < 0 || pos[i] >= (int)len)
			continue;	/* a parity symbol: the copy is dropped */
		data[pos[i]] ^= (u8)corr[i];
		if (positions && n_data < max_positions)
			positions[n_data] = (u8)pos[i];
		n_data++;
	}

	if (positions)
		return (int)min(n_data, max_positions);
	return nerr;
}

/*
 * Where symbol @i of codeword @sub lives once interleaved.
 *
 * Contiguous, codeword j owns bytes [j*239, (j+1)*239): a nine-byte
 * burst lands entirely in it and takes it past correction.
 *
 * Interleaved, the same burst puts at most one symbol in any codeword.
 * 129 consecutive bytes are needed to lose one, for not a byte of
 * extra parity.
 *
 * The parity stays with its codeword, sixteen bytes in a row: nine bad
 * bytes among them still lose that codeword. Spreading the parity as
 * well would take a different on-disk layout.
 */
static inline size_t rs_woven_off(unsigned int sub, size_t i,
				  unsigned int n_subblocks)
{
	return i * n_subblocks + sub;
}

/*
 * Gather one codeword out of an interleaved block.
 *
 * @scratch must hold @data_len bytes. The caller owns it: this runs on
 * the writeback path and an allocation here would be one more thing to
 * fail under the memory pressure that writeback is trying to relieve.
 */
static void rs_gather(const u8 *blk, unsigned int sub, size_t data_len,
		      unsigned int n_subblocks, u8 *scratch)
{
	size_t i;

	for (i = 0; i < data_len; i++)
		scratch[i] = blk[rs_woven_off(sub, i, n_subblocks)];
}

static void rs_scatter(u8 *blk, unsigned int sub, size_t data_len,
		       unsigned int n_subblocks, const u8 *scratch)
{
	size_t i;

	for (i = 0; i < data_len; i++)
		blk[rs_woven_off(sub, i, n_subblocks)] = scratch[i];
}

/*
 * beamfs_rs_encode_woven -- encode a block whose symbols are interleaved.
 *
 * @data_buf:      the interleaved data area, @data_len * @n_subblocks bytes
 * @parity_buf:    where the parity goes, @n_subblocks codewords of it
 * @parity_stride: bytes from one codeword's parity to the next
 *
 * Same contract as beamfs_rs_encode_region, same return values. The
 * difference is where the symbols of a codeword are read from.
 */
int beamfs_rs_encode_woven(u8 *data_buf, u8 *parity_buf, size_t parity_stride,
			   size_t data_len, unsigned int n_subblocks)
{
	unsigned int i;
	int ret = 0;
	u8 **sp, *scratch;

	if (!data_buf || !parity_buf || !beamfs_rs_scratch_pcpu)
		return -EINVAL;
	if (data_len > BEAMFS_SUBBLOCK_DATA)
		return -EINVAL;

	/*
	 * Preemption off for the whole block rather than per codeword:
	 * sixteen encodes of 239 bytes, and taking the pointer sixteen
	 * times costs more than holding it once.
	 */
	sp = get_cpu_ptr(beamfs_rs_scratch_pcpu);
	scratch = *sp;
	if (!scratch) {
		put_cpu_ptr(beamfs_rs_scratch_pcpu);
		return -ENOMEM;
	}

	for (i = 0; i < n_subblocks; i++) {
		rs_gather(data_buf, i, data_len, n_subblocks, scratch);
		ret = beamfs_rs_encode(scratch, data_len,
				       parity_buf + (size_t)i * parity_stride);
		if (ret < 0)
			break;
	}

	put_cpu_ptr(beamfs_rs_scratch_pcpu);
	return ret < 0 ? ret : 0;
}

/*
 * beamfs_rs_decode_woven -- decode one, correcting in place.
 *
 * A codeword that decodes is written back into the block; one that
 * does not is left as it was, so a caller that ignores the result
 * reads what the medium gave rather than a half-corrected mixture.
 */
int beamfs_rs_decode_woven(u8 *data_buf, u8 *parity_buf, size_t parity_stride,
			   size_t data_len, unsigned int n_subblocks,
			   int *results, const char *who)
{
	unsigned int i;
	int worst = 0;
	u8 **sp, *scratch;

	if (!data_buf || !parity_buf || !beamfs_rs_scratch_pcpu)
		return -EINVAL;
	if (data_len > BEAMFS_SUBBLOCK_DATA)
		return -EINVAL;

	sp = get_cpu_ptr(beamfs_rs_scratch_pcpu);
	scratch = *sp;
	if (!scratch) {
		put_cpu_ptr(beamfs_rs_scratch_pcpu);
		return -ENOMEM;
	}

	for (i = 0; i < n_subblocks; i++) {
		int rc;

		rs_gather(data_buf, i, data_len, n_subblocks, scratch);
		rc = beamfs_rs_decode(scratch, data_len,
				      parity_buf + (size_t)i * parity_stride,
				      NULL, 0, who);
		if (results)
			results[i] = rc;
		if (rc < 0) {
			worst = rc;
			continue;
		}
		rs_scatter(data_buf, i, data_len, n_subblocks, scratch);
		if (rc > worst)
			worst = rc;
	}

	put_cpu_ptr(beamfs_rs_scratch_pcpu);
	return worst;
}

/*
 * Encode a 4096-byte block in the grouped layout.
 *
 * The block is its own argument rather than a data pointer and a
 * parity pointer: the two are at fixed offsets within it and passing
 * them apart invites the mismatch that cost an afternoon -- woven
 * functions expecting a compact area, callers handing them an
 * alternating one.
 *
 * Returns 0, or the first encode failure.
 */
int beamfs_rs_encode_block(u8 *block)
{
	if (!block)
		return -EINVAL;

	return beamfs_rs_encode_woven(block + BEAMFS_DATA_WOVEN_OFF,
				      block + BEAMFS_DATA_WOVEN_PARITY_OFF,
				      BEAMFS_RS_PARITY,
				      BEAMFS_SUBBLOCK_DATA,
				      BEAMFS_DATA_INLINE_SUBBLOCKS);
}

/*
 * Decode one, correcting in place.
 *
 * @results, when given, holds one entry per codeword: the number of
 * symbols corrected, or negative for one that would not decode.
 *
 * Returns the worst of those, so a caller that wants only "is this
 * block sound" can test the return and a caller that wants to log
 * which codewords suffered has the array.
 */
int beamfs_rs_decode_block(u8 *block, int *results, const char *who)
{
	if (!block)
		return -EINVAL;

	return beamfs_rs_decode_woven(block + BEAMFS_DATA_WOVEN_OFF,
				      block + BEAMFS_DATA_WOVEN_PARITY_OFF,
				      BEAMFS_RS_PARITY,
				      BEAMFS_SUBBLOCK_DATA,
				      BEAMFS_DATA_INLINE_SUBBLOCKS,
				      results, who);
}

/*
 * Read @len bytes of user data at @off out of a block.
 *
 * Grouped, the data is contiguous but the symbols of a codeword are
 * not: byte @off of the user's data is at @off within the data area,
 * because interleaving moves symbols within a codeword and not the
 * bytes the user sees. The two are the same array read two ways.
 *
 * This exists so nothing outside edac has to know that.
 */
void beamfs_block_read(const u8 *block, size_t off, size_t len, u8 *out)
{
	memcpy(out, block + BEAMFS_DATA_WOVEN_OFF + off, len);
}

void beamfs_block_write(u8 *block, size_t off, size_t len, const u8 *in)
{
	memcpy(block + BEAMFS_DATA_WOVEN_OFF + off, in, len);
}

/*
 * beamfs_rs_encode_region -- encode N RS(255,239) shortened subblocks
 * across a region.
 *
 * @data_buf:      base pointer of the data region
 * @data_stride:   distance in bytes between the start of two
 *                 consecutive data subblocks (e.g.
 *                 BEAMFS_SUBBLOCK_TOTAL=255 for the bitmap)
 * @parity_buf:    base pointer of the parity region (may equal
 *                 data_buf + data_len when data and parity alternate)
 * @parity_stride: distance between the start of two consecutive
 *                 parity blobs (e.g. BEAMFS_SUBBLOCK_TOTAL=255 for
 *                 the bitmap, BEAMFS_RS_PARITY=16 for contiguous
 *                 parity placement)
 * @data_len:      number of data bytes per subblock (e.g.
 *                 BEAMFS_SUBBLOCK_DATA=239 for a full subblock)
 * @n_subblocks:   number of subblocks to encode
 *
 * Returns 0 on success, negative on the first encode failure.
 * On failure the buffer is in an indeterminate state.
 */
int beamfs_rs_encode_region(u8 *data_buf, size_t data_stride,
			    u8 *parity_buf, size_t parity_stride,
			   size_t data_len, unsigned int n_subblocks)
{
	unsigned int i;

	if (!data_buf || !parity_buf)
		return -EINVAL;

	/*
	 * Contiguous only: codeword i is @data_len bytes at
	 * data_buf + i * data_stride, its parity at
	 * parity_buf + i * parity_stride. Nothing here interleaves.
	 *
	 * A data block on a volume with BEAMFS_FEATURE_INCOMPAT_RS_INTERLEAVE
	 * goes to beamfs_rs_encode_woven instead; beamfs_seal_data_block
	 * makes that choice. Data blocks on other volumes, directory blocks,
	 * parity region blocks and the bitmap come here with a stride of
	 * BEAMFS_SUBBLOCK_TOTAL, data and parity alternating every 255
	 * bytes. The superblock, the indirect-block slots and rsbench pass
	 * other strides.
	 */

	for (i = 0; i < n_subblocks; i++) {
		u8 *d = data_buf   + (size_t)i * data_stride;
		u8 *p = parity_buf + (size_t)i * parity_stride;
		int rc = beamfs_rs_encode(d, data_len, p);

		if (rc < 0)
			return rc;
	}
	return 0;
}

/*
 * beamfs_rs_decode_region -- decode N RS(255,239) shortened subblocks
 * across a region. Symmetric to beamfs_rs_encode_region.
 *
 * @results:          optional, n_subblocks entries. On exit each
 *                    entry holds the return value of beamfs_rs_decode
 *                    for the corresponding subblock: < 0 uncorrectable,
 *                    = 0 no errors, > 0 number of corrected symbols.
 *                    Pass NULL to skip per-subblock reporting.
 * @positions_buf:    optional, flat array of n_subblocks * positions_stride
 *                    int entries. On a positive results[i], the first
 *                    results[i] entries of positions_buf[i*stride ..] hold
 *                    the corrected DATA positions for subblock i. May be
 *                    NULL (then positions_stride must be 0).
 * @positions_stride: slot count per subblock in positions_buf; should be
 *                    >= BEAMFS_RS_PARITY/2. Ignored if positions_buf NULL.
 *
 * Returns 0 if every subblock decoded successfully, the first
 * negative error otherwise. Decoding does not stop on error: all
 * subblocks are processed, results[] reflects the per-subblock
 * outcome, and the first negative error is the one returned.
 */
int beamfs_rs_decode_region(u8 *data_buf, size_t data_stride,
			    u8 *parity_buf, size_t parity_stride,
			   size_t data_len, unsigned int n_subblocks,
			   int *results,
			   u8 *positions_buf,
			   unsigned int positions_stride,
			   const char *who)
{
	unsigned int i;
	int worst = 0;

	if (!data_buf || !parity_buf)
		return -EINVAL;
	if (positions_buf && positions_stride == 0)
		return -EINVAL;
	if (!positions_buf && positions_stride != 0)
		return -EINVAL;

	for (i = 0; i < n_subblocks; i++) {
		u8 *d = data_buf   + (size_t)i * data_stride;
		u8 *p = parity_buf + (size_t)i * parity_stride;
		u8 *pos = positions_buf
			? positions_buf + (size_t)i * positions_stride
			: NULL;
		int rc = beamfs_rs_decode(d, data_len, p,
					 pos, positions_stride, who);

		if (results)
			results[i] = rc;
		if (rc < 0 && worst >= 0)
			worst = rc;
	}
	return worst;
}

/*
 * beamfs_crc32 - compute CRC32 checksum
 * @buf: data buffer
 * @len: length in bytes
 *
 * Uses the kernel's hardware-accelerated crc32_le (same as ext4/btrfs).
 * Seed 0xFFFFFFFF, final XOR 0xFFFFFFFF (standard CRC-32/ISO-HDLC).
 */
__u32 beamfs_crc32(const void *buf, size_t len)
{
	return crc32_le(0xFFFFFFFF, buf, len) ^ 0xFFFFFFFF;
}

/*
 * beamfs_crc32_sb -- compute CRC32 over the meaningful regions of the
 *                   superblock, excluding s_crc32 itself and s_pad.
 *
 * Coverage (computed at compile time from struct layout):
 *   region A: [0, offsetof(s_crc32))         -- magic, counters,
 *                                               version, flags
 *   region B: [offsetof(s_uuid), offsetof(s_pad)) -- uuid, label,
 *                                               RS journal,
 *                                               bitmap_blk, features,
 *                                               protection scheme
 * Total coverage: BEAMFS_SB_RS_COVERAGE_BYTES, asserted at build time
 * by BUILD_BUG_ON below.
 *
 * Chained via crc32_le without intermediate XOR. Must match the
 * userspace mkfs.beamfs::crc32_sb() byte-for-byte so that superblocks
 * formatted by mkfs validate at mount time.
 *
 * On-disk format compatibility: a superblock written by an mkfs whose
 * struct layout differs from this kernel's fails this CRC. Images of
 * an older on-disk version do not get this far: fill_super refuses
 * them on s_version first.
 */
__u32 beamfs_crc32_sb(const struct beamfs_super_block *fsb)
{
	const u8 *base = (const u8 *)fsb;
	const size_t off_crc32 = offsetof(struct beamfs_super_block, s_crc32);
	const size_t off_uuid  = offsetof(struct beamfs_super_block, s_uuid);
	const size_t off_pad   = offsetof(struct beamfs_super_block, s_pad);
	u32 c;

	BUILD_BUG_ON(sizeof_field(struct beamfs_super_block, s_crc32) != 4);
	BUILD_BUG_ON(off_uuid != off_crc32 + 4);
	BUILD_BUG_ON(off_crc32 + (off_pad - off_uuid) !=
		     BEAMFS_SB_RS_COVERAGE_BYTES);

	c = crc32_le(0xFFFFFFFF, base, off_crc32);
	c = crc32_le(c, base + off_uuid, off_pad - off_uuid);
	return c ^ 0xFFFFFFFF;
}
