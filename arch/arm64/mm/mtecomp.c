// SPDX-License-Identifier: GPL-2.0-only

/*
 * MTE tag compression algorithm.
 * See Documentation/arch/arm64/mte-tag-compression.rst for more details.
 */

#include <linux/bits.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/mtecomp.h>

#include "mtecomp.h"

#define MTE_BITS_PER_LARGEST_IDX 3
/* Range size cannot exceed MTE_GRANULES_PER_PAGE / 2. */
#define MTE_BITS_PER_SIZE (ilog2(MTE_GRANULES_PER_PAGE) - 1)

/*
 * See Documentation/arch/arm64/mte-tag-compression.rst for details on how the
 * maximum number of ranges is calculated.
 */
#if defined(CONFIG_ARM64_4K_PAGES)
#define MTE_MAX_RANGES 6
#elif defined(CONFIG_ARM64_16K_PAGES)
#define MTE_MAX_RANGES 5
#else
#define MTE_MAX_RANGES 4
#endif

/**
 * mte_tags_to_ranges() - break @tags into arrays of tag ranges.
 * @tags: MTE_GRANULES_PER_PAGE-byte array containing MTE tags.
 * @out_tags: u8 array to store the tag of every range.
 * @out_sizes: unsigned short array to store the size of every range.
 * @out_len: length of @out_tags and @out_sizes (output parameter, initially
 *           equal to lengths of out_tags[] and out_sizes[]).
 *
 * This function is exported for testing purposes.
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
 *
 * This function is exported for testing purposes.
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

static void mte_bitmap_write(unsigned long *bitmap, unsigned long value,
			     unsigned long *pos, unsigned long bits)
{
	bitmap_write(bitmap, value, *pos, bits);
	*pos += bits;
}

/* Compress ranges into an unsigned long. */
static void mte_compress_to_ulong(size_t len, u8 *tags, unsigned short *sizes,
				  unsigned long *result)
{
	unsigned long bit_pos = 0;
	unsigned int largest_idx, i;
	unsigned short largest = 0;

	for (i = 0; i < len; i++) {
		if (sizes[i] > largest) {
			largest = sizes[i];
			largest_idx = i;
		}
	}
	/* Bit 1 in position 0 indicates compressed data. */
	mte_bitmap_write(result, 1, &bit_pos, 1);
	mte_bitmap_write(result, largest_idx, &bit_pos,
			 MTE_BITS_PER_LARGEST_IDX);
	for (i = 0; i < len; i++)
		mte_bitmap_write(result, tags[i], &bit_pos, MTE_TAG_SIZE);
	if (len == 1) {
		/*
		 * We are compressing MTE_GRANULES_PER_PAGE of identical tags.
		 * Split it into two ranges containing
		 * MTE_GRANULES_PER_PAGE / 2 tags, so that it falls into the
		 * special case described below.
		 */
		mte_bitmap_write(result, tags[0], &bit_pos, MTE_TAG_SIZE);
		i = 2;
	} else {
		i = len;
	}
	for (; i < MTE_MAX_RANGES; i++)
		mte_bitmap_write(result, 0, &bit_pos, MTE_TAG_SIZE);
	/*
	 * Most of the time sizes[i] fits into MTE_BITS_PER_SIZE, apart from a
	 * special case when:
	 *   len = 2;
	 *   sizes = { MTE_GRANULES_PER_PAGE / 2, MTE_GRANULES_PER_PAGE / 2};
	 * In this case largest_idx will be set to 0, and the size written to
	 * the bitmap will be also 0.
	 */
	for (i = 0; i < len; i++) {
		if (i != largest_idx)
			mte_bitmap_write(result, sizes[i], &bit_pos,
					 MTE_BITS_PER_SIZE);
	}
	for (i = len; i < MTE_MAX_RANGES; i++)
		mte_bitmap_write(result, 0, &bit_pos, MTE_BITS_PER_SIZE);
}

/**
 * mte_compress() - compress the given tag array.
 * @tags: MTE_GRANULES_PER_PAGE-byte array to read the tags from.
 *
 * Attempts to compress the user-supplied tag array.
 *
 * Returns: compressed data or NULL.
 */
void *mte_compress(u8 *tags)
{
	unsigned short *r_sizes;
	void *result = NULL;
	u8 *r_tags;
	size_t r_len;

	r_sizes = kmalloc_array(MTE_GRANULES_PER_PAGE, sizeof(unsigned short),
				GFP_KERNEL);
	r_tags = kmalloc(MTE_GRANULES_PER_PAGE, GFP_KERNEL);
	if (!r_sizes || !r_tags)
		goto ret;
	r_len = MTE_GRANULES_PER_PAGE;
	mte_tags_to_ranges(tags, r_tags, r_sizes, &r_len);
	if (r_len <= MTE_MAX_RANGES)
		mte_compress_to_ulong(r_len, r_tags, r_sizes,
				      (unsigned long *)&result);
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

/**
 * mte_decompress() - decompress the tag array from the given pointer.
 * @data: pointer returned by @mte_compress()
 * @tags: MTE_GRANULES_PER_PAGE-byte array to write the tags to.
 *
 * Reads the compressed data and writes it into the user-supplied tag array.
 *
 * Returns: true on success, false if the passed data is uncompressed.
 */
bool mte_decompress(void *data, u8 *tags)
{
	unsigned short r_sizes[MTE_MAX_RANGES];
	u8 r_tags[MTE_MAX_RANGES];
	unsigned int largest_idx, i;
	unsigned long bit_pos = 0;
	unsigned long *bitmap;
	unsigned short sum;
	size_t max_ranges;

	if (!mte_is_compressed(data))
		return false;

	bitmap = (unsigned long *)&data;
	max_ranges = MTE_MAX_RANGES;
	/* Skip the leading bit indicating the inline case. */
	mte_bitmap_read(bitmap, &bit_pos, 1);
	largest_idx =
		mte_bitmap_read(bitmap, &bit_pos, MTE_BITS_PER_LARGEST_IDX);
	if (largest_idx >= MTE_MAX_RANGES)
		return false;

	for (i = 0; i < max_ranges; i++)
		r_tags[i] = mte_bitmap_read(bitmap, &bit_pos, MTE_TAG_SIZE);
	for (i = 0, sum = 0; i < max_ranges; i++) {
		if (i == largest_idx)
			continue;
		r_sizes[i] =
			mte_bitmap_read(bitmap, &bit_pos, MTE_BITS_PER_SIZE);
		/*
		 * Special case: tag array consists of two ranges of
		 * `MTE_GRANULES_PER_PAGE / 2` tags.
		 */
		if ((largest_idx == 0) && (i == 1) && (r_sizes[i] == 0))
			r_sizes[i] = MTE_GRANULES_PER_PAGE / 2;
		if (!r_sizes[i]) {
			max_ranges = i;
			break;
		}
		sum += r_sizes[i];
	}
	if (sum >= MTE_GRANULES_PER_PAGE)
		return false;
	r_sizes[largest_idx] = MTE_GRANULES_PER_PAGE - sum;
	mte_ranges_to_tags(r_tags, r_sizes, max_ranges, tags);
	return true;
}
EXPORT_SYMBOL_NS(mte_decompress, MTECOMP);
