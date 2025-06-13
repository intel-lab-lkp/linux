/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _IOMAP_INTERNAL_H
#define _IOMAP_INTERNAL_H 1

#define IOEND_BATCH_SIZE	4096

/*
 * Structure allocated for each folio to track per-block uptodate, dirty state
 * and I/O completions.
 */
struct iomap_folio_state {
	spinlock_t		state_lock;
	unsigned int		read_bytes_pending;
	atomic_t		write_bytes_pending;

	/*
	 * Each block has two bits in this bitmap:
	 * Bits [0..blocks_per_folio) has the uptodate status.
	 * Bits [b_p_f...(2*b_p_f))   has the dirty status.
	 */
	unsigned long		state[];
};

struct iomap_readpage_ctx {
	struct folio		*cur_folio;
	bool			cur_folio_in_bio;
	struct bio		*bio;
	struct readahead_control *rac;
};

u32 iomap_finish_ioend_buffered(struct iomap_ioend *ioend);
u32 iomap_finish_ioend_direct(struct iomap_ioend *ioend);
bool ifs_set_range_uptodate(struct folio *folio, struct iomap_folio_state *ifs,
		size_t off, size_t len);
int iomap_submit_ioend(struct iomap_writepage_ctx *wpc, int error);

#ifdef CONFIG_BLOCK
int iomap_bio_read_folio_sync(loff_t block_start, struct folio *folio,
		size_t poff, size_t plen, const struct iomap *iomap);
int iomap_bio_add_to_ioend(struct iomap_writepage_ctx *wpc,
		struct writeback_control *wbc, struct folio *folio,
		struct inode *inode, loff_t pos, loff_t end_pos, unsigned len);
void iomap_bio_readpage(const struct iomap *iomap, loff_t pos,
		struct iomap_readpage_ctx *ctx, size_t poff, size_t plen,
		loff_t length);
void iomap_bio_ioend_error(struct iomap_writepage_ctx *wpc, int error);
void iomap_submit_bio(struct bio *bio);
#else
#define iomap_bio_read_folio_sync(...)		(-ENOSYS)
#define iomap_bio_add_to_ioend(...)		(-ENOSYS)
#define iomap_bio_readpage(...)		((void)0)
#define iomap_bio_ioend_error(...)		((void)0)
#define iomap_submit_bio(...)			((void)0)
#endif /* CONFIG_BLOCK */

#endif /* _IOMAP_INTERNAL_H */
