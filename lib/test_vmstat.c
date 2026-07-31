// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test module for in-kernel synthetic vm statistics performance.
 *
 * execute
 *
 *	modprobe test_vmstat
 *
 * to run this test
 *
 * (C) 2009 Linux Foundation, Christoph Lameter <cl@gentwo.org>
 */

#include <linux/jiffies.h>
#include <linux/compiler.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <asm/timex.h>

#define TEST_COUNT 10000

static int vmstat_test_init(void)
{
	unsigned int i;
	cycles_t time1, time2, time;
	int rem;
	struct page *page = alloc_page(GFP_KERNEL);

	pr_alert("VMstat testing\n");
	pr_alert("=====================\n");
	pr_alert("1. inc_zone_page_state() then dec_zone_page_state()\n");
	time1 = get_cycles();
	for (i = 0; i < TEST_COUNT; i++)
		inc_zone_page_state(page, NR_FREE_CMA_PAGES);

	time2 = get_cycles();
	time = time2 - time1;

	pr_alert("%i times inc_zone_page_state() ", i);
	time = div_u64_rem(time, TEST_COUNT, &rem);
	pr_cont("-> %llu cycles ", (unsigned long long) time);

	time1 = get_cycles();
	for (i = 0; i < TEST_COUNT; i++)
		__dec_zone_page_state(page, NR_FREE_CMA_PAGES);

	time2 = get_cycles();
	time = time2 - time1;

	pr_cont("__dec_z_p_s() ");
	time = div_u64_rem(time, TEST_COUNT, &rem);
	pr_cont("-> %llu cycles\n",  (unsigned long long) time);

	pr_alert("2. inc_zone_page_state()/dec_zone_page_state()\n");
	time1 = get_cycles();
	for (i = 0; i < TEST_COUNT; i++) {
		inc_zone_page_state(page, NR_FREE_CMA_PAGES);
		dec_zone_page_state(page, NR_FREE_CMA_PAGES);
	}

	time2 = get_cycles();
	time = time2 - time1;

	pr_alert("%i times inc/dec ", i);
	time = div_u64_rem(time, TEST_COUNT, &rem);
	pr_cont("-> %llu cycles\n",  (unsigned long long) time);

	pr_alert("3. count_vm_event()\n");
	time1 = get_cycles();
	for (i = 0; i < TEST_COUNT; i++)
		count_vm_event(SLABS_SCANNED);

	time2 = get_cycles();
	time = time2 - time1;

	count_vm_events(SLABS_SCANNED, -TEST_COUNT);
	pr_alert("%i count_vm_events ", i);
	time = div_u64_rem(time, TEST_COUNT, &rem);
	pr_cont("-> %llu cycles\n",  (unsigned long long) time);
	__free_page(page);
	return -EAGAIN; /* Fail will directly unload the module */
}

static void vmstat_test_exit(void)
{
	pr_alert("test exit\n");
}

module_init(vmstat_test_init)
module_exit(vmstat_test_exit)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Christoph Lameter");
MODULE_DESCRIPTION("VM statistics test");
