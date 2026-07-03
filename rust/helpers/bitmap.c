// SPDX-License-Identifier: GPL-2.0

#include <linux/bitmap.h>

__rust_helper
void rust_helper_bitmap_copy_and_extend(unsigned long *to, const unsigned long *from,
		unsigned int count, unsigned int size)
{
	bitmap_copy_and_extend(to, from, count, size);
}

__rust_helper
unsigned long rust_helper_bitmap_find_next_zero_area(unsigned long *map,
						     unsigned long size,
						     unsigned long start,
						     unsigned int nr,
						     unsigned long align_mask)
{
	return bitmap_find_next_zero_area(map, size, start, nr, align_mask);
}

__rust_helper
void rust_helper_bitmap_set(unsigned long *map, unsigned int start, unsigned int nbits)
{
	bitmap_set(map, start, nbits);
}

__rust_helper
void rust_helper_bitmap_clear(unsigned long *map, unsigned int start, unsigned int nbits)
{
	bitmap_clear(map, start, nbits);
}
