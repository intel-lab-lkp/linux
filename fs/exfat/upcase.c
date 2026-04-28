// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/buffer_head.h>

#include "exfat_raw.h"
#include "exfat_fs.h"

int exfat_set_upcase_ptable(struct exfat_upcase_ptable *ptbl,
		const __u16 index, const __u16 value)
{
	const size_t page_idx = index / EXFAT_UPTBL_PAGESIZE;
	const size_t idx_in_page = index % EXFAT_UPTBL_PAGESIZE;

	if (value == 0)
		return 0;

	if (ptbl->pages[page_idx] == NULL) {
		void *nm = kvcalloc(EXFAT_UPTBL_PAGESIZE, sizeof(__u16), GFP_KERNEL);

		if (nm == NULL)
			return -ENOMEM;
		ptbl->pages[page_idx] = nm;
		ptbl->cnt++;
	}

	ptbl->pages[page_idx][idx_in_page] = value;

	return 0;
}

void exfat_free_upcase_ptable(struct exfat_upcase_ptable *ptbl)
{
	if (ptbl == NULL)
		return;

	for (size_t i = 0; i < ARRAY_SIZE(ptbl->pages); i++) {
		kvfree(ptbl->pages[i]);
		ptbl->pages[i] = NULL;
	}
	ptbl->cnt = 0;
}

int exfat_populate_upcase_ptable(struct exfat_upcase_ptable *ptbl,
		const struct exfat_upcase_range_info *ri,
		const size_t cnt)
{
	int err;

	for (size_t i = 0; i < cnt; i++) {
		/* Memory safety: allow the value to wrap around but not the index */
		const unsigned int step = ri[i].inc;
		unsigned int index = ri[i].start;
		__u16 value = ri[i].value;

		if (step == 0 || index >= ri[i].end)
			/* Damaged .rodata */
			return -EINVAL;

		while (index < ri[i].end) {
			err = exfat_set_upcase_ptable(ptbl, index, value);
			if (err)
				return err;

			index += step;
			value += step;
		}
	}

	return 0;
}
