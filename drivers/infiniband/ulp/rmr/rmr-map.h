/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Reliable multicast over RTRS (RMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#ifndef RMR_MAP_H
#define RMR_MAP_H

#include <linux/types.h>
#include <linux/xarray.h>

#include "rmr.h"

/**
 * The dirty map buffer is used to track dirty chunks through bits.
 * The position of the bit denotes the chunk number it tracks.
 *
 * Bitmap structure
 * ----------------
 * The dirty bitmap is stored in a 2 level tree-like structure.
 * The main unit of storage are memory pages; They act as nodes of this structure.
 * The first level pages (FLP) stores the address of the second level pages.
 * There can be a total of 256 first level pages.
 * The second level pages (SLP, also the leaf nodes/pages) stores the bitmap.
 *
 * The first level pages have to store the address of the second level pages.
 * An address being 8B (default/max) long, the addresses of a maximum of 512 pages can
 * be stored in a first level page. This then decides the maximum leaf pages a pool can
 * have, which, for our example, is [(# pages of FLP) * (PAGE_SIZE / address_size)],
 * (256*512)=131072.
 * With the above info, the available space for bitmap is 131072*4KB(PAGE_SIZE)=512MB.
 *
 * A chunk is the smallest unit of data which is tracked for being dirty. A chunk is
 * called dirty/unsynced, even if a single byte in it is dirty/unsynced.
 * To track a chunk, a single byte (1B) is used. The least significant bit is used to signify
 * if the chunk is dirty (set) or not. Other bits can be used for other purposes (for example,
 * filters). The maximum number of chunks RMR can manage are then, (512MB)/1B=536870912.
 * This number is fixed, as one can see from the calculations, and hence the maximum size of
 * metadata RMR can allocate and use is fixed.

 * The user configurable part is the chunk size. Its range is 128KB-1MB, and it has to be a
 * power of 2.
 * The chunk size decides the maximum mapped size for an RMR pool.
 * For example, for chunk size 1MB, and taking the maximum number of chunks RMR can allocate
 * and handle (536870912, see above), the maximum mapped size would be (536870912*1MB)=512TB.
 * The table showing the relation between chunk size and maximum mapped size is as follows,
 * Chunk size	Maximum mapped size
 * 128KB	64TB
 * 256KB	128TB
 * 512KB	256TB
 * 1MB		512TB
 *
 * Calculating chunk number
 * ------------------------
 * Some key points
 * 1) The Linux kernel has a fixed size for sector, which is 512 (or 9 bitshift)
 * 2) The mapped_size provided and stores in the rmr_pool structure is in sectors.
 * 3) The chunk_size provided and stored in the rmr_pool structure is in bytes.
 * 4) The code calculates and stores chunk_size_shift in the rmr_pool structure to do fast
 *    calculation.
 * 5) The IO offset give to RMR (through function rmr_clt_request) is in bytes.
 *
 * --
 * With the above points, lets have a sample scenario with mapped_size 1GB and chunk_size 128KB
 * The numbers would then be,
 *
 * no_of_chunks = (mapped_size / chunk_size)
 * no_of_chunks = 8192
 *
 * chunk_size = 131072
 * chunk_size_shift = 17
 *
 * dirty_map buffer size (in BYTES) = (no_of_chunks / bits in a byte)
 * dirty_map buffer size (in BYTES) = 1024
 *
 * --
 * Lets do a sample calculation of chunk_no from offset and length of an IO
 *
 * For offset 30801920 and length 4096
 *
 * chunk_no = (offset >> chunk_size_shift)
 * chunk_no = 235
 *
 */

#define RMR_KEY_SHIFT 32

// Each chunk requires 1B of metadata
#define PER_CHUNK_MD		1
#define PER_CHUNK_MD_LOG2	ilog2(PER_CHUNK_MD)

#define GET_CHUNK_NUMBER(offset, shift)			(offset >> shift)
#define GET_FOLLOWING_CHUNKS(offset_len, shift, start)	(((offset_len - 1) >> shift) - start + 1)

#define CHUNK_TO_OFFSET(chunk_no, shift)       (chunk_no << shift)

// The element type stored in FLP
typedef unsigned long	el_flp;

enum {
	CHUNK_DIRTY_BIT = 0,
	CHUNK_FILTER_BIT,
};

enum {
	MAX_NO_OF_FLP = 256,
	NO_OF_SLP_PER_FLP = (PAGE_SIZE >> ilog2(sizeof(void *))),
	NO_OF_SLP_PER_FLP_LOG2 = ilog2(NO_OF_SLP_PER_FLP),
	MAX_NO_OF_SLP = (MAX_NO_OF_FLP * NO_OF_SLP_PER_FLP),

	NO_OF_CHUNKS_PER_PAGE = (PAGE_SIZE >> PER_CHUNK_MD_LOG2),
	// Chunks data is stored only in SLP
	MAX_NO_OF_CHUNKS = (MAX_NO_OF_SLP * NO_OF_CHUNKS_PER_PAGE),

	CHUNKS_PER_SLP = (PAGE_SIZE >> PER_CHUNK_MD_LOG2),
	CHUNKS_PER_SLP_LOG2 = ilog2(CHUNKS_PER_SLP),
	CHUNKS_PER_FLP = (CHUNKS_PER_SLP * NO_OF_SLP_PER_FLP),
	CHUNKS_PER_FLP_LOG2 = ilog2(CHUNKS_PER_FLP),
};

typedef enum {
	MAP_NO_FILTER = 0,
	MAP_ENTRY_UNSYNCED
} rmr_map_filter;

enum rmr_map_state {
	RMR_MAP_STATE_NO_CHECK = 0,
	RMR_MAP_STATE_CHECKING,
	// do we have some other useful states ?
};

struct rmr_dirty_id_map {
	u8 member_id;
	struct xarray rmr_id_map;
	unsigned long ts;
	atomic_t check_state;

	/*
	 * The usage of this is restricted to form a linked lised
	 * during mass deletion. Since this is in an RCU list (maps
	 * in rmr_pool), we cannot use this or change any data until
	 * the RCU period completes. So we use this next variable
	 * during mass deletion so we can have a list and don't have
	 * to wait and restart the search on every individual deletion
	 * of a map. Refer destroy_clt_pool().
	 */
	struct rmr_dirty_id_map *next;

	u64		no_of_chunks;
	u64		no_of_flp;
	u64		no_of_slp_in_last_flp;
	u64		no_of_chunk_in_last_slp;
	u64		total_slp;
	u8		*bitmap_filter;
	void		*dirty_bitmap[MAX_NO_OF_FLP];
};

struct rmr_map_entry {
	atomic_t sync_cnt;
	struct llist_head wait_list;
};

/*
 * The header of the bitmap buffer.
 */
struct rmr_map_cbuf_hdr {
	u64		version;
	u8		member_id;

	u64		no_of_chunks;
	u64		no_of_flp;
	u64		no_of_slp_in_last_flp;
	u64		no_of_chunk_in_last_slp;
	u64		total_slp;
} __packed;

static inline unsigned long rmr_id_to_key(rmr_id_t id)
{
	unsigned long res;

	// highest bits for id.a, the rest are for id.b;
	res = ((id.a << RMR_KEY_SHIFT) | id.b);
	return res;
}

static inline u64 key_to_a(unsigned long key)
{
	return key >> RMR_KEY_SHIFT;
}

static inline u64 key_to_b(unsigned long key)
{
	return key & ((1ULL << RMR_KEY_SHIFT) - 1);
}

void rmr_map_update_page_params(struct rmr_dirty_id_map *map);
struct rmr_dirty_id_map *rmr_map_create(struct rmr_pool *pool, u8 member_id);
void rmr_map_destroy(struct rmr_dirty_id_map *map);
void rmr_map_calc_chunk(struct rmr_pool *pool, size_t offset, size_t length, rmr_id_t *id);
void rmr_map_set_dirty(struct rmr_dirty_id_map *map, rmr_id_t id, u8 filter);
void rmr_map_set_dirty_all(struct rmr_dirty_id_map *map, u8 filter);
struct rmr_map_entry *rmr_map_unset_dirty(struct rmr_dirty_id_map *map, rmr_id_t id, u8 filter);
bool rmr_map_check_dirty(struct rmr_dirty_id_map *map, rmr_id_t id);
struct rmr_map_entry *rmr_map_get_dirty_entry(struct rmr_dirty_id_map *map, rmr_id_t id);
void rmr_map_clear_filter_all(struct rmr_dirty_id_map *map, u8 filter);
void rmr_map_unset_dirty_all(struct rmr_dirty_id_map *map);
bool rmr_map_empty(struct rmr_dirty_id_map *map);

void rmr_map_bitwise_or_buf(void *dst_buf, void *src_buf, u32 buf_size);
int rmr_map_create_entries(struct rmr_dirty_id_map *map);

void rmr_map_hexdump_bitmap_buf(u8 member_id, void *buf, u32 buf_size);
void rmr_map_slps_to_buf(struct rmr_dirty_id_map *map, u64 slp_idx, u64 no_of_slp, u8 *buf);
u64 rmr_map_buf_to_slps(struct rmr_dirty_id_map *map, u8 *buf, u32 buf_size, u64 slp_idx,
			bool test);
void rmr_map_dump_bitmap(struct rmr_dirty_id_map *map);
int rmr_map_summary_format(struct rmr_pool *pool, char *buf, size_t buf_size);
void rmr_map_bidump_bitmap_buf(void *buf, u8 member_id, u32 buf_size);

static inline void map_entry_get_sync(struct rmr_map_entry *entry)
{
	atomic_inc(&entry->sync_cnt);
	pr_debug("after get ref for entry %p, sync cnt %d\n",
		 entry, atomic_read(&entry->sync_cnt));
}

static inline int map_entry_put_sync(struct rmr_map_entry *entry)
{
	pr_debug("before dec_and_test for entry %p, sync cnt %d\n",
		 entry, atomic_read(&entry->sync_cnt));
	return atomic_dec_and_test(&entry->sync_cnt);
}

static inline void rmr_maplist_destroy(struct rmr_dirty_id_map *maplist)
{
	struct rmr_dirty_id_map *mp;

	while (maplist != NULL) {
		mp = maplist;
		maplist = maplist->next;
		rmr_map_destroy(mp);
	}
}
#endif /* RMR_MAP_H */
