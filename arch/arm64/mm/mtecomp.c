// SPDX-License-Identifier: GPL-2.0-only

/*
 * MTE tag compression algorithm.
 * See Documentation/arch/arm64/mte-tag-compression.rst for more details.
 */

#include <linux/bits.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/mtecomp.h>

#include "mtecomp.h"

/* The handle must fit into an Xarray value. */
#define MTE_HANDLE_MASK GENMASK_ULL(62, 0)

/* Out-of-line handles have 0b111 in bits 62..60. */
#define MTE_NOINLINE_MASK GENMASK_ULL(62, 60)

/* Cache index is stored in the lowest pointer bits. */
#define MTE_CACHE_ID_MASK GENMASK_ULL(2, 0)

/* Caches start at mte-tags-16 and go up to mte-tags-MTE_PAGE_TAG_STORAGE. */
#define MTECOMP_NUM_CACHES ilog2(MTE_PAGE_TAG_STORAGE / 8)
static struct kmem_cache *mtecomp_caches[MTECOMP_NUM_CACHES];
/*
 * [0] - store the numbers of created/released inline handles;
 * [1..MTECOMP_NUM_CACHES] - store the number of allocations/deallocations from
 *                           mtecomp_caches.
 */
static atomic_long_t alloc_counters[MTECOMP_NUM_CACHES + 1];
static atomic_long_t dealloc_counters[MTECOMP_NUM_CACHES + 1];

/*
 * Largest number of ranges, for which compressed data fits into 63 bits, can
 * be encoded with 4 bits.
 */
#define MTE_BITS_PER_LARGEST_IDX_INLINE 4
/*
 * In the worst case every tag is different, then largest index can be up to
 * MTE_GRANULES_PER_PAGE.
 */
#define MTE_BITS_PER_LARGEST_IDX ilog2(MTE_GRANULES_PER_PAGE)
/* Range size cannot exceed MTE_GRANULES_PER_PAGE / 2. */
#define MTE_BITS_PER_SIZE (MTE_BITS_PER_LARGEST_IDX - 1)

/* Translate allocation size into mtecomp_caches[] index. */
static unsigned int mte_size_to_cache_id(size_t len)
{
	return fls(len) - 5;
}

/* Translate mtecomp_caches[] index into allocation size. */
static size_t mte_cache_id_to_size(unsigned int id)
{
	return 16 << id;
}

/**
 * mte_tags_to_ranges() - break @tags into arrays of tag ranges.
 * @tags: MTE_GRANULES_PER_PAGE-byte array containing MTE tags.
 * @out_tags: u8 array to store the tag of every range.
 * @out_sizes: unsigned short array to store the size of every range.
 * @out_len: length of @out_tags and @out_sizes (output parameter, initially
 *           equal to lengths of out_tags[] and out_sizes[]).
 */
void mte_tags_to_ranges(u8 *tags, u8 *out_tags, unsigned short *out_sizes,
			size_t *out_len)
{
	u8 prev_tag = tags[0] / 16; /* First tag in the array. */
	unsigned int cur_idx = 0, i, j;
	u8 cur_tag;

	memset(out_tags, 0, array_size(*out_len, sizeof(*out_tags)));
	memset(out_sizes, 0, array_size(*out_len, sizeof(*out_sizes)));

	out_tags[cur_idx] = prev_tag;
	for (i = 0; i < MTE_GRANULES_PER_PAGE; i++) {
		j = i % 2;
		cur_tag = j ? (tags[i / 2] % 16) : (tags[i / 2] / 16);
		if (cur_tag == prev_tag) {
			out_sizes[cur_idx]++;
		} else {
			cur_idx++;
			prev_tag = cur_tag;
			out_tags[cur_idx] = prev_tag;
			out_sizes[cur_idx] = 1;
		}
	}
	*out_len = cur_idx + 1;
}
EXPORT_SYMBOL_NS(mte_tags_to_ranges, MTECOMP);

/**
 * mte_ranges_to_tags() - fill @tags using given tag ranges.
 * @r_tags: u8[] containing the tag of every range.
 * @r_sizes: unsigned short[] containing the size of every range.
 * @r_len: length of @r_tags and @r_sizes.
 * @tags: MTE_GRANULES_PER_PAGE-byte array to write the tags to.
 */
void mte_ranges_to_tags(u8 *r_tags, unsigned short *r_sizes, size_t r_len,
			u8 *tags)
{
	unsigned int i, j, pos = 0;
	u8 prev;

	for (i = 0; i < r_len; i++) {
		for (j = 0; j < r_sizes[i]; j++) {
			if (pos % 2)
				tags[pos / 2] = (prev << 4) | r_tags[i];
			else
				prev = r_tags[i];
			pos++;
		}
	}
}
EXPORT_SYMBOL_NS(mte_ranges_to_tags, MTECOMP);

/*
 * Translate allocation size into maximum number of ranges that it can hold.
 *
 * It is the biggest number N such as:
 *   MTE_BITS_PER_LARGEST_IDX_INLINE + MTE_TAG_SIZE * N
 *                                   + MTE_BITS_PER_SIZE * (N-1) <= 63 bits,
 * for the inline case, or
 *   MTE_BITS_PER_LARGEST_IDX
 *     + MTE_TAG_SIZE * N + MTE_BITS_PER_SIZE * (N-1) <= tag array size in bits,
 * for the out-of line case.
 */
static size_t mte_size_to_ranges(size_t size)
{
	size_t largest_bits;

	largest_bits = (size == 8) ? MTE_BITS_PER_LARGEST_IDX_INLINE :
				     MTE_BITS_PER_LARGEST_IDX;
	return (size * 8 + MTE_BITS_PER_SIZE - largest_bits) /
	       (MTE_TAG_SIZE + MTE_BITS_PER_SIZE);
}

/* Translate @num_ranges into the allocation size needed to hold them. */
static size_t mte_alloc_size(unsigned int num_ranges)
{
	size_t size = 8;

	while (size < (1 << MTE_BITS_PER_SIZE)) {
		if (num_ranges <= mte_size_to_ranges(size))
			return size;
		size <<= 1;
	}
	return size;
}

/* Is the data stored inline in the handle itself? */
static bool mte_is_inline(unsigned long handle)
{
	return (handle & MTE_NOINLINE_MASK) != MTE_NOINLINE_MASK;
}

/**
 * mte_storage_size() - calculate the memory occupied by compressed tags.
 * @handle: storage handle returned by mte_compress.
 *
 * Returns: size of the storage used for @handle.
 */
size_t mte_storage_size(unsigned long handle)
{
	if (mte_is_inline(handle))
		return 8;
	return mte_cache_id_to_size(handle & MTE_CACHE_ID_MASK);
}
EXPORT_SYMBOL_NS(mte_storage_size, MTECOMP);

static void mte_bitmap_write(unsigned long *bitmap, unsigned long value,
			     unsigned long *pos, unsigned long bits)
{
	bitmap_write(bitmap, value, *pos, bits);
	*pos += bits;
}

static inline unsigned long mte_largest_idx_bits(size_t size)
{
	if (size == 8)
		return MTE_BITS_PER_LARGEST_IDX_INLINE;
	return MTE_BITS_PER_LARGEST_IDX;
}

/* Compress ranges into the buffer that can accommodate up to max_ranges. */
static void mte_compress_to_buf(size_t len, u8 *tags, unsigned short *sizes,
				unsigned long *bitmap, size_t size)
{
	unsigned long bit_pos = 0, l_bits;
	unsigned int largest_idx, i;
	unsigned short largest = 0;
	size_t max_ranges;

	for (i = 0; i < len; i++) {
		if (sizes[i] > largest) {
			largest = sizes[i];
			largest_idx = i;
		}
	}
	l_bits = mte_largest_idx_bits(size);
	max_ranges = mte_size_to_ranges(size);
	mte_bitmap_write(bitmap, largest_idx, &bit_pos, l_bits);
	for (i = 0; i < len; i++)
		mte_bitmap_write(bitmap, tags[i], &bit_pos, MTE_TAG_SIZE);
	for (i = len; i < max_ranges; i++)
		mte_bitmap_write(bitmap, 0, &bit_pos, MTE_TAG_SIZE);
	for (i = 0; i < len; i++) {
		if (i != largest_idx)
			mte_bitmap_write(bitmap, sizes[i], &bit_pos,
					 MTE_BITS_PER_SIZE);
	}
	for (i = len; i < max_ranges; i++)
		mte_bitmap_write(bitmap, 0, &bit_pos, MTE_BITS_PER_SIZE);
}

/**
 * mte_compress() - compress the given tag array.
 * @tags: MTE_GRANULES_PER_PAGE-byte array to read the tags from.
 *
 * Compresses the tags and returns a 64-bit opaque handle pointing to the
 * tag storage. May allocate memory, which is freed by @mte_release_handle().
 *
 * Returns: 64-bit tag storage handle.
 */
unsigned long mte_compress(u8 *tags)
{
	struct kmem_cache *cache;
	unsigned short *r_sizes;
	unsigned long *storage;
	unsigned int cache_id;
	size_t alloc_size;
	u8 *r_tags;
	size_t r_len;
	/*
	 * mte_compress_to_buf() only initializes the bits that mte_decompress()
	 * will read. But when the tags are stored in the handle itself, it must
	 * have all its bits initialized.
	 */
	unsigned long result = 0;

	r_sizes = kmalloc_array(MTE_GRANULES_PER_PAGE, sizeof(unsigned short),
				GFP_KERNEL);
	r_tags = kmalloc(MTE_GRANULES_PER_PAGE, GFP_KERNEL);
	if (!r_sizes || !r_tags)
		goto ret;
	r_len = MTE_GRANULES_PER_PAGE;
	mte_tags_to_ranges(tags, r_tags, r_sizes, &r_len);
	alloc_size = mte_alloc_size(r_len);
	if (alloc_size == 8) {
		mte_compress_to_buf(r_len, r_tags, r_sizes, &result,
				    alloc_size);
		atomic_long_inc(&alloc_counters[0]);
		goto ret;
	}
	cache_id = mte_size_to_cache_id(alloc_size);
	cache = mtecomp_caches[cache_id];
	storage = kmem_cache_alloc(cache, GFP_KERNEL);
	atomic_long_inc(&alloc_counters[cache_id + 1]);
	if (!storage) {
		result = 0;
		goto ret;
	}
	if (alloc_size < MTE_PAGE_TAG_STORAGE) {
		/* alloc_size is always a multiple of sizeof(unsigned long). */
		mte_compress_to_buf(r_len, r_tags, r_sizes, storage,
				    alloc_size);
		result = ((unsigned long)storage | cache_id) & MTE_HANDLE_MASK;
		goto ret;
	}
	memcpy(storage, tags, alloc_size);
	result = ((unsigned long)storage | cache_id) & MTE_HANDLE_MASK;
ret:
	kfree(r_tags);
	kfree(r_sizes);
	return result;
}
EXPORT_SYMBOL_NS(mte_compress, MTECOMP);

static unsigned long mte_bitmap_read(const unsigned long *bitmap,
				     unsigned long *pos, unsigned long bits)
{
	unsigned long start = *pos;

	*pos += bits;
	return bitmap_read(bitmap, start, bits);
}

/* Decompress the contents of the given buffer into @tags. */
static bool mte_decompress_from_buf(const unsigned long *bitmap, size_t size,
				    u8 *tags)
{
	unsigned long bit_pos = 0, l_bits;
	unsigned short *r_sizes, sum;
	unsigned int largest_idx, i;
	bool result = true;
	size_t max_ranges;
	u8 *r_tags;

	max_ranges = mte_size_to_ranges(size);
	l_bits = mte_largest_idx_bits(size);
	largest_idx = mte_bitmap_read(bitmap, &bit_pos, l_bits);
	r_sizes = kmalloc_array(max_ranges, sizeof(unsigned short), GFP_KERNEL);
	r_tags = kmalloc(max_ranges, GFP_KERNEL);
	if (!r_sizes || !r_tags) {
		result = false;
		goto ret;
	}

	for (i = 0; i < max_ranges; i++)
		r_tags[i] = mte_bitmap_read(bitmap, &bit_pos, MTE_TAG_SIZE);
	for (i = 0, sum = 0; i < max_ranges; i++) {
		if (i == largest_idx)
			continue;
		r_sizes[i] =
			mte_bitmap_read(bitmap, &bit_pos, MTE_BITS_PER_SIZE);
		if (!r_sizes[i]) {
			max_ranges = i;
			break;
		}
		sum += r_sizes[i];
	}
	if (sum >= MTE_GRANULES_PER_PAGE) {
		result = false;
		goto ret;
	}
	r_sizes[largest_idx] = MTE_GRANULES_PER_PAGE - sum;
	mte_ranges_to_tags(r_tags, r_sizes, max_ranges, tags);
	result = true;
ret:
	kfree(r_sizes);
	kfree(r_tags);
	return result;
}

/* Get pointer to the out-of-line storage from a handle. */
static void *mte_storage(unsigned long handle)
{
	if (mte_is_inline(handle))
		return NULL;
	return (void *)((handle & (~MTE_CACHE_ID_MASK)) | BIT_ULL(63));
}

/**
 * mte_decompress() - decompress the tag array addressed by the handle.
 * @handle: handle returned by @mte_decompress()
 * @tags: MTE_GRANULES_PER_PAGE-byte array to write the tags to.
 *
 * Reads the compressed data and writes it into the user-supplied tag array.
 *
 * Returns: true on success, false on error.
 */
bool mte_decompress(unsigned long handle, u8 *tags)
{
	unsigned long *storage = mte_storage(handle);
	size_t size = mte_storage_size(handle);

	switch (size) {
	case 8:
		return mte_decompress_from_buf(&handle, size, tags);
	case MTE_PAGE_TAG_STORAGE:
		memcpy(tags, storage, size);
		return true;
	default:
		return mte_decompress_from_buf(storage, size, tags);
	}
}
EXPORT_SYMBOL_NS(mte_decompress, MTECOMP);

/**
 * mte_release_handle() - release the handle returned by mte_compress().
 * @handle: handle returned by mte_compress().
 */
void mte_release_handle(unsigned long handle)
{
	unsigned int cache_id;
	struct kmem_cache *c;
	void *storage;
	size_t size;

	storage = mte_storage(handle);
	if (!storage) {
		atomic_long_inc(&dealloc_counters[0]);
		return;
	}

	size = mte_storage_size(handle);
	cache_id = mte_size_to_cache_id(size);
	c = mtecomp_caches[cache_id];
	kmem_cache_free(c, storage);
	atomic_long_inc(&dealloc_counters[cache_id + 1]);
}
EXPORT_SYMBOL_NS(mte_release_handle, MTECOMP);

/* DebugFS interface. */
static int stats_show(struct seq_file *seq, void *v)
{
	unsigned long total_mem_alloc = 0, total_mem_dealloc = 0;
	unsigned long total_num_alloc = 0, total_num_dealloc = 0;
	unsigned long size = 8;
	long alloc, dealloc;
	int i;

	for (i = 0; i <= MTECOMP_NUM_CACHES; i++) {
		alloc = atomic_long_read(&alloc_counters[i]);
		dealloc = atomic_long_read(&dealloc_counters[i]);
		total_num_alloc += alloc;
		total_num_dealloc += dealloc;
		/*
		 * Do not count 8-byte buffers towards compressed tag storage
		 * size.
		 */
		if (i) {
			total_mem_alloc += (size * alloc);
			total_mem_dealloc += (size * dealloc);
		}
		seq_printf(seq,
			   "%lu bytes: %lu allocations, %lu deallocations\n",
			   size, alloc, dealloc);
		size <<= 1;
	}
	seq_printf(seq, "uncompressed tag storage size: %lu\n",
		   (total_num_alloc - total_num_dealloc) *
			   MTE_PAGE_TAG_STORAGE);
	seq_printf(seq, "compressed tag storage size: %lu\n",
		   total_mem_alloc - total_mem_dealloc);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

static int mtecomp_debugfs_init(void)
{
	struct dentry *mtecomp_dir;

	mtecomp_dir = debugfs_create_dir("mtecomp", NULL);
	debugfs_create_file("stats", 0444, mtecomp_dir, NULL, &stats_fops);
	return 0;
}

/* Set up mtecomp_caches[]. */
static int mtecomp_init(void)
{
	unsigned int i;
	char name[16];
	size_t size;

	static_assert(MTECOMP_NUM_CACHES <= (MTE_CACHE_ID_MASK + 1));
	for (i = 0; i < MTECOMP_NUM_CACHES; i++) {
		size = mte_cache_id_to_size(i);
		snprintf(name, sizeof(name), "mte-tags-%ld", size);
		mtecomp_caches[i] =
			kmem_cache_create(name, size, size, 0, NULL);
	}
	mtecomp_debugfs_init();
	return 0;
}
module_init(mtecomp_init);
