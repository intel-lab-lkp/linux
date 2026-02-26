// SPDX-License-Identifier: GPL-2.0

#include "ext4.h"
#include "fast_commit_bytelog.h"

#include <linux/crc32c.h>
#include <linux/dax.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/libnvdimm.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <asm/barrier.h>

#define EXT4_FC_BYTELOG_META_BLOCKS	1

static void ext4_fc_bytelog_reset_batch(struct ext4_fc_bytelog *log);
static int ext4_fc_bytelog_flush_batch(struct super_block *sb, u32 tid);

#define EXT4_FC_CRC32C_POLY	0x82f63b78
#define EXT4_FC_CRC32C_SHIFT_BITS	(sizeof(size_t) * 8)

static u32 ext4_fc_crc32c_shift_mats[EXT4_FC_CRC32C_SHIFT_BITS][32];
static bool ext4_fc_crc32c_shift_mats_ready;

static u32 ext4_fc_gf2_matrix_times(const u32 *mat, u32 vec)
{
	u32 sum = 0;
	int i;

	for (i = 0; i < 32; i++) {
		if (vec & 1)
			sum ^= mat[i];
		vec >>= 1;
	}

	return sum;
}

static void ext4_fc_gf2_matrix_square(u32 *square, const u32 *mat)
{
	int i;

	for (i = 0; i < 32; i++)
		square[i] = ext4_fc_gf2_matrix_times(mat, mat[i]);
}

static void ext4_fc_crc32c_shift_mats_init_once(void)
{
	static DEFINE_MUTEX(lock);
	u32 even[32], odd[32], one_byte[32];
	u32 row = 1;
	int i;

	if (READ_ONCE(ext4_fc_crc32c_shift_mats_ready))
		return;

	mutex_lock(&lock);
	if (ext4_fc_crc32c_shift_mats_ready)
		goto out;

	/*
	 * Build the GF(2) matrix operator for shifting by one byte of zeros,
	 * then square it repeatedly to get powers of two.
	 */
	odd[0] = EXT4_FC_CRC32C_POLY;
	for (i = 1; i < 32; i++) {
		odd[i] = row;
		row <<= 1;
	}
	ext4_fc_gf2_matrix_square(even, odd);	/* 2 zero bits */
	ext4_fc_gf2_matrix_square(odd, even);	/* 4 zero bits */
	ext4_fc_gf2_matrix_square(one_byte, odd); /* 8 zero bits */

	memcpy(ext4_fc_crc32c_shift_mats[0], one_byte, sizeof(one_byte));
	for (i = 1; i < EXT4_FC_CRC32C_SHIFT_BITS; i++)
		ext4_fc_gf2_matrix_square(ext4_fc_crc32c_shift_mats[i],
					  ext4_fc_crc32c_shift_mats[i - 1]);

	WRITE_ONCE(ext4_fc_crc32c_shift_mats_ready, true);
out:
	mutex_unlock(&lock);
}

static u32 ext4_fc_crc32c_shift_zeros(u32 crc, size_t len)
{
	size_t shift = len;
	int bit = 0;

	while (shift) {
		if (shift & 1)
			crc = ext4_fc_gf2_matrix_times(ext4_fc_crc32c_shift_mats[bit], crc);
		shift >>= 1;
		bit++;
	}

	return crc;
}

u32 ext4_fc_bytelog_crc32(const void *buf, size_t len)
{
	return crc32c(~0, buf, len);
}

bool ext4_fc_bytelog_mapped(struct ext4_sb_info *sbi)
{
	return READ_ONCE(sbi->s_fc_bytelog.mapped);
}

bool ext4_fc_bytelog_active(struct ext4_sb_info *sbi)
{
	struct ext4_fc_bytelog *log = &sbi->s_fc_bytelog;

	return log->mapped && log->enabled;
}

size_t ext4_fc_bytelog_record_size(size_t payload_len)
{
	size_t len = sizeof(struct ext4_fc_bytelog_hdr) + payload_len;

	return ALIGN(len, EXT4_FC_BYTELOG_ALIGN);
}

void ext4_fc_bytelog_prep_hdr(struct ext4_fc_bytelog_hdr *hdr, u16 tag,
			      u16 flags, u32 tid, u64 seq, u32 payload_len)
{
	memset(hdr, 0, sizeof(*hdr));

	hdr->magic = cpu_to_le32(EXT4_FC_BYTELOG_MAGIC);
	hdr->version = cpu_to_le16(EXT4_FC_BYTELOG_VERSION);
	hdr->hdr_len = cpu_to_le16(sizeof(*hdr));
	hdr->tid = cpu_to_le32(tid);
	hdr->tag = cpu_to_le16(tag);
	hdr->flags = cpu_to_le16(flags & ~EXT4_FC_BYTELOG_COMMITTED);
	hdr->payload_len = cpu_to_le32(payload_len);
	hdr->record_len = cpu_to_le32(ext4_fc_bytelog_record_size(payload_len));
	hdr->seq = cpu_to_le64(seq);
}

void ext4_fc_bytelog_finalize_hdr_crc(struct ext4_fc_bytelog_hdr *hdr,
				      u32 payload_crc)
{
	struct ext4_fc_bytelog_hdr tmp;
	u32 crc;

	hdr->payload_crc = cpu_to_le32(payload_crc);
	hdr->header_crc = 0;

	tmp = *hdr;
	tmp.header_crc = 0;
	crc = ext4_fc_bytelog_crc32(&tmp, sizeof(tmp));
	hdr->header_crc = cpu_to_le32(crc);
}

static bool ext4_fc_bytelog_record_sane(const struct ext4_fc_bytelog_hdr *hdr,
					size_t remaining)
{
	u32 record_len = le32_to_cpu(hdr->record_len);
	u32 payload_len = le32_to_cpu(hdr->payload_len);
	u16 hdr_len = le16_to_cpu(hdr->hdr_len);

	if (le32_to_cpu(hdr->magic) != EXT4_FC_BYTELOG_MAGIC)
		return false;
	if (le16_to_cpu(hdr->version) != EXT4_FC_BYTELOG_VERSION)
		return false;
	if (hdr_len != sizeof(*hdr))
		return false;
	if (!record_len || record_len > remaining)
		return false;
	if (!IS_ALIGNED(record_len, EXT4_FC_BYTELOG_ALIGN))
		return false;
	if (record_len < hdr_len)
		return false;
	if (payload_len > record_len - hdr_len)
		return false;

	return true;
}

int ext4_fc_bytelog_validate_hdr(const struct ext4_fc_bytelog_hdr *hdr,
				 size_t remaining, const void *payload)
{
	struct ext4_fc_bytelog_hdr tmp;
	u32 payload_len = le32_to_cpu(hdr->payload_len);
	u32 crc;

	if (!ext4_fc_bytelog_record_sane(hdr, remaining))
		return -EINVAL;

	tmp = *hdr;
	tmp.header_crc = 0;
	crc = ext4_fc_bytelog_crc32(&tmp, sizeof(tmp));
	if (crc != le32_to_cpu(hdr->header_crc))
		return -EFSBADCRC;

	if (!payload_len)
		return 0;
	if (!payload)
		return -EINVAL;

	crc = ext4_fc_bytelog_crc32(payload, payload_len);
	if (crc != le32_to_cpu(hdr->payload_crc))
		return -EFSBADCRC;

	return 0;
}

void ext4_fc_bytelog_mark_committed(struct ext4_fc_bytelog_hdr *hdr)
{
	u16 flags = le16_to_cpu(hdr->flags);
	struct ext4_fc_bytelog_hdr tmp;
	u32 crc;

	flags |= EXT4_FC_BYTELOG_COMMITTED;
	hdr->flags = cpu_to_le16(flags);

	tmp = *hdr;
	tmp.header_crc = 0;
	crc = ext4_fc_bytelog_crc32(&tmp, sizeof(tmp));
	hdr->header_crc = cpu_to_le32(crc);
}

void ext4_fc_bytelog_flush_persist(void *addr, size_t len)
{
	u8 *p = addr;
	size_t off = 0;

	if (!len)
		return;

	/*
	 * Large flushes can be very bursty. Chunk the flush so other tasks
	 * can make progress between chunks.
	 */
	if (len <= 65536) {
		arch_wb_cache_pmem(p, len);
		return;
	}

	while (off < len) {
		size_t n = min(len - off, (size_t)65536);

		arch_wb_cache_pmem(p + off, n);
		off += n;
		cond_resched();
	}
}

void ext4_fc_bytelog_persist_barrier(void)
{
	pmem_wmb();
}

static int ext4_fc_bytelog_map_ring(struct super_block *sb,
				    journal_t *journal,
				    struct ext4_fc_bytelog *log)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	unsigned long long first, anchor;
	unsigned long fc_blocks;
	unsigned long ring_blocks;
	u64 start_bytes, ring_bytes, start_offset;
	pgoff_t start_pgoff;
	unsigned long ring_pages;
	void *addr = NULL;
	int ret;
	int blkbits = sb->s_blocksize_bits;

	if (!journal->j_inode)
		return -EOPNOTSUPP;

	if (journal->j_fc_last <= journal->j_fc_first + 1)
		return -ENOSPC;

	fc_blocks = journal->j_fc_last - journal->j_fc_first;
	ring_blocks = fc_blocks - 1;
	if (ring_blocks <= EXT4_FC_BYTELOG_META_BLOCKS)
		return -ENOSPC;

	ret = jbd2_journal_bmap(journal, journal->j_fc_first, &first);
	if (ret)
		return ret;

	ret = jbd2_journal_bmap(journal, journal->j_fc_last - 1, &anchor);
	if (ret)
		return ret;

	start_bytes = (u64)first << blkbits;
	ring_bytes = (u64)ring_blocks << blkbits;
	if (!ring_bytes)
		return -ENOSPC;
	if (ring_bytes & (PAGE_SIZE - 1))
		return -EOPNOTSUPP;
	if (start_bytes > U64_MAX - sbi->s_dax_part_off)
		return -ERANGE;

	start_offset = start_bytes + sbi->s_dax_part_off;
	if (!IS_ALIGNED(start_offset, PAGE_SIZE))
		return -EINVAL;

	start_pgoff = start_offset >> PAGE_SHIFT;
	ring_pages = ring_bytes >> PAGE_SHIFT;
	if (!ring_pages || ring_pages > LONG_MAX)
		return -E2BIG;

#if IS_ENABLED(CONFIG_DAX)
	{
		long mapped;
		int dax_id = dax_read_lock();

		mapped = dax_direct_access(sbi->s_daxdev, start_pgoff,
					   ring_pages, DAX_ACCESS, &addr,
					   NULL);
		dax_read_unlock(dax_id);
		if (mapped < 0)
			return mapped;
		if (mapped < ring_pages)
			return -ENXIO;
	}
#else
	return -EOPNOTSUPP;
#endif

	log->kaddr = addr;
	log->size_bytes = ring_bytes;
	log->base_off = (u64)EXT4_FC_BYTELOG_META_BLOCKS << blkbits;
	log->persist_off = log->base_off;
	log->blocks = ring_blocks;
	log->blocksize = sb->s_blocksize;
	log->start_pblk = first;
	log->anchor_pblk = anchor;

	return 0;
}

int ext4_fc_bytelog_init(struct super_block *sb, journal_t *journal)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_fc_bytelog *log = &sbi->s_fc_bytelog;
	bool have_feature = ext4_has_feature_dax_fc_bytelog(sb);
	bool requested = test_opt2(sb, DAX_FC_BYTELOG);
	bool force = test_opt2(sb, DAX_FC_BYTELOG_FORCE);
	bool need_map = have_feature || requested || force;
	u32 batch_max;
	int ret;

	if (!need_map) {
		log->enabled = false;
		log->last_error = -EOPNOTSUPP;
		return 0;
	}

	ext4_fc_crc32c_shift_mats_init_once();

	if (log->mapped)
		goto enable;

	batch_max = log->batch_max;
	memset(log, 0, sizeof(*log));
	log->batch_max = batch_max ? batch_max :
		EXT4_FC_BYTELOG_BATCH_MAX_DEFAULT;
	log->last_error = -EOPNOTSUPP;

	if (!journal || !test_opt2(sb, JOURNAL_FAST_COMMIT)) {
		if (requested)
			ext4_msg(sb, KERN_INFO,
				 "dax_fc_bytelog requires fast commits enabled");
		return -EOPNOTSUPP;
	}

	/*
	 * ext4_fc_bytelog_init() is called once before jbd2_journal_load() so
	 * that existing ByteLog records can be replayed.  On a fresh
	 * filesystem, the JBD2 fast-commit feature may not be enabled on the
	 * journal yet, so there is no fast-commit area to map at this stage.
	 *
	 * If the on-disk feature bit is set, lack of journal fast-commit
	 * support indicates an inconsistent filesystem and must be fatal.
	 * Otherwise, defer mapping until the post-journal-load init path.
	 */
	if (!jbd2_has_feature_fast_commit(journal)) {
		if (have_feature) {
			ext4_msg(sb, KERN_ERR,
				 "dax_fc_bytelog requires JBD2 fast commits enabled");
			return -EINVAL;
		}

		log->enabled = false;
		log->last_error = -EOPNOTSUPP;
		return 0;
	}

	/*
	 * When dax_fc_bytelog=on is specified without the incompat feature
	 * bit, refuse to enable ByteLog.  dax_fc_bytelog=force overrides this
	 * check and is intended only for testing.
	 */
	if (!have_feature && requested && !force) {
		ext4_msg(sb, KERN_INFO,
			 "dax_fc_bytelog=on requires INCOMPAT_DAX_FC_BYTELOG");
		return -EOPNOTSUPP;
	}
	if (!have_feature && force)
		ext4_warning(sb,
			     "forcing dax_fc_bytelog without INCOMPAT_DAX_FC_BYTELOG; older kernels cannot safely mount this filesystem");

	if (test_opt2(sb, DAX_NEVER)) {
		ext4_msg(sb, KERN_INFO,
			 "dax_fc_bytelog requires DAX, but dax=never is set");
		return -EOPNOTSUPP;
	}
	if (!sbi->s_daxdev) {
		ext4_msg(sb, KERN_INFO,
			 "dax_fc_bytelog requires a dax-capable filesystem device");
		return -EOPNOTSUPP;
	}
	if (sb->s_blocksize != PAGE_SIZE) {
		ext4_msg(sb, KERN_INFO,
			 "dax_fc_bytelog requires blocksize == PAGE_SIZE");
		return -EOPNOTSUPP;
	}

	ret = ext4_fc_bytelog_map_ring(sb, journal, log);
	if (ret) {
		log->last_error = ret;
		ext4_msg(sb, KERN_INFO,
			 "dax_fc_bytelog disabled: unable to map fast-commit ring (err=%d)",
			 ret);
		ext4_debug("ByteLog mapping unavailable (err=%d)\n", ret);
		return ret;
	}

	log->head = log->base_off;
	log->tail = log->base_off;
	log->seq = 0;
	log->ring_crc = ~0;
	log->dirty = false;
	log->persist_off = log->base_off;
	ext4_fc_bytelog_reset_batch(log);
	log->mapped = true;
	log->last_error = 0;
enable:
	log->enabled = requested || force;
	return 0;
}

void ext4_fc_bytelog_release(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);

	memset(&sbi->s_fc_bytelog, 0, sizeof(sbi->s_fc_bytelog));
}

void ext4_fc_bytelog_reset(struct super_block *sb, bool full)
{
	struct ext4_fc_bytelog *log = &EXT4_SB(sb)->s_fc_bytelog;

	if (!log->mapped)
		return;
	if (!full)
		return;

	log->head = log->base_off;
	log->tail = log->base_off;
	log->seq = 0;
	log->ring_crc = ~0;
	log->dirty = false;
	log->persist_off = log->base_off;
	ext4_fc_bytelog_reset_batch(log);
}

void ext4_fc_bytelog_begin_commit(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_fc_bytelog *log = &sbi->s_fc_bytelog;

	if (!log->mapped || !log->enabled)
		return;

	log->head = log->base_off;
	log->tail = log->base_off;
	log->seq = 0;
	log->ring_crc = ~0;
	log->dirty = false;
	log->persist_off = log->base_off;
	ext4_fc_bytelog_reset_batch(log);
}

int ext4_fc_bytelog_end_commit(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_fc_bytelog *log = &sbi->s_fc_bytelog;
	journal_t *journal = sbi->s_journal;
	u8 *base;
	u64 cursor, end;
	u32 tid;
	int ret;

	if (!log->mapped || !log->enabled)
		return 0;

	if (!journal || !journal->j_running_transaction)
		return -EINVAL;
	tid = journal->j_running_transaction->t_tid;

	ret = ext4_fc_bytelog_flush_batch(sb, tid);
	if (ret) {
		log->last_error = ret;
		return ret;
	}

	if (!log->dirty)
		return 0;

	base = log->kaddr;
	if (!base)
		return -EOPNOTSUPP;

	cursor = log->persist_off;
	end = log->head;
	if (end <= cursor)
		return 0;

	ext4_fc_bytelog_flush_persist(base + cursor, end - cursor);
	ext4_fc_bytelog_persist_barrier();

	log->persist_off = end;
	log->dirty = false;
	return 0;
}

static inline bool ext4_fc_bytelog_has_space(struct ext4_fc_bytelog *log,
					     size_t len)
{
	if (log->head < log->base_off)
		return false;
	if (len > log->size_bytes - log->base_off)
		return false;
	return log->head + len <= log->size_bytes;
}

static void ext4_fc_bytelog_reset_batch(struct ext4_fc_bytelog *log)
{
	log->batch_first_tag = 0;
	log->batch_len = 0;
	log->batch_tlvs = 0;
	log->batch_payload_crc = ~0U;
}

static int ext4_fc_bytelog_commit_record(struct super_block *sb, u32 tid, u16 tag,
					 size_t payload_len, u32 payload_crc)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_fc_bytelog *log = &sbi->s_fc_bytelog;
	struct ext4_fc_bytelog_hdr hdr;
	size_t total_len, off;
	u32 ring_crc;
	u8 *dst;
	u8 *payload;
	u64 seq;
	bool mats_ready;

	total_len = ext4_fc_bytelog_record_size(payload_len);
	if (!ext4_fc_bytelog_has_space(log, total_len))
		return -ENOSPC;

	seq = log->seq;
	ring_crc = log->ring_crc;

	mats_ready = READ_ONCE(ext4_fc_crc32c_shift_mats_ready);
	ext4_fc_bytelog_prep_hdr(&hdr, tag, 0, tid, seq, payload_len);
	dst = (u8 *)log->kaddr + log->head;
	off = sizeof(hdr);
	payload = dst + off;

	if (payload_len) {
		if (likely(mats_ready)) {
			ring_crc = ext4_fc_crc32c_shift_zeros(ring_crc ^ ~0U, payload_len);
			ring_crc ^= payload_crc;
		} else {
			ring_crc = crc32c(ring_crc, payload, payload_len);
		}
		off += payload_len;
	} else {
		payload_crc = ext4_fc_bytelog_crc32(NULL, 0);
	}

	if (off < total_len) {
		size_t pad = total_len - off;

		memset(dst + off, 0, pad);
	}

	hdr.flags = cpu_to_le16(le16_to_cpu(hdr.flags) | EXT4_FC_BYTELOG_COMMITTED);
	ext4_fc_bytelog_finalize_hdr_crc(&hdr, payload_crc);
	memcpy(dst, &hdr, sizeof(hdr));

	log->head += total_len;
	log->seq++;
	log->dirty = true;
	log->ring_crc = ring_crc;

	return 0;
}

static size_t ext4_fc_bytelog_copy_vecs(u8 *dst,
					struct ext4_fc_bytelog_vec *vecs,
					int nvec, u32 *crc)
{
	size_t off = 0;
	u32 crc_val = crc ? *crc : 0;
	int i;

	for (i = 0; i < nvec; i++) {
		const u8 *src = vecs[i].base;
		size_t len = vecs[i].len;

		if (!len)
			continue;

		while (i + 1 < nvec && vecs[i + 1].len &&
		       vecs[i + 1].base == src + len) {
			len += vecs[i + 1].len;
			i++;
		}

		if (crc)
			crc_val = crc32c(crc_val, src, len);
		memcpy(dst + off, src, len);
		off += len;
	}

	if (crc)
		*crc = crc_val;
	return off;
}

static int ext4_fc_bytelog_append_vec_direct(struct super_block *sb, u32 tid, u16 tag,
					     struct ext4_fc_bytelog_vec *vecs,
					     int nvec, size_t payload_len)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_fc_bytelog *log = &sbi->s_fc_bytelog;
	size_t total_len;
	u32 payload_crc = ~0U;
	u8 *dst;

	total_len = ext4_fc_bytelog_record_size(payload_len);
	if (!ext4_fc_bytelog_has_space(log, total_len))
		return -ENOSPC;

	dst = (u8 *)log->kaddr + log->head + sizeof(struct ext4_fc_bytelog_hdr);
	ext4_fc_bytelog_copy_vecs(dst, vecs, nvec, &payload_crc);
	return ext4_fc_bytelog_commit_record(sb, tid, tag, payload_len,
					     payload_crc);
}

static int ext4_fc_bytelog_flush_batch(struct super_block *sb, u32 tid)
{
	struct ext4_fc_bytelog *log = &EXT4_SB(sb)->s_fc_bytelog;
	u32 payload_crc = ~0U;
	u16 tag;
	int ret;

	if (!log->batch_len)
		return 0;

	tag = log->batch_first_tag;
	if (log->batch_tlvs > 1)
		tag = EXT4_FC_BYTELOG_TAG_BATCH;

	if (!log->kaddr)
		return -EOPNOTSUPP;

	payload_crc = log->batch_payload_crc;
	ret = ext4_fc_bytelog_commit_record(sb, tid, tag, log->batch_len,
					    payload_crc);
	ext4_fc_bytelog_reset_batch(log);
	return ret;
}

int ext4_fc_bytelog_append_vec(struct super_block *sb, u16 tag,
			       struct ext4_fc_bytelog_vec *vecs, int nvec)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_fc_bytelog *log = &sbi->s_fc_bytelog;
	struct journal_s *journal = sbi->s_journal;
	size_t payload_len = 0;
	u32 batch_max = log->batch_max;
	u32 tid;
	int i;
	u8 *base;
	u8 *dst;

	if (!ext4_fc_bytelog_active(sbi))
		return -EOPNOTSUPP;

	if (!journal || !journal->j_running_transaction)
		return -EINVAL;
	tid = journal->j_running_transaction->t_tid;

	for (i = 0; i < nvec; i++)
		payload_len += vecs[i].len;

	base = log->kaddr;
	if (!base)
		return -EOPNOTSUPP;

	if (!batch_max) {
		int ret;

		ret = ext4_fc_bytelog_flush_batch(sb, tid);
		if (ret)
			return ret;
		return ext4_fc_bytelog_append_vec_direct(sb, tid, tag, vecs,
							 nvec, payload_len);
	}

	if (payload_len > batch_max) {
		int ret;

		ret = ext4_fc_bytelog_flush_batch(sb, tid);
		if (ret)
			return ret;
		return ext4_fc_bytelog_append_vec_direct(sb, tid, tag, vecs,
							 nvec, payload_len);
	}

	if (log->batch_len && log->batch_len + payload_len > batch_max) {
		int ret;

		ret = ext4_fc_bytelog_flush_batch(sb, tid);
		if (ret)
			return ret;
	}

	if (!log->batch_len)
		log->batch_first_tag = tag;

	if (!ext4_fc_bytelog_has_space(log,
				       ext4_fc_bytelog_record_size(log->batch_len +
								   payload_len))) {
		int ret;

		ret = ext4_fc_bytelog_flush_batch(sb, tid);
		if (ret)
			return ret;
		log->batch_first_tag = tag;
	}

	if (!ext4_fc_bytelog_has_space(log,
				       ext4_fc_bytelog_record_size(log->batch_len +
								   payload_len)))
		return -ENOSPC;

	dst = base + log->head + sizeof(struct ext4_fc_bytelog_hdr) +
	      log->batch_len;
	log->batch_len += ext4_fc_bytelog_copy_vecs(dst, vecs, nvec, &log->batch_payload_crc);
	log->batch_tlvs++;
	log->dirty = true;
	return 0;
}

void ext4_fc_bytelog_build_anchor(struct super_block *sb,
				  struct ext4_fc_bytelog_anchor *anchor,
				  u32 tid)
{
	struct ext4_fc_bytelog *log = &EXT4_SB(sb)->s_fc_bytelog;

	memset(anchor, 0, sizeof(*anchor));
	anchor->tid = tid;
	anchor->head = log->head;
	anchor->tail = log->tail;
	anchor->seq = log->seq;
	anchor->crc = log->ring_crc;
}
