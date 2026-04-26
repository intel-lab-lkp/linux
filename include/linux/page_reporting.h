/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGE_REPORTING_H
#define _LINUX_PAGE_REPORTING_H

#include <linux/mmzone.h>
#include <linux/scatterlist.h>

/* This value should always be a power of 2, see page_reporting_cycle() */
#define PAGE_REPORTING_CAPACITY		32

struct page_reporting_dev_info {
	/* function that alters pages to make them "reported" */
	int (*report)(struct page_reporting_dev_info *prdev,
		      struct scatterlist *sg, unsigned int nents);

	/* If true, host zeros reported pages on reclaim */
	bool host_zeroes_pages;

	/*
	 * Per-page zeroed status, indexed by scatterlist position.
	 * The driver's report() callback must clear the bitmap,
	 * then set bits for pages that were actually zeroed.
	 */
	DECLARE_BITMAP(zeroed_bitmap, PAGE_REPORTING_CAPACITY);

	/* work struct for processing reports */
	struct delayed_work work;

	/* Current state of page reporting */
	atomic_t state;

	/* Minimal order of page reporting */
	unsigned int order;
};

/* Tear-down and bring-up for page reporting devices */
void page_reporting_unregister(struct page_reporting_dev_info *prdev);
int page_reporting_register(struct page_reporting_dev_info *prdev);
#endif /*_LINUX_PAGE_REPORTING_H */
