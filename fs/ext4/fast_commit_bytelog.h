/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _EXT4_FAST_COMMIT_BYTELOG_H
#define _EXT4_FAST_COMMIT_BYTELOG_H

#include <linux/bitops.h>
#include <linux/byteorder/generic.h>
#include <linux/types.h>

struct super_block;
struct journal_s;
struct ext4_sb_info;

#define EXT4_FC_BYTELOG_MAGIC			0x4c424346 /* "FCBL" */
#define EXT4_FC_BYTELOG_VERSION			1
#define EXT4_FC_BYTELOG_ALIGN			64
#define EXT4_FC_BYTELOG_BATCH_MAX_DEFAULT	4096

/*
 * Record header @tag for a batched TLV payload stream.
 *
 * In this case the payload is a stream of standard fast-commit TLVs
 * (struct ext4_fc_tl + value).
 */
#define EXT4_FC_BYTELOG_TAG_BATCH		0xffff

/* Record flag bits */
#define EXT4_FC_BYTELOG_COMMITTED		BIT(0)

/**
 * struct ext4_fc_bytelog_hdr - On-media header for a ByteLog record
 * @magic:	Magic identifying the record
 * @version:	On-disk header format version
 * @hdr_len:	Length of this header in bytes
 * @tid:	JBD2 transaction identifier
 * @tag:	Ext4 fast-commit tag (or EXT4_FC_BYTELOG_TAG_BATCH)
 * @flags:	Record flags (EXT4_FC_BYTELOG_*)
 * @payload_len:Length of payload bytes following the header
 * @payload_crc:CRC32C of the payload
 * @record_len:	Entire record length including header, payload and padding
 * @header_crc:	CRC32C of the header with @header_crc zeroed
 * @seq:	Monotonic sequence number assigned by the ByteLog writer
 * @reserved:	Future fields, currently zeroed
 *
 * The structure is padded to 64 bytes to keep each record 64B aligned.
 */
struct ext4_fc_bytelog_hdr {
	__le32	magic;
	__le16	version;
	__le16	hdr_len;
	__le32	tid;
	__le16	tag;
	__le16	flags;
	__le32	payload_len;
	__le32	payload_crc;
	__le32	record_len;
	__le32	header_crc;
	__le64	seq;
	__le64	reserved[3];
} __packed;

struct ext4_fc_bytelog_anchor {
	u32		tid;
	u64		head;
	u64		tail;
	u64		seq;
	u32		crc;
};

struct ext4_fc_bytelog {
	void		*kaddr;
	u64		size_bytes;
	u64		base_off;
	u64		persist_off;
	u32		blocksize;
	u32		blocks;
	u64		start_pblk;
	u64		anchor_pblk;
	u64		head;
	u64		tail;
	u64		seq;
	u32		ring_crc;

	u32		batch_max;
	u16		batch_first_tag;
	u32		batch_len;
	u32		batch_tlvs;
	u32		batch_payload_crc;

	bool		mapped;
	bool		enabled;
	bool		dirty;
	int		last_error;
};

struct ext4_fc_bytelog_vec {
	const void	*base;
	size_t		len;
};

int ext4_fc_bytelog_init(struct super_block *sb, struct journal_s *journal);
void ext4_fc_bytelog_release(struct super_block *sb);
void ext4_fc_bytelog_reset(struct super_block *sb, bool full);
void ext4_fc_bytelog_begin_commit(struct super_block *sb);
int ext4_fc_bytelog_end_commit(struct super_block *sb);
bool ext4_fc_bytelog_active(struct ext4_sb_info *sbi);
bool ext4_fc_bytelog_mapped(struct ext4_sb_info *sbi);
int ext4_fc_bytelog_append_vec(struct super_block *sb, u16 tag,
			       struct ext4_fc_bytelog_vec *vecs, int nvec);
void ext4_fc_bytelog_build_anchor(struct super_block *sb,
				  struct ext4_fc_bytelog_anchor *anchor,
				  u32 tid);

static inline bool ext4_fc_bytelog_record_committed(const struct ext4_fc_bytelog_hdr *hdr)
{
	return !!(le16_to_cpu(hdr->flags) & EXT4_FC_BYTELOG_COMMITTED);
}

static inline u32 ext4_fc_bytelog_record_len(const struct ext4_fc_bytelog_hdr *hdr)
{
	return le32_to_cpu(hdr->record_len);
}

static inline u32 ext4_fc_bytelog_payload_len(const struct ext4_fc_bytelog_hdr *hdr)
{
	return le32_to_cpu(hdr->payload_len);
}

static inline u64 ext4_fc_bytelog_seq(const struct ext4_fc_bytelog_hdr *hdr)
{
	return le64_to_cpu(hdr->seq);
}

size_t ext4_fc_bytelog_record_size(size_t payload_len);
void ext4_fc_bytelog_prep_hdr(struct ext4_fc_bytelog_hdr *hdr, u16 tag,
			      u16 flags, u32 tid, u64 seq, u32 payload_len);
void ext4_fc_bytelog_finalize_hdr_crc(struct ext4_fc_bytelog_hdr *hdr,
				      u32 payload_crc);
int ext4_fc_bytelog_validate_hdr(const struct ext4_fc_bytelog_hdr *hdr,
				 size_t remaining, const void *payload);
void ext4_fc_bytelog_mark_committed(struct ext4_fc_bytelog_hdr *hdr);

void ext4_fc_bytelog_flush_persist(void *addr, size_t len);
void ext4_fc_bytelog_persist_barrier(void);

u32 ext4_fc_bytelog_crc32(const void *buf, size_t len);

#endif /* _EXT4_FAST_COMMIT_BYTELOG_H */
