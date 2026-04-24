/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Dual-bitmap consistency primitives
 *
 * Provides a generic library for maintaining dual bitmaps with the invariant
 * that (primary == ~secondary). This pattern is useful for detecting
 * single-bit memory corruption in bitmap-based data structures.
 *
 * Based on NVIDIA safety research.
 */
#ifndef _LINUX_DUAL_BITMAP_H
#define _LINUX_DUAL_BITMAP_H

#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/bitmap.h>
#include <linux/bug.h>
#include <asm/barrier.h>
#include <linux/processor.h>

/* Number of retries for transient inconsistencies from concurrent updates */
#define DUAL_BITMAP_RETRY_COUNT 3

/* Bitmap indices */
enum dual_bitmap_index {
	DUAL_BITMAP_PRIMARY = 0,	/* 0=free, 1=allocated */
	DUAL_BITMAP_SECONDARY = 1,	/* 0=allocated, 1=free (complement) */
	DUAL_BITMAP_COUNT = 2
};

/**
 * struct dual_bitmap - Dual bitmap structure
 * @bitmap: Array of two bitmap pointers [PRIMARY, SECONDARY]
 * @nbits: Number of bits in each bitmap
 */
struct dual_bitmap {
	unsigned long *bitmap[DUAL_BITMAP_COUNT];
	unsigned int nbits;
};

/**
 * dual_bitmap_consistent_word - Check if a word pair maintains the invariant
 * @primary: Primary bitmap word
 * @secondary: Secondary bitmap word
 *
 * Returns true if primary == ~secondary
 */
static inline bool dual_bitmap_consistent_word(unsigned long primary,
					       unsigned long secondary)
{
	return primary == ~secondary;
}

/**
 * dual_bitmap_set - Set bit in dual bitmap (mark as allocated)
 * @db: Dual bitmap structure
 * @bit: Bit position to set
 *
 * Sets bit in primary and clears corresponding bit in secondary.
 * Returns the old value of the primary bit (true if was already set).
 */
static inline bool dual_bitmap_set(struct dual_bitmap *db, unsigned long bit)
{
	bool was_set;

	if (WARN_ON_ONCE(bit >= db->nbits))
		return false;

	was_set = test_and_set_bit(bit, db->bitmap[DUAL_BITMAP_PRIMARY]);
	test_and_clear_bit(bit, db->bitmap[DUAL_BITMAP_SECONDARY]);

	return was_set;
}

/**
 * dual_bitmap_clear - Clear bit in dual bitmap (mark as free)
 * @db: Dual bitmap structure
 * @bit: Bit position to clear
 *
 * Clears bit in primary and sets corresponding bit in secondary.
 * Returns the old value of the primary bit (true if was set).
 */
static inline bool dual_bitmap_clear(struct dual_bitmap *db, unsigned long bit)
{
	bool was_set;

	if (WARN_ON_ONCE(bit >= db->nbits))
		return false;

	was_set = test_and_clear_bit(bit, db->bitmap[DUAL_BITMAP_PRIMARY]);
	test_and_set_bit(bit, db->bitmap[DUAL_BITMAP_SECONDARY]);

	return was_set;
}

/**
 * dual_bitmap_test - Test if bit is set in primary bitmap
 * @db: Dual bitmap structure
 * @bit: Bit position to test
 *
 * Returns true if bit is set in primary (allocated), false if clear (free).
 */
static inline bool dual_bitmap_test(const struct dual_bitmap *db,
				    unsigned long bit)
{
	if (WARN_ON_ONCE(bit >= db->nbits))
		return false;

	return test_bit(bit, db->bitmap[DUAL_BITMAP_PRIMARY]);
}

/**
 * dual_bitmap_consistent - Check consistency of a single bit
 * @db: Dual bitmap structure
 * @bit: Bit position to check
 *
 * Returns true if the bit values are consistent (primary != secondary).
 * Uses retry logic to handle transient inconsistencies from concurrent
 * updates - real corruption persists while races resolve quickly.
 */
static inline bool dual_bitmap_consistent(const struct dual_bitmap *db,
					  unsigned long bit)
{
	int retries = DUAL_BITMAP_RETRY_COUNT;

	if (WARN_ON_ONCE(bit >= db->nbits))
		return false;

	do {
		bool primary = test_bit(bit, db->bitmap[DUAL_BITMAP_PRIMARY]);
		bool secondary = test_bit(bit, db->bitmap[DUAL_BITMAP_SECONDARY]);

		if (primary != secondary)
			return true;  /* Consistent */

		/* Inconsistent - could be transient race, retry */
		cpu_relax();
	} while (--retries > 0);

	/*
	 * Inconsistent after retries. Issue a read barrier and check
	 * one last time to rule out stale/reordered reads.
	 *
	 * Note: the two test_bit() calls are still non-atomic w.r.t.
	 * each other, so a concurrent set/clear between them can cause
	 * a transient false positive. This is acceptable because real
	 * corruption is persistent and will be caught on the next check.
	 */
	smp_rmb();
	return test_bit(bit, db->bitmap[DUAL_BITMAP_PRIMARY]) !=
	       test_bit(bit, db->bitmap[DUAL_BITMAP_SECONDARY]);
}

/**
 * dual_bitmap_validate - Validate entire dual bitmap
 * @db: Dual bitmap structure
 *
 * Checks that the invariant (primary == ~secondary) holds for all words.
 * Uses retry logic to handle transient inconsistencies from concurrent
 * updates - real corruption persists while races resolve quickly.
 * Returns the number of inconsistent words found (0 = all consistent).
 *
 * Note: this is a cold-path diagnostic function kept inline for
 * header-only library simplicity. It should not be called in hot paths.
 */
static inline unsigned long dual_bitmap_validate(const struct dual_bitmap *db)
{
	unsigned int words = BITS_TO_LONGS(db->nbits);
	unsigned long violations = 0;
	unsigned int i;

	for (i = 0; i < words; i++) {
		unsigned long primary, secondary;
		int retries = DUAL_BITMAP_RETRY_COUNT;

		do {
			primary = READ_ONCE(db->bitmap[DUAL_BITMAP_PRIMARY][i]);
			secondary = READ_ONCE(db->bitmap[DUAL_BITMAP_SECONDARY][i]);

			if (dual_bitmap_consistent_word(primary, secondary))
				break;  /* Consistent, move to next word */

			cpu_relax();
		} while (--retries > 0);

		if (retries == 0) {
			/*
			 * Inconsistent after retries. Issue a read
			 * barrier and re-read to rule out stale/reordered
			 * memory views before declaring corruption.
			 */
			smp_rmb();
			primary = READ_ONCE(db->bitmap[DUAL_BITMAP_PRIMARY][i]);
			secondary = READ_ONCE(db->bitmap[DUAL_BITMAP_SECONDARY][i]);
			if (!dual_bitmap_consistent_word(primary, secondary))
				violations++;
		}
	}

	return violations;
}

/**
 * dual_bitmap_init - Initialize dual bitmap to empty state
 * @db: Dual bitmap structure
 *
 * Sets primary to all zeros (nothing allocated) and secondary to all ones.
 * The bitmaps must already be allocated before calling this.
 */
static inline void dual_bitmap_init(struct dual_bitmap *db)
{
	bitmap_zero(db->bitmap[DUAL_BITMAP_PRIMARY], db->nbits);
	bitmap_fill(db->bitmap[DUAL_BITMAP_SECONDARY], db->nbits);
}

#endif /* _LINUX_DUAL_BITMAP_H */
