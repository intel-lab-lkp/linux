// SPDX-License-Identifier: GPL-2.0
/*
 * self test for change_page_attr.
 *
 * Clears the a test pte bit on random pages in the direct mapping,
 * then reverts and compares page tables forwards and afterwards.
 */
#include <linux/memblock.h>
#include <linux/kthread.h>
#include <linux/random.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>

/*
 * Only print the results of the first pass:
 */
static __read_mostly int print = 1;
static unsigned long max_test_pfn;

enum {
	NTEST			= 3 * 100,
	NPAGES			= 100,
	LPS			= PMD_SIZE,
	GPS			= PUD_SIZE
};

#define PAGE_CPA_TEST	__pgprot(_PAGE_SPECIAL)

static int pte_testbit(pte_t pte)
{
	return pte_special(pte);
}

struct split_state {
	long lpg, gpg, spg, exec;
	long min_exec, max_exec;
};

static int print_split(struct split_state *s)
{
	long i, expected, missed = 0;
	int err = 0;

	s->lpg = s->gpg = s->spg = s->exec = 0;
	s->min_exec = ~0UL;
	s->max_exec = 0;
	for (i = 0; i < max_test_pfn; ) {
		unsigned long addr = (unsigned long)__va(i << PAGE_SHIFT);
		unsigned int level;
		pte_t *pte, pteval;

		pte = lookup_address(addr, &level);
		if (!pte) {
			missed++;
			i++;
			continue;
		}
		pteval = ptep_get(pte);

		if (level == PGTABLE_LEVEL_PUD && sizeof(long) == 8) {
			s->gpg++;
			i += GPS/PAGE_SIZE;
		} else if (level == PGTABLE_LEVEL_PMD) {
			if (pte_present(pteval) && !pte_huge(pteval)) {
				printk(KERN_ERR
					"%lx level %d but not leaf %Lx\n",
					addr, level, (u64)pte_val(pteval));
				err = 1;
			}
			s->lpg++;
			i += LPS/PAGE_SIZE;
		} else {
			s->spg++;
			i++;
		}
		if (pte_exec(pteval)) {
			s->exec++;
			if (addr < s->min_exec)
				s->min_exec = addr;
			if (addr > s->max_exec)
				s->max_exec = addr;
		}
	}
	if (print) {
		printk(KERN_INFO
			" 4k %lu large %lu gb %lu x %lu[%lx-%lx] miss %lu\n",
			s->spg, s->lpg, s->gpg, s->exec,
			s->min_exec != ~0UL ? s->min_exec : 0,
			s->max_exec, missed);
	}

	expected = (s->gpg*GPS + s->lpg*LPS)/PAGE_SIZE + s->spg + missed;
	if (expected != i) {
		printk(KERN_ERR "CPA max_test_pfn %lu but expected %lu\n",
			max_test_pfn, expected);
		return 1;
	}
	return err;
}

static unsigned long addr[NTEST];
static unsigned int len[NTEST];

static struct page *pages[NPAGES];
static unsigned long addrs[NPAGES];

/* Change the global bit on random pages in the direct mapping */
static int pageattr_test(void)
{
	struct split_state sa, sb, sc;
	unsigned long *bm;
	pte_t *pte, pte0, pte_val;
	int failed = 0;
	unsigned int level;
	int i, k;
	int err;

	if (print)
		printk(KERN_INFO "CPA self-test:\n");

	bm = vzalloc((max_test_pfn + 7) / 8);
	if (!bm) {
		printk(KERN_ERR "CPA Cannot vmalloc bitmap\n");
		return -ENOMEM;
	}

	failed += print_split(&sa);

	for (i = 0; i < NTEST; i++) {
		unsigned long pfn = get_random_u32_below(max_test_pfn);

		addr[i] = (unsigned long)__va(pfn << PAGE_SHIFT);
		len[i] = get_random_u32_below(NPAGES);
		len[i] = min_t(unsigned long, len[i], max_test_pfn - pfn - 1);

		if (len[i] == 0)
			len[i] = 1;

		pte = NULL;
		pte0 = pfn_pte(0, __pgprot(0)); /* shut gcc up */

		for (k = 0; k < len[i]; k++) {
			pte = lookup_address(addr[i] + k*PAGE_SIZE, &level);
			if (!pte) {
				addr[i] = 0;
				break;
			}
			pte_val = ptep_get(pte);
			if (pgprot_val(pte_pgprot(pte_val)) == 0 ||
			    !pte_present(pte_val)) {
				addr[i] = 0;
				break;
			}
			if (k == 0) {
				pte0 = pte_val;
			} else {
				if (pgprot_val(pte_pgprot(pte_val)) !=
					pgprot_val(pte_pgprot(pte0))) {
					len[i] = k;
					break;
				}
			}
			if (test_bit(pfn + k, bm)) {
				len[i] = k;
				break;
			}
			__set_bit(pfn + k, bm);
			addrs[k] = addr[i] + k*PAGE_SIZE;
			pages[k] = pfn_to_page(pfn + k);
		}
		if (!addr[i] || !pte || !k) {
			addr[i] = 0;
			continue;
		}

		switch (i % 3) {
		case 0:
			err = change_page_attr_set(&addr[i], len[i], PAGE_CPA_TEST, 0);
			break;

		case 1:
			err = change_page_attr_set(addrs, len[i], PAGE_CPA_TEST, 1);
			break;

		case 2:
			err = cpa_set_pages_array(pages, len[i], PAGE_CPA_TEST);
			break;
		}


		if (err < 0) {
			printk(KERN_ERR "CPA %d failed %d\n", i, err);
			failed++;
		}

		pte = lookup_address(addr[i], &level);
		if (!pte || !pte_testbit(*pte) || level != PGTABLE_LEVEL_PTE) {
			printk(KERN_ERR "CPA %lx: bad pte %Lx\n", addr[i],
				pte ? (u64)pte_val(*pte) : 0ULL);
			failed++;
		}
		if (level != PGTABLE_LEVEL_PTE) {
			printk(KERN_ERR "CPA %lx: unexpected level %d\n",
				addr[i], level);
			failed++;
		}

	}
	vfree(bm);

	failed += print_split(&sb);

	for (i = 0; i < NTEST; i++) {
		if (!addr[i])
			continue;
		pte = lookup_address(addr[i], &level);
		if (!pte) {
			printk(KERN_ERR "CPA lookup of %lx failed\n", addr[i]);
			failed++;
			continue;
		}
		err = change_page_attr_clear(&addr[i], len[i], PAGE_CPA_TEST, 0);
		if (err < 0) {
			printk(KERN_ERR "CPA reverting failed: %d\n", err);
			failed++;
		}
		pte = lookup_address(addr[i], &level);
		if (!pte || pte_testbit(*pte)) {
			printk(KERN_ERR "CPA %lx: bad pte after revert %Lx\n",
				addr[i], pte ? (u64)pte_val(*pte) : 0ULL);
			failed++;
		}

	}

	failed += print_split(&sc);

	if (failed) {
		WARN(1, KERN_ERR "NOT PASSED. Please report.\n");
		return -EINVAL;
	} else {
		if (print)
			printk(KERN_INFO "ok.\n");
	}

	return 0;
}

static int do_pageattr_test(void *__unused)
{
	while (!kthread_should_stop()) {
		schedule_timeout_interruptible(HZ*30);
		if (pageattr_test() < 0)
			break;
		if (print)
			print--;
	}
	return 0;
}

static int start_pageattr_test(void)
{
	struct task_struct *p;

	max_test_pfn = PFN_DOWN(__pa(high_memory - 1));

	p = kthread_create(do_pageattr_test, NULL, "pageattr-test");
	if (!IS_ERR(p))
		wake_up_process(p);
	else
		WARN_ON(1);

	return 0;
}
device_initcall(start_pageattr_test);
