// SPDX-License-Identifier: GPL-2.0
/*
 * ucs.c - Universal Character Set processing
 */

#include <linux/array_size.h>
#include <linux/bsearch.h>
#include <linux/consolemap.h>
#include <linux/minmax.h>

struct ucs_interval16 {
	u16 first;
	u16 last;
};

struct ucs_interval32 {
	u32 first;
	u32 last;
};

#include "ucs_width_table.h"

static int interval16_cmp(const void *key, const void *element)
{
	u16 cp = *(u16 *)key;
	const struct ucs_interval16 *entry = element;

	if (cp < entry->first)
		return -1;
	if (cp > entry->last)
		return 1;
	return 0;
}

static int interval32_cmp(const void *key, const void *element)
{
	u32 cp = *(u32 *)key;
	const struct ucs_interval32 *entry = element;

	if (cp < entry->first)
		return -1;
	if (cp > entry->last)
		return 1;
	return 0;
}

static const struct ucs_interval16 *find_cp_in_range16(u16 cp,
						       const struct ucs_interval16 *ranges,
						       size_t size)
{
	if (cp < ranges[0].first || cp > ranges[size - 1].last)
		return NULL;

	return __inline_bsearch(&cp, ranges, size, sizeof(*ranges),
				interval16_cmp);
}

static bool cp_in_range16(u16 cp, const struct ucs_interval16 *ranges, size_t size)
{
	return find_cp_in_range16(cp, ranges, size) != NULL;
}

static bool cp_in_range32(u32 cp, const struct ucs_interval32 *ranges, size_t size)
{
	if (cp < ranges[0].first || cp > ranges[size - 1].last)
		return false;

	return __inline_bsearch(&cp, ranges, size, sizeof(*ranges),
				interval32_cmp) != NULL;
}

#define UCS_IS_BMP(cp)	((cp) <= 0xffff)

/**
 * ucs_is_zero_width() - Determine if a Unicode code point is zero-width.
 * @cp: Unicode code point (UCS-4)
 *
 * Return: true if the character is zero-width, false otherwise
 */
bool ucs_is_zero_width(u32 cp)
{
	if (UCS_IS_BMP(cp))
		return cp_in_range16(cp, ucs_zero_width_bmp_ranges,
				     ARRAY_SIZE(ucs_zero_width_bmp_ranges));
	else
		return cp_in_range32(cp, ucs_zero_width_non_bmp_ranges,
				     ARRAY_SIZE(ucs_zero_width_non_bmp_ranges));
}

/**
 * ucs_is_double_width() - Determine if a Unicode code point is double-width.
 * @cp: Unicode code point (UCS-4)
 *
 * Return: true if the character is double-width, false otherwise
 */
bool ucs_is_double_width(u32 cp)
{
	if (UCS_IS_BMP(cp))
		return cp_in_range16(cp, ucs_double_width_bmp_ranges,
				     ARRAY_SIZE(ucs_double_width_bmp_ranges));
	else
		return cp_in_range32(cp, ucs_double_width_non_bmp_ranges,
				     ARRAY_SIZE(ucs_double_width_non_bmp_ranges));
}

/*
 * Structure for base with combining mark pairs and resulting recompositions.
 * Using u16 to save space since all values are within BMP range.
 */
struct ucs_recomposition {
	u16 base;	/* base character */
	u16 mark;	/* combining mark */
	u16 recomposed;	/* corresponding recomposed character */
};

#include "ucs_recompose_table.h"

struct compare_key {
	u16 base;
	u16 mark;
};

static int recomposition_cmp(const void *key, const void *element)
{
	const struct compare_key *search_key = key;
	const struct ucs_recomposition *entry = element;

	/* Compare base character first */
	if (search_key->base < entry->base)
		return -1;
	if (search_key->base > entry->base)
		return 1;

	/* Base characters match, now compare combining character */
	if (search_key->mark < entry->mark)
		return -1;
	if (search_key->mark > entry->mark)
		return 1;

	/* Both match */
	return 0;
}

/**
 * ucs_recompose() - Attempt to recompose two Unicode characters into a single character.
 * @base: Base Unicode code point (UCS-4)
 * @mark: Combining mark Unicode code point (UCS-4)
 *
 * Return: Recomposed Unicode code point, or 0 if no recomposition is possible
 */
u32 ucs_recompose(u32 base, u32 mark)
{
	/* Check if characters are within the range of our table */
	if (base < UCS_RECOMPOSE_MIN_BASE || base > UCS_RECOMPOSE_MAX_BASE ||
	    mark < UCS_RECOMPOSE_MIN_MARK || mark > UCS_RECOMPOSE_MAX_MARK)
		return 0;

	struct compare_key key = { base, mark };
	struct ucs_recomposition *result =
		__inline_bsearch(&key, ucs_recomposition_table,
				 ARRAY_SIZE(ucs_recomposition_table),
				 sizeof(*ucs_recomposition_table),
				 recomposition_cmp);

	return result ? result->recomposed : 0;
}

/*
 * The fallback tables are using struct ucs_interval16 or plain literals
 * directly. We reuse interval16_cmp() for the former, but another compare
 * function is needed in the singles case.
 */

#include "ucs_fallback_table.h"

static int u16_cmp(const void *key, const void *element)
{
	u16 cp = *(u16 *)key;
	u16 entry = *(u16 *)element;

	if (cp < entry)
		return -1;
	if (cp > entry)
		return 1;
	return 0;
}

static u16 *find_cp_in_table16(u16 cp, const u16 *table, size_t size)
{
	if (cp < table[0] || cp > table[size - 1])
		return NULL;

	return __inline_bsearch(&cp, table, size, sizeof(u16), u16_cmp);
}

/**
 * ucs_get_fallback() - Get a substitution for the provided Unicode character
 * @base: Base Unicode code point (UCS-4)
 *
 * Get a simpler fallback character for the provided Unicode character.
 * This is used for terminal display when corresponding glyph is unavailable.
 * The substitution may not be as good as the actual glyph for the original
 * character but still way more helpful than a squared question mark.
 *
 * Return: Fallback Unicode code point, or 0 if none is available
 */
u32 ucs_get_fallback(u32 cp)
{
	const struct ucs_interval16 *interval;
	u16 *single;

	if (!UCS_IS_BMP(cp))
		return 0;

	interval = find_cp_in_range16(cp, ucs_fallback_intervals,
				      ARRAY_SIZE(ucs_fallback_intervals));
	if (interval)
		return ucs_fallback_intervals_subs[interval - ucs_fallback_intervals];

	single = find_cp_in_table16(cp, ucs_fallback_singles,
				    ARRAY_SIZE(ucs_fallback_singles));
	if (single)
		return ucs_fallback_singles_subs[single - ucs_fallback_singles];

	return 0;
}
