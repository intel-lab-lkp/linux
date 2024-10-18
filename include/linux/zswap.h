/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ZSWAP_H
#define _LINUX_ZSWAP_H

#include <linux/types.h>
#include <linux/mm_types.h>

struct lruvec;

extern atomic_long_t zswap_stored_pages;

#ifdef CONFIG_ZSWAP

struct zswap_lruvec_state {
	/*
	 * Number of swapped in pages from disk, i.e not found in the zswap pool.
	 *
	 * This is consumed and subtracted from the lru size in
	 * zswap_shrinker_count() to penalize past overshrinking that led to disk
	 * swapins. The idea is that had we considered this many more pages in the
	 * LRU active/protected and not written them back, we would not have had to
	 * swapped them in.
	 */
	atomic_long_t nr_disk_swapins;
};

/*
 * struct zswap_store_sub_batch_page:
 *
 * This represents one "zswap batching element", namely, the
 * attributes associated with a page in a large folio that will
 * be compressed and stored in zswap. The term "batch" is reserved
 * for a conceptual "batch" of folios that can be sent to
 * zswap_store() by reclaim. The term "sub-batch" is used to describe
 * a collection of "zswap batching elements", i.e., an array of
 * "struct zswap_store_sub_batch_page *".
 *
 * The zswap compress sub-batch size is specified by
 * SWAP_CRYPTO_SUB_BATCH_SIZE, currently set as 8UL if the
 * platform has Intel IAA. This means zswap can store a large folio
 * by creating sub-batches of up to 8 pages and compressing this
 * batch using IAA to parallelize the 8 compress jobs in hardware.
 * For e.g., a 64KB folio can be compressed as 2 sub-batches of
 * 8 pages each. This can significantly improve the zswap_store()
 * performance for large folios.
 *
 * Although the page itself is represented directly, the structure
 * adds a "u8 batch_idx" to represent an index for the folio in a
 * conceptual "batch of folios" that can be passed to zswap_store().
 * Conceptually, this allows for up to 256 folios that can be passed
 * to zswap_store(). If this conceptual number of folios sent to
 * zswap_store() exceeds 256, the "batch_idx" needs to become u16.
 */
struct zswap_store_sub_batch_page {
	u8 batch_idx;
	swp_entry_t swpentry;
	struct obj_cgroup *objcg;
	struct zswap_entry *entry;
	int error; /* folio error status. */
};

/*
 * struct zswap_store_pipeline_state:
 *
 * This stores state during IAA compress batching of (conceptually, a batch of)
 * folios. The term pipelining in this context, refers to breaking down
 * the batch of folios being reclaimed into sub-batches of
 * SWAP_CRYPTO_SUB_BATCH_SIZE pages, batch compressing and storing the
 * sub-batch. This concept could be further evolved to use overlap of CPU
 * computes with IAA computes. For instance, we could stage the post-compress
 * computes for sub-batch "N-1" to happen in parallel with IAA batch
 * compression of sub-batch "N".
 *
 * We begin by developing the concept of compress batching. Pipelining with
 * overlap can be future work.
 *
 * @errors: The errors status for the batch of reclaim folios passed in from
 *          a higher mm layer such as swap_writepage().
 * @pool: A valid zswap_pool.
 * @acomp_ctx: The per-cpu pointer to the crypto_acomp_ctx for the @pool.
 * @sub_batch: This is an array that represents the sub-batch of up to
 *             SWAP_CRYPTO_SUB_BATCH_SIZE pages that are being stored
 *             in zswap.
 * @comp_dsts: The destination buffers for crypto_acomp_compress() for each
 *             page being compressed.
 * @comp_dlens: The destination buffers' lengths from crypto_acomp_compress()
 *              obtained after crypto_acomp_poll() returns completion status,
 *              for each page being compressed.
 * @comp_errors: Compression errors for each page being compressed.
 * @nr_comp_pages: Total number of pages in @sub_batch.
 *
 * Note:
 * The max sub-batch size is SWAP_CRYPTO_SUB_BATCH_SIZE, currently 8UL.
 * Hence, if SWAP_CRYPTO_SUB_BATCH_SIZE exceeds 256, some of the
 * u8 members (except @comp_dsts) need to become u16.
 */
struct zswap_store_pipeline_state {
	int *errors;
	struct zswap_pool *pool;
	struct crypto_acomp_ctx *acomp_ctx;
	struct zswap_store_sub_batch_page *sub_batch;
	struct page **comp_pages;
	u8 **comp_dsts;
	unsigned int *comp_dlens;
	int *comp_errors;
	u8 nr_comp_pages;
};

bool zswap_store_batching_enabled(void);
unsigned long zswap_total_pages(void);
bool zswap_store(struct folio *folio);
bool zswap_load(struct folio *folio);
void zswap_invalidate(swp_entry_t swp);
int zswap_swapon(int type, unsigned long nr_pages);
void zswap_swapoff(int type);
void zswap_memcg_offline_cleanup(struct mem_cgroup *memcg);
void zswap_lruvec_state_init(struct lruvec *lruvec);
void zswap_folio_swapin(struct folio *folio);
bool zswap_is_enabled(void);
bool zswap_never_enabled(void);
#else

struct zswap_lruvec_state {};
struct zswap_store_sub_batch_page {};
struct zswap_store_pipeline_state {};

static inline bool zswap_store_batching_enabled(void)
{
	return false;
}

static inline bool zswap_store(struct folio *folio)
{
	return false;
}

static inline bool zswap_load(struct folio *folio)
{
	return false;
}

static inline void zswap_invalidate(swp_entry_t swp) {}
static inline int zswap_swapon(int type, unsigned long nr_pages)
{
	return 0;
}
static inline void zswap_swapoff(int type) {}
static inline void zswap_memcg_offline_cleanup(struct mem_cgroup *memcg) {}
static inline void zswap_lruvec_state_init(struct lruvec *lruvec) {}
static inline void zswap_folio_swapin(struct folio *folio) {}

static inline bool zswap_is_enabled(void)
{
	return false;
}

static inline bool zswap_never_enabled(void)
{
	return true;
}

#endif

#endif /* _LINUX_ZSWAP_H */
