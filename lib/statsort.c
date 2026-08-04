// SPDX-License-Identifier: GPL-2.0
/*
 * A distribution (bucket) sort for the Linux kernel
 *
 * statsort() sorts an array of elements by a caller-supplied signed
 * 64-bit key. At each level of recursion it distributes the elements
 * of a range into m = isqrt(n) buckets by linearly interpolating each
 * element's key over the range's [min, max) span, then recurses into
 * every non-empty bucket with the range narrowed to that bucket. This
 * is the same idea as radix/bucket sort: when keys are close to
 * uniformly distributed, each level shrinks n by a factor of roughly
 * sqrt(n), giving close to O(n) average behaviour overall.
 *
 * Two safety nets keep this from degrading to O(n^2) on adversarial
 * or skewed input:
 *
 *   - Ranges of STATSORT_THRESHOLD elements or fewer are finished off
 *     with a plain insertion sort, which is faster than recursing
 *     further for small n and bounds the recursion depth.
 *
 *   - If every element in a range lands in the same bucket (the key
 *     distribution didn't actually discriminate between them),
 *     recursing again would repeat the same bucketing forever. That
 *     case, and any allocation failure, falls back to sort_r() (see
 *     lib/sort.c), which guarantees O(n log n) worst-case behaviour.
 *
 * All arithmetic is done with plain integer keys; the kernel has no
 * business touching the FPU, so unlike a userspace bucket sort there
 * is no floating-point interpolation here, only scaled 64-bit integer
 * division.
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/statsort.h>
#include <linux/string.h>
#include <linux/types.h>

/* Ranges at or below this size are finished off with insertion sort. */
#define STATSORT_THRESHOLD 16

/*
 * Bundles the key function and its private argument so they can be
 * threaded through the sort_r()-based fallback path, which only has
 * room for a single opaque priv pointer.
 */
struct statsort_priv {
	statsort_key_func_t key;
	const void *arg;
};

static int statsort_cmp(const void *a, const void *b, const void *priv)
{
	const struct statsort_priv *p = priv;
	s64 ka = p->key(a, p->arg);
	s64 kb = p->key(b, p->arg);

	if (ka < kb)
		return -1;
	if (ka > kb)
		return 1;
	return 0;
}

/*
 * Guaranteed O(n log n) fallback used for small ranges' degenerate
 * siblings, allocation failure, and the single-bucket degenerate case.
 */
static void statsort_fallback(void *base, size_t num, size_t size,
			       statsort_key_func_t key_func, const void *priv)
{
	struct statsort_priv p = { .key = key_func, .arg = priv };

	sort_r(base, num, size, statsort_cmp, NULL, &p);
}

/**
 * statsort_insertion - insertion sort by key, for small ranges
 * @base: pointer to the range to sort
 * @num: number of elements in the range
 * @size: size of each element
 * @key_func: pointer to the key-extraction function
 * @priv: opaque pointer passed through to @key_func
 * @tmp: scratch buffer of at least @size bytes, for the sifted element
 *
 * O(n^2) worst case, but with very low constant factors, which makes
 * it faster than recursing further once a range is small.
 */
static void statsort_insertion(void *base, size_t num, size_t size,
				statsort_key_func_t key_func, const void *priv,
				void *tmp)
{
	size_t i, j;

	for (i = 1; i < num; i++) {
		s64 key;

		memcpy(tmp, base + i * size, size);
		key = key_func(tmp, priv);

		j = i;
		while (j > 0 && key_func(base + (j - 1) * size, priv) > key) {
			memcpy(base + j * size, base + (j - 1) * size, size);
			j--;
		}
		memcpy(base + j * size, tmp, size);
	}
}

/*
 * Map a key into one of @m buckets spanning the half-open key range
 * [@min, @min + @span). @span is precomputed by the caller as an
 * unsigned 64-bit quantity to sidestep signed overflow.
 */
static size_t statsort_bucket(s64 key, s64 min, u64 span, size_t m)
{
	size_t b;

	if (!span)
		return 0;

	b = div64_u64((u64)(key - min) * (u64)m, span);
	if (b >= m)
		b = m - 1;
	return b;
}

/*
 * Recursive core. @data holds the range to sort on entry; @scratch is
 * a same-sized buffer that either the caller allocated (top level) or
 * the previous level's own @data region (recursive levels), and ends
 * up holding the sorted range, which is then copied back into @data.
 */
static void statsort_recurse(void *data, size_t num, size_t size,
			      s64 min, s64 max, void *scratch,
			      statsort_key_func_t key_func, const void *priv,
			      void *tmp)
{
	size_t m, i, b, nonempty;
	size_t *cnt, *off, *pos;
	u64 span;

	if (num <= STATSORT_THRESHOLD) {
		statsort_insertion(data, num, size, key_func, priv, tmp);
		return;
	}

	m = int_sqrt(num);
	if (!m)
		m = 1;
	span = (u64)(max - min);

	cnt = kcalloc(m, sizeof(*cnt), GFP_KERNEL);
	if (!cnt) {
		statsort_fallback(data, num, size, key_func, priv);
		return;
	}

	for (i = 0; i < num; i++) {
		b = statsort_bucket(key_func(data + i * size, priv), min, span, m);
		cnt[b]++;
	}

	off = kmalloc_array(m + 1, sizeof(*off), GFP_KERNEL);
	if (!off) {
		kfree(cnt);
		statsort_fallback(data, num, size, key_func, priv);
		return;
	}
	off[0] = 0;
	for (i = 0; i < m; i++)
		off[i + 1] = off[i] + cnt[i];

	pos = kmalloc_array(m, sizeof(*pos), GFP_KERNEL);
	if (!pos) {
		kfree(off);
		kfree(cnt);
		statsort_fallback(data, num, size, key_func, priv);
		return;
	}
	memcpy(pos, off, m * sizeof(*pos));

	nonempty = 0;
	for (i = 0; i < m; i++)
		if (cnt[i])
			nonempty++;

	for (i = 0; i < num; i++) {
		b = statsort_bucket(key_func(data + i * size, priv), min, span, m);
		memcpy(scratch + pos[b] * size, data + i * size, size);
		pos[b]++;
	}
	kfree(pos);

	if (nonempty == 1) {
		/*
		 * Every element hashed into the same bucket: the key
		 * range didn't discriminate between them, so recursing
		 * again would just repeat this bucketing forever.
		 */
		memcpy(data, scratch, num * size);
		statsort_fallback(data, num, size, key_func, priv);
		kfree(off);
		kfree(cnt);
		return;
	}

	for (b = 0; b < m; b++) {
		size_t bstart = off[b];
		size_t bsize = cnt[b];
		s64 bmin, bmax;

		if (!bsize)
			continue;

		bmin = min + (s64)div_u64((u64)b * span, m);
		bmax = min + (s64)div_u64((u64)(b + 1) * span, m);

		if (bsize <= STATSORT_THRESHOLD)
			statsort_insertion(scratch + bstart * size, bsize, size,
					    key_func, priv, tmp);
		else
			statsort_recurse(scratch + bstart * size, bsize, size,
					  bmin, bmax, data + bstart * size,
					  key_func, priv, tmp);
	}

	kfree(off);
	kfree(cnt);
	memcpy(data, scratch, num * size);
}

/**
 * statsort - sort an array of elements by a signed 64-bit key
 * @base: pointer to data to sort
 * @num: number of elements
 * @size: size of each element
 * @key_func: pointer to a function returning each element's sort key
 * @priv: opaque pointer passed through to @key_func
 *
 * Sorts @num elements of @size bytes each, in ascending order of the
 * key returned by key_func(elem, priv). Performs best when keys are
 * close to uniformly distributed over their range, and falls back to
 * sort_r()'s guaranteed O(n log n) heapsort for skewed distributions
 * or on allocation failure, so worst-case behaviour is never worse
 * than a plain sort_r() call plus O(n) bucketing overhead.
 *
 * Like sort_r(), this is not a stable sort: elements that compare
 * equal may be reordered.
 */
void statsort(void *base, size_t num, size_t size,
	      statsort_key_func_t key_func, const void *priv)
{
	void *scratch, *tmp;
	s64 min, max;
	size_t i;

	if (num < 2 || !size)
		return;

	min = max = key_func(base, priv);
	for (i = 1; i < num; i++) {
		s64 key = key_func(base + i * size, priv);

		if (key < min)
			min = key;
		if (key > max)
			max = key;
	}

	if (min == max)
		return; /* all keys equal: already sorted */

	/*
	 * Bucketing treats the range as the half-open interval
	 * [min, max), so widen max by one to keep the true maximum
	 * key out of the (nonexistent) bucket m. Leave it alone in
	 * the vanishingly unlikely case that max is already S64_MAX.
	 */
	if (max < S64_MAX)
		max += 1;

	scratch = kmalloc_array(num, size, GFP_KERNEL);
	if (!scratch) {
		statsort_fallback(base, num, size, key_func, priv);
		return;
	}

	tmp = kmalloc(size, GFP_KERNEL);
	if (!tmp) {
		kfree(scratch);
		statsort_fallback(base, num, size, key_func, priv);
		return;
	}

	statsort_recurse(base, num, size, min, max, scratch, key_func, priv, tmp);

	kfree(tmp);
	kfree(scratch);
}
EXPORT_SYMBOL(statsort);

static s64 statsort_identity_key(const void *elem, const void *priv)
{
	return *(const long *)elem;
}

/**
 * statsort_longs - sort an array of longs
 * @base: pointer to the array
 * @num: number of elements
 *
 * Convenience wrapper around statsort() for plain "long" arrays,
 * mirroring the relationship between sort() and sort_r() in
 * lib/sort.c.
 */
void statsort_longs(long *base, size_t num)
{
	statsort(base, num, sizeof(*base), statsort_identity_key, NULL);
}
EXPORT_SYMBOL(statsort_longs);
