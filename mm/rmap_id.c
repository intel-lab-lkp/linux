// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * rmap ID tracking for precise "mapped shared" vs. "mapped exclusively"
 * detection of partially-mappable folios (e.g., PTE-mapped THP).
 *
 * Copyright Red Hat, Inc. 2023
 *
 * Author(s): David Hildenbrand <david@redhat.com>
 */

#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/idr.h>

#include "internal.h"

static DEFINE_SPINLOCK(rmap_id_lock);
static DEFINE_IDA(rmap_ida);

/* For now we only expect folios from the buddy, not hugetlb folios. */
#if MAX_ORDER > RMAP_SUBID_6_MAX_ORDER
#error "rmap ID tracking does not support such large MAX_ORDER"
#endif

/*
 * We assign each MM a unique rmap ID and derive from it a sequence of
 * special sub-IDs. We add/remove these sub-IDs to/from the corresponding
 * folio rmap values (folio->rmap_valX) whenever (un)mapping (parts of) a
 * partially mappable folio.
 *
 * With 24bit rmap IDs, and a folio size that is compatible with 4
 * rmap values (more below), we calculate the sub-ID sequence like this:
 *
 * rmap ID    :  | 3 3 3 3 3 3 | 2 2 2 2 2 2 | 1 1 1 1 1 1 | 0 0 0 0 0 0 |
 * sub-ID IDX :  |   IDX #3    |   IDX #2    |   IDX #1    |   IDX #0    |
 *
 * sub-IDs    :  [ subid_4(#3), subid_4(#2), subid_4(#1), subid_4(#0) ]
 * rmap value :  [  _rmap_val3,  _rmap_val2,  _rmap_val1,  _rmap_val0 ]
 *
 * Any time we map/unmap a part (e.g., PTE, PMD) of a partially-mappable
 * folio to/from a MM, we:
 *  (1) Adjust (increment/decrement) the mapcount of the folio
 *  (2) Adjust (add/remove) the folio rmap values using the MM sub-IDs
 *
 * So the rmap values are always linked to the folio mapcount.
 * Consequently, we know that a single rmap value in the folio is the sum
 * of exactly #folio_mapcount() rmap sub-IDs. As one example, if the folio
 * is completely unmapped, the rmap values must be 0. As another example,
 * if the folio is mapped exactly once, the rmap values correspond to the
 * MM sub-IDs.
 *
 * To identify whether a given MM is responsible for all #folio_mapcount()
 * mappings of a folio ("mapped exclusively") or whether other MMs are
 * involved ("mapped shared"), we perform the following checks:
 *  (1) Do we have more mappings than the folio has pages? Then the folio
 *      is mapped shared. So when "folio_mapcount() > folio_nr_pages()".
 *  (2) Do the rmap values corresond to "#folio_mapcount() * sub-IDs" of
 *      the MM? Then the folio is mapped exclusive.
 *
 * To achieve (2), we generate sub-IDs that have the following property,
 * assuming that our folio has P=folio_nr_pages() pages.
 *   "2 * sub-ID" cannot be represented by the sum of any other *2* sub-IDs
 *   "3 * sub-ID" cannot be represented by the sum of any other *3* sub-IDs
 *   "4 * sub-ID" cannot be represented by the sum of any other *4* sub-IDs
 *   ...
 *   "P * sub-ID" cannot be represented by the sum of any other *P* sub-IDs
 *
 * Further, we want "P * sub-ID" (the maximum number we will ever look at)
 * to not overflow. If we overflow with " > P" mappings, we don't care as
 * we won't be looking at the numbers until theya re fully expressive
 * again.
 *
 * Consequently, to not overflow 64bit values with "P * sub-ID", folios
 * with large P require more rmap values (we cannot generate that many sub
 * IDs), whereby folios with smaller P can get away with less rmap values
 * (we can generate more sub-IDs).
 *
 * The sub-IDs are generated in generations, whereby
 * (1) Generation #0 is the number 0
 * (2) Generation #N takes all numbers from generations #0..#N-1 and adds
 *     (P + 1)^(N - 1), effectively doubling the number of sub-IDs
 *
 * Note: a PMD-sized THP can, for a short time while PTE-mapping it, be
 *       mapped using PTEs and a single PMD, resulting in "P + 1" mappings.
 *       For now, we don't consider this case, as we are ususally not
 *       looking at such folios while they being remapped, because the
 *       involved page tables are locked and stop any page table walkers.
 */

/*
 * With 4 (order-2) possible exclusive mappings per folio, we can have
 * 16777216 = 16M sub-IDs per 64bit value.
 */
static unsigned long get_rmap_subid_1(struct mm_struct *mm)
{
	return mm->mm_rmap_subid_1;
}

/*
 * With 32 (order-5) possible exclusive mappings per folio, we can have
 * 4096 sub-IDs per 64bit value.
 *
 * With 2 such 64bit values, we can support 4096^2 == 16M IDs.
 */
static unsigned long get_rmap_subid_2(struct mm_struct *mm, int nr)
{
	VM_WARN_ON_ONCE(nr > 1);
	return mm->mm_rmap_subid_2[nr];
}

/*
 * With 512 (order-9) possible exclusive mappings per folio, we can have
 * 128 sub-IDs per 64bit value.
 *
 * With 3 such 64bit values, we can support 128^3 == 16M IDs.
 */
static unsigned long get_rmap_subid_3(struct mm_struct *mm, int nr)
{
	VM_WARN_ON_ONCE(nr > 2);
	return mm->mm_rmap_subid_3[nr];
}

/*
 * With 1024 (order-10) possible exclusive mappings per folio, we can have 64
 * sub-IDs per 64bit value.
 *
 * With 4 such 64bit values, we can support 64^4 == 16M IDs.
 */
static const unsigned long rmap_subids_4[64] = {
	0ul,
	1ul,
	1025ul,
	1026ul,
	1050625ul,
	1050626ul,
	1051650ul,
	1051651ul,
	1076890625ul,
	1076890626ul,
	1076891650ul,
	1076891651ul,
	1077941250ul,
	1077941251ul,
	1077942275ul,
	1077942276ul,
	1103812890625ul,
	1103812890626ul,
	1103812891650ul,
	1103812891651ul,
	1103813941250ul,
	1103813941251ul,
	1103813942275ul,
	1103813942276ul,
	1104889781250ul,
	1104889781251ul,
	1104889782275ul,
	1104889782276ul,
	1104890831875ul,
	1104890831876ul,
	1104890832900ul,
	1104890832901ul,
	1131408212890625ul,
	1131408212890626ul,
	1131408212891650ul,
	1131408212891651ul,
	1131408213941250ul,
	1131408213941251ul,
	1131408213942275ul,
	1131408213942276ul,
	1131409289781250ul,
	1131409289781251ul,
	1131409289782275ul,
	1131409289782276ul,
	1131409290831875ul,
	1131409290831876ul,
	1131409290832900ul,
	1131409290832901ul,
	1132512025781250ul,
	1132512025781251ul,
	1132512025782275ul,
	1132512025782276ul,
	1132512026831875ul,
	1132512026831876ul,
	1132512026832900ul,
	1132512026832901ul,
	1132513102671875ul,
	1132513102671876ul,
	1132513102672900ul,
	1132513102672901ul,
	1132513103722500ul,
	1132513103722501ul,
	1132513103723525ul,
	1132513103723526ul,
};

static unsigned long get_rmap_subid_4(struct mm_struct *mm, int nr)
{
	const unsigned int rmap_id = mm->mm_rmap_id;

	VM_WARN_ON_ONCE(rmap_id < RMAP_ID_MIN || rmap_id > RMAP_ID_MAX || nr > 3);
	return rmap_subids_4[(rmap_id >> (nr * 6)) & 0x3f];
}

#if MAX_ORDER >= RMAP_SUBID_5_MIN_ORDER
/*
 * With 4096 (order-12) possible exclusive mappings per folio, we can have
 * 32 sub-IDs per 64bit value.
 *
 * With 5 such 64bit values, we can support 32^5 > 16M IDs.
 */
static const unsigned long rmap_subids_5[32] = {
	0ul,
	1ul,
	4097ul,
	4098ul,
	16785409ul,
	16785410ul,
	16789506ul,
	16789507ul,
	68769820673ul,
	68769820674ul,
	68769824770ul,
	68769824771ul,
	68786606082ul,
	68786606083ul,
	68786610179ul,
	68786610180ul,
	281749955297281ul,
	281749955297282ul,
	281749955301378ul,
	281749955301379ul,
	281749972082690ul,
	281749972082691ul,
	281749972086787ul,
	281749972086788ul,
	281818725117954ul,
	281818725117955ul,
	281818725122051ul,
	281818725122052ul,
	281818741903363ul,
	281818741903364ul,
	281818741907460ul,
	281818741907461ul,
};

static unsigned long get_rmap_subid_5(struct mm_struct *mm, int nr)
{
	const unsigned int rmap_id = mm->mm_rmap_id;

	VM_WARN_ON_ONCE(rmap_id < RMAP_ID_MIN || rmap_id > RMAP_ID_MAX || nr > 4);
	return rmap_subids_5[(rmap_id >> (nr * 5)) & 0x1f];
}
#endif

#if MAX_ORDER >= RMAP_SUBID_6_MIN_ORDER
/*
 * With 32768 (order-15) possible exclusive mappings per folio, we can have
 * 16 sub-IDs per 64bit value.
 *
 * With 6 such 64bit values, we can support 8^6 == 16M IDs.
 */
static const unsigned long rmap_subids_6[16] = {
	0ul,
	1ul,
	32769ul,
	32770ul,
	1073807361ul,
	1073807362ul,
	1073840130ul,
	1073840131ul,
	35187593412609ul,
	35187593412610ul,
	35187593445378ul,
	35187593445379ul,
	35188667219970ul,
	35188667219971ul,
	35188667252739ul,
	35188667252740ul,
};

static unsigned long get_rmap_subid_6(struct mm_struct *mm, int nr)
{
	const unsigned int rmap_id = mm->mm_rmap_id;

	VM_WARN_ON_ONCE(rmap_id < RMAP_ID_MIN || rmap_id > RMAP_ID_MAX || nr > 15);
	return rmap_subids_6[(rmap_id >> (nr * 4)) & 0xf];
}
#endif

void __folio_set_large_rmap_val(struct folio *folio, int count,
		struct mm_struct *mm)
{
	const unsigned int order = folio_order(folio);

	switch (order) {
#if MAX_ORDER >= RMAP_SUBID_6_MIN_ORDER
	case RMAP_SUBID_6_MIN_ORDER ... RMAP_SUBID_6_MAX_ORDER:
		atomic_long_set(&folio->_rmap_val0, get_rmap_subid_6(mm, 0) * count);
		atomic_long_set(&folio->_rmap_val1, get_rmap_subid_6(mm, 1) * count);
		atomic_long_set(&folio->_rmap_val2, get_rmap_subid_6(mm, 2) * count);
		atomic_long_set(&folio->_rmap_val3, get_rmap_subid_6(mm, 3) * count);
		atomic_long_set(&folio->_rmap_val4, get_rmap_subid_6(mm, 4) * count);
		atomic_long_set(&folio->_rmap_val5, get_rmap_subid_6(mm, 5) * count);
		break;
#endif
#if MAX_ORDER >= RMAP_SUBID_5_MIN_ORDER
	case RMAP_SUBID_5_MIN_ORDER ... RMAP_SUBID_5_MAX_ORDER:
		atomic_long_set(&folio->_rmap_val0, get_rmap_subid_5(mm, 0) * count);
		atomic_long_set(&folio->_rmap_val1, get_rmap_subid_5(mm, 1) * count);
		atomic_long_set(&folio->_rmap_val2, get_rmap_subid_5(mm, 2) * count);
		atomic_long_set(&folio->_rmap_val3, get_rmap_subid_5(mm, 3) * count);
		atomic_long_set(&folio->_rmap_val4, get_rmap_subid_5(mm, 4) * count);
		break;
#endif
	case RMAP_SUBID_4_MIN_ORDER ... RMAP_SUBID_4_MAX_ORDER:
		atomic_long_set(&folio->_rmap_val0, get_rmap_subid_4(mm, 0) * count);
		atomic_long_set(&folio->_rmap_val1, get_rmap_subid_4(mm, 1) * count);
		atomic_long_set(&folio->_rmap_val2, get_rmap_subid_4(mm, 2) * count);
		atomic_long_set(&folio->_rmap_val3, get_rmap_subid_4(mm, 3) * count);
		break;
	case RMAP_SUBID_3_MIN_ORDER ... RMAP_SUBID_3_MAX_ORDER:
		atomic_long_set(&folio->_rmap_val0, get_rmap_subid_3(mm, 0) * count);
		atomic_long_set(&folio->_rmap_val1, get_rmap_subid_3(mm, 1) * count);
		atomic_long_set(&folio->_rmap_val2, get_rmap_subid_3(mm, 2) * count);
		break;
	case RMAP_SUBID_2_MIN_ORDER ... RMAP_SUBID_2_MAX_ORDER:
		atomic_long_set(&folio->_rmap_val0, get_rmap_subid_2(mm, 0) * count);
		atomic_long_set(&folio->_rmap_val1, get_rmap_subid_2(mm, 1) * count);
		break;
	default:
		atomic_long_set(&folio->_rmap_val0, get_rmap_subid_1(mm) * count);
		break;
	}
}

void __folio_add_large_rmap_val(struct folio *folio, int count,
		struct mm_struct *mm)
{
	const unsigned int order = folio_order(folio);

	switch (order) {
#if MAX_ORDER >= RMAP_SUBID_6_MIN_ORDER
	case RMAP_SUBID_6_MIN_ORDER ... RMAP_SUBID_6_MAX_ORDER:
		atomic_long_add(get_rmap_subid_6(mm, 0) * count, &folio->_rmap_val0);
		atomic_long_add(get_rmap_subid_6(mm, 1) * count, &folio->_rmap_val1);
		atomic_long_add(get_rmap_subid_6(mm, 2) * count, &folio->_rmap_val2);
		atomic_long_add(get_rmap_subid_6(mm, 3) * count, &folio->_rmap_val3);
		atomic_long_add(get_rmap_subid_6(mm, 4) * count, &folio->_rmap_val4);
		atomic_long_add(get_rmap_subid_6(mm, 5) * count, &folio->_rmap_val5);
		break;
#endif
#if MAX_ORDER >= RMAP_SUBID_5_MIN_ORDER
	case RMAP_SUBID_5_MIN_ORDER ... RMAP_SUBID_5_MAX_ORDER:
		atomic_long_add(get_rmap_subid_5(mm, 0) * count, &folio->_rmap_val0);
		atomic_long_add(get_rmap_subid_5(mm, 1) * count, &folio->_rmap_val1);
		atomic_long_add(get_rmap_subid_5(mm, 2) * count, &folio->_rmap_val2);
		atomic_long_add(get_rmap_subid_5(mm, 3) * count, &folio->_rmap_val3);
		atomic_long_add(get_rmap_subid_5(mm, 4) * count, &folio->_rmap_val4);
		break;
#endif
	case RMAP_SUBID_4_MIN_ORDER ... RMAP_SUBID_4_MAX_ORDER:
		atomic_long_add(get_rmap_subid_4(mm, 0) * count, &folio->_rmap_val0);
		atomic_long_add(get_rmap_subid_4(mm, 1) * count, &folio->_rmap_val1);
		atomic_long_add(get_rmap_subid_4(mm, 2) * count, &folio->_rmap_val2);
		atomic_long_add(get_rmap_subid_4(mm, 3) * count, &folio->_rmap_val3);
		break;
	case RMAP_SUBID_3_MIN_ORDER ... RMAP_SUBID_3_MAX_ORDER:
		atomic_long_add(get_rmap_subid_3(mm, 0) * count, &folio->_rmap_val0);
		atomic_long_add(get_rmap_subid_3(mm, 1) * count, &folio->_rmap_val1);
		atomic_long_add(get_rmap_subid_3(mm, 2) * count, &folio->_rmap_val2);
		break;
	case RMAP_SUBID_2_MIN_ORDER ... RMAP_SUBID_2_MAX_ORDER:
		atomic_long_add(get_rmap_subid_2(mm, 0) * count, &folio->_rmap_val0);
		atomic_long_add(get_rmap_subid_2(mm, 1) * count, &folio->_rmap_val1);
		break;
	default:
		atomic_long_add(get_rmap_subid_1(mm) * count, &folio->_rmap_val0);
		break;
	}
}

bool __folio_has_large_matching_rmap_val(struct folio *folio, int count,
		 struct mm_struct *mm)
{
	const unsigned int order = folio_order(folio);
	unsigned long diff = 0;

	switch (order) {
#if MAX_ORDER >= RMAP_SUBID_6_MIN_ORDER
	case RMAP_SUBID_6_MIN_ORDER ... RMAP_SUBID_6_MAX_ORDER:
		diff |= atomic_long_read(&folio->_rmap_val0) ^ (get_rmap_subid_6(mm, 0) * count);
		diff |= atomic_long_read(&folio->_rmap_val1) ^ (get_rmap_subid_6(mm, 1) * count);
		diff |= atomic_long_read(&folio->_rmap_val2) ^ (get_rmap_subid_6(mm, 2) * count);
		diff |= atomic_long_read(&folio->_rmap_val3) ^ (get_rmap_subid_6(mm, 3) * count);
		diff |= atomic_long_read(&folio->_rmap_val4) ^ (get_rmap_subid_6(mm, 4) * count);
		diff |= atomic_long_read(&folio->_rmap_val5) ^ (get_rmap_subid_6(mm, 5) * count);
		break;
#endif
#if MAX_ORDER >= RMAP_SUBID_5_MIN_ORDER
	case RMAP_SUBID_5_MIN_ORDER ... RMAP_SUBID_5_MAX_ORDER:
		diff |= atomic_long_read(&folio->_rmap_val0) ^ (get_rmap_subid_5(mm, 0) * count);
		diff |= atomic_long_read(&folio->_rmap_val1) ^ (get_rmap_subid_5(mm, 1) * count);
		diff |= atomic_long_read(&folio->_rmap_val2) ^ (get_rmap_subid_5(mm, 2) * count);
		diff |= atomic_long_read(&folio->_rmap_val3) ^ (get_rmap_subid_5(mm, 3) * count);
		diff |= atomic_long_read(&folio->_rmap_val4) ^ (get_rmap_subid_5(mm, 4) * count);
		break;
#endif
	case RMAP_SUBID_4_MIN_ORDER ... RMAP_SUBID_4_MAX_ORDER:
		diff |= atomic_long_read(&folio->_rmap_val0) ^ (get_rmap_subid_4(mm, 0) * count);
		diff |= atomic_long_read(&folio->_rmap_val1) ^ (get_rmap_subid_4(mm, 1) * count);
		diff |= atomic_long_read(&folio->_rmap_val2) ^ (get_rmap_subid_4(mm, 2) * count);
		diff |= atomic_long_read(&folio->_rmap_val3) ^ (get_rmap_subid_4(mm, 3) * count);
		break;
	case RMAP_SUBID_3_MIN_ORDER ... RMAP_SUBID_3_MAX_ORDER:
		diff |= atomic_long_read(&folio->_rmap_val0) ^ (get_rmap_subid_3(mm, 0) * count);
		diff |= atomic_long_read(&folio->_rmap_val1) ^ (get_rmap_subid_3(mm, 1) * count);
		diff |= atomic_long_read(&folio->_rmap_val2) ^ (get_rmap_subid_3(mm, 2) * count);
		break;
	case RMAP_SUBID_2_MIN_ORDER ... RMAP_SUBID_2_MAX_ORDER:
		diff |= atomic_long_read(&folio->_rmap_val0) ^ (get_rmap_subid_2(mm, 0) * count);
		diff |= atomic_long_read(&folio->_rmap_val1) ^ (get_rmap_subid_2(mm, 1) * count);
		break;
	default:
		diff |= atomic_long_read(&folio->_rmap_val0) ^ (get_rmap_subid_1(mm) * count);
		break;
	}
	return !diff;
}

bool __folio_large_mapped_shared(struct folio *folio, struct mm_struct *mm)
{
	unsigned long start;
	bool exclusive;
	int mapcount;

	VM_WARN_ON_ONCE(!folio_test_large_rmappable(folio));
	VM_WARN_ON_ONCE(folio_test_hugetlb(folio));

	/*
	 * Livelocking here is unlikely, as the caller already handles the
	 * "obviously shared" cases. If ever an issue and there is too much
	 * concurrent (un)mapping happening (using different page tables), we
	 * could stop earlier and just return "shared".
	 */
	do {
		start = raw_read_atomic_seqcount_begin(&folio->_rmap_atomic_seqcount);
		mapcount = folio_mapcount(folio);
		if (unlikely(mapcount > folio_nr_pages(folio)))
			return true;
		exclusive = __folio_has_large_matching_rmap_val(folio, mapcount, mm);
	} while (raw_read_atomic_seqcount_retry(&folio->_rmap_atomic_seqcount,
						start));

	return !exclusive;
}

int alloc_rmap_id(void)
{
	int id;

	/*
	 * We cannot use a mutex, because free_rmap_id() might get called
	 * when we are not allowed to sleep.
	 *
	 * TODO: do we need something like idr_preload()?
	 */
	spin_lock(&rmap_id_lock);
	id = ida_alloc_range(&rmap_ida, RMAP_ID_MIN, RMAP_ID_MAX, GFP_ATOMIC);
	spin_unlock(&rmap_id_lock);

	return id;
}

void free_rmap_id(int id)
{
	if (id == RMAP_ID_DUMMY)
		return;
	if (WARN_ON_ONCE(id < RMAP_ID_MIN || id > RMAP_ID_MAX))
		return;
	spin_lock(&rmap_id_lock);
	ida_free(&rmap_ida, id);
	spin_unlock(&rmap_id_lock);
}
