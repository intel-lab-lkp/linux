// SPDX-License-Identifier: GPL-2.0+

/*
 * Copyright (c) 2024, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/slab.h>
#include <linux/sched/mm.h>
#include <linux/hugetlb.h>
#include <linux/pagewalk.h>
#include <linux/sched/clock.h>
#include <linux/oom.h>

#undef pr_fmt
#define pr_fmt(fmt) "Page Detective: " fmt

/*
 * Walk 4T of VA space at a time, in order to periodically release the mmap
 * lock
 */
#define PD_WALK_MAX_RANGE	BIT(42)

/* Synchronizes writes to virt and phys files */
static DEFINE_MUTEX(page_detective_mutex);
static struct dentry *page_detective_debugfs_dir;

static void page_detective_memcg(struct folio *folio)
{
	struct mem_cgroup *memcg;

	if (!folio_try_get(folio))
		return;

	memcg = get_mem_cgroup_from_folio(folio);
	if (memcg) {
		pr_info("memcg:");
		do {
			pr_cont(" [");
			pr_cont_cgroup_path(memcg->css.cgroup);
			pr_cont(" ]");
		} while ((memcg = parent_mem_cgroup(memcg)));
		mem_cgroup_put(memcg);
		pr_cont("\n");
	}
	folio_put(folio);
}

static void page_detective_metadata(unsigned long pfn)
{
	struct folio *folio = pfn_folio(pfn);
	bool hugetlb, trans;
	unsigned int order;

	if (!folio) {
		pr_info("metadata for pfn[%lx] not found\n", pfn);
		return;
	}

	trans = folio_test_large(folio) && folio_test_large_rmappable(folio);
	hugetlb = folio_test_hugetlb(folio);
	order = folio_order(folio);

	pr_info("metadata for %s pfn[%lx] folio[%px] order [%u]\n",
		(trans) ? "Transparent Huge Page" : (hugetlb) ? "HugeTLB" :
		"Small Page", pfn, folio, order);
	dump_page_lvl(KERN_INFO pr_fmt(""), &folio->page);
	page_detective_memcg(folio);
}

struct pd_private_kernel {
	unsigned long pfn;
	unsigned long direct_map_addr;
	bool direct_map;
	unsigned long vmalloc_maps;
	long maps;
};

#define ENTRY_NAME(entry_page_size) ({					\
	unsigned long __entry_page_size = (entry_page_size);		\
									\
	(__entry_page_size == PUD_SIZE) ? "pud" :			\
	(__entry_page_size == PMD_SIZE) ? "pmd" : "pte";		\
})

static void pd_print_entry_kernel(struct pd_private_kernel *pr,
				  unsigned long pfn_current,
				  unsigned long addr,
				  unsigned long entry_page_size,
				  unsigned long entry)
{
	unsigned long pfn = pr->pfn;

	if (pfn_current <= pfn &&
	    pfn < (pfn_current + (entry_page_size >> PAGE_SHIFT))) {
		bool v, d;

		addr += ((pfn << PAGE_SHIFT) & (entry_page_size - 1));
		v = (addr >= VMALLOC_START && addr < VMALLOC_END);
		d = (pr->direct_map_addr == addr);

		if (v) {
			pr_info("The page is mapped in vmalloc addr[%lx] %s entry[%lx]\n",
				addr, ENTRY_NAME(entry_page_size), entry);
			pr->vmalloc_maps++;
		} else if (d) {
			pr_info("The page is direct mapped addr[%lx] %s entry[%lx]\n",
				addr, ENTRY_NAME(entry_page_size), entry);
			pr->direct_map = true;
		} else {
			pr_info("The page is mapped into kernel addr[%lx] %s entry[%lx]\n",
				addr, ENTRY_NAME(entry_page_size), entry);
		}

		pr->maps++;
	}
}

static int pd_pud_entry_kernel(pud_t *pud, unsigned long addr,
			       unsigned long next,
			       struct mm_walk *walk)
{
	pud_t pudval = READ_ONCE(*pud);

	cond_resched();
	if (!pud_leaf(pudval))
		return 0;

	pd_print_entry_kernel(walk->private, pud_pfn(pudval), addr,
			      PUD_SIZE, pud_val(pudval));

	return 0;
}

static int pd_pmd_entry_kernel(pmd_t *pmd, unsigned long addr,
			       unsigned long next,
			       struct mm_walk *walk)
{
	pmd_t pmdval = READ_ONCE(*pmd);

	cond_resched();
	if (!pmd_leaf(pmdval))
		return 0;

	pd_print_entry_kernel(walk->private, pmd_pfn(pmdval), addr,
			      PMD_SIZE, pmd_val(pmdval));

	return 0;
}

static int pd_pte_entry_kernel(pte_t *pte, unsigned long addr,
			       unsigned long next,
			       struct mm_walk *walk)
{
	pte_t pteval = READ_ONCE(*pte);

	pd_print_entry_kernel(walk->private, pte_pfn(pteval), addr,
			      PAGE_SIZE, pte_val(pteval));

	return 0;
}

static const struct mm_walk_ops pd_kernel_ops = {
	.pud_entry = pd_pud_entry_kernel,
	.pmd_entry = pd_pmd_entry_kernel,
	.pte_entry = pd_pte_entry_kernel,
	.walk_lock = PGWALK_RDLOCK
};

/*
 * Walk kernel page table, and print all mappings to this pfn, return 1 if
 * pfn is mapped in direct map, return 0 if not mapped in direct map, and
 * return -1 if operation canceled by user.
 */
static int page_detective_kernel_map_info(unsigned long pfn,
					  unsigned long direct_map_addr)
{
	struct pd_private_kernel pr = {0};
	unsigned long s, e;

	pr.direct_map_addr = direct_map_addr;
	pr.pfn = pfn;

	for (s = PAGE_OFFSET; s != ~0ul; ) {
		e = s + PD_WALK_MAX_RANGE;
		if (e < s)
			e = ~0ul;

		if (walk_page_range_kernel(s, e, &pd_kernel_ops, &pr)) {
			pr_info("Received a cancel signal from user, while scanning kernel mappings\n");
			return -1;
		}
		cond_resched();
		s = e;
	}

	if (!pr.vmalloc_maps) {
		pr_info("The page is not mapped into kernel vmalloc area\n");
	} else if (pr.vmalloc_maps > 1) {
		pr_info("The page is mapped into vmalloc area: %ld times\n",
			pr.vmalloc_maps);
	}

	if (!pr.direct_map)
		pr_info("The page is not mapped into kernel direct map\n");

	pr_info("The page mapped into kernel page table: %ld times\n", pr.maps);

	return pr.direct_map ? 1 : 0;
}

/* Print kernel information about the pfn, return -1 if canceled by user */
static int page_detective_kernel(unsigned long pfn)
{
	unsigned long *mem = __va((pfn) << PAGE_SHIFT);
	unsigned long sum = 0;
	int direct_map;
	u64 s, e;
	int i;

	s = sched_clock();
	direct_map = page_detective_kernel_map_info(pfn, (unsigned long)mem);
	e = sched_clock() - s;
	pr_info("Scanned kernel page table in [%llu.%09llus]\n",
		e / NSEC_PER_SEC, e % NSEC_PER_SEC);

	/* Canceled by user or no direct map */
	if (direct_map < 1)
		return direct_map;

	for (i = 0; i < PAGE_SIZE / sizeof(unsigned long); i++)
		sum |= mem[i];

	if (sum == 0)
		pr_info("The page contains only zeroes\n");
	else
		pr_info("The page contains some data\n");

	return 0;
}

static char __vma_name[PATH_MAX];
static const char *vma_name(struct vm_area_struct *vma)
{
	const struct path *path;
	const char *name_fmt, *name;

	get_vma_name(vma, &path, &name, &name_fmt);

	if (path) {
		name = d_path(path, __vma_name, PATH_MAX);
		if (IS_ERR(name)) {
			strscpy(__vma_name, "[???]", PATH_MAX);
			goto out;
		}
	} else if (name || name_fmt) {
		snprintf(__vma_name, PATH_MAX, name_fmt ?: "%s", name);
	} else {
		if (vma_is_anonymous(vma))
			strscpy(__vma_name, "[anon]", PATH_MAX);
		else if (vma_is_fsdax(vma))
			strscpy(__vma_name, "[fsdax]", PATH_MAX);
		else if (vma_is_dax(vma))
			strscpy(__vma_name, "[dax]", PATH_MAX);
		else
			strscpy(__vma_name, "[other]", PATH_MAX);
	}

out:
	return __vma_name;
}

static void pd_show_vma_info(struct mm_struct *mm, unsigned long addr)
{
	struct vm_area_struct *vma = find_vma(mm, addr);

	if (!vma) {
		pr_info("vma not found for this mapping\n");
		return;
	}

	pr_info("vma[%px] start[%lx] end[%lx] flags[%016lx] name: %s\n",
		vma, vma->vm_start, vma->vm_end, vma->vm_flags, vma_name(vma));
}

static void pd_get_comm_pid(struct mm_struct *mm, char *comm, int *pid)
{
	struct task_struct *task;

	rcu_read_lock();
	task = rcu_dereference(mm->owner);
	if (task) {
		strscpy(comm, task->comm, TASK_COMM_LEN);
		*pid = task->pid;
	} else {
		strscpy(comm, "__ exited __", TASK_COMM_LEN);
		*pid = -1;
	}
	rcu_read_unlock();
}

struct pd_private_user {
	struct mm_struct *mm;
	unsigned long pfn;
	long maps;
};

static void pd_print_entry_user(struct pd_private_user *pr,
				unsigned long pfn_current,
				unsigned long addr,
				unsigned long entry_page_size,
				unsigned long entry,
				bool is_hugetlb)
{
	unsigned long pfn = pr->pfn;

	if (pfn_current <= pfn &&
	    pfn < (pfn_current + (entry_page_size >> PAGE_SHIFT))) {
		char comm[TASK_COMM_LEN];
		int pid;

		pd_get_comm_pid(pr->mm, comm, &pid);
		addr += ((pfn << PAGE_SHIFT) & (entry_page_size - 1));
		pr_info("%smapped by PID[%d] cmd[%s] mm[%px] pgd[%px] at addr[%lx] %s[%lx]\n",
			is_hugetlb ? "hugetlb " : "",
			pid, comm, pr->mm, pr->mm->pgd, addr,
			ENTRY_NAME(entry_page_size), entry);
		pd_show_vma_info(pr->mm, addr);
		pr->maps++;
	}
}

static int pd_pud_entry_user(pud_t *pud, unsigned long addr, unsigned long next,
			     struct mm_walk *walk)
{
	pud_t pudval = READ_ONCE(*pud);

	cond_resched();
	if (!pud_user_accessible_page(pudval))
		return 0;

	pd_print_entry_user(walk->private, pud_pfn(pudval), addr, PUD_SIZE,
			    pud_val(pudval), false);
	walk->action = ACTION_CONTINUE;

	return 0;
}

static int pd_pmd_entry_user(pmd_t *pmd, unsigned long addr, unsigned long next,
			     struct mm_walk *walk)
{
	pmd_t pmdval = READ_ONCE(*pmd);

	cond_resched();
	if (!pmd_user_accessible_page(pmdval))
		return 0;

	pd_print_entry_user(walk->private, pmd_pfn(pmdval), addr, PMD_SIZE,
			    pmd_val(pmdval), false);
	walk->action = ACTION_CONTINUE;

	return 0;
}

static int pd_pte_entry_user(pte_t *pte, unsigned long addr, unsigned long next,
			     struct mm_walk *walk)
{
	pte_t pteval = READ_ONCE(*pte);

	if (!pte_user_accessible_page(pteval))
		return 0;

	pd_print_entry_user(walk->private, pte_pfn(pteval), addr, PAGE_SIZE,
			    pte_val(pteval), false);
	walk->action = ACTION_CONTINUE;

	return 0;
}

static int pd_hugetlb_entry(pte_t *pte, unsigned long hmask, unsigned long addr,
			    unsigned long next, struct mm_walk *walk)
{
	pte_t pteval = READ_ONCE(*pte);

	cond_resched();
	pd_print_entry_user(walk->private, pte_pfn(pteval), addr, next - addr,
			    pte_val(pteval), true);
	walk->action = ACTION_CONTINUE;

	return 0;
}

static const struct mm_walk_ops pd_user_ops = {
	.pud_entry = pd_pud_entry_user,
	.pmd_entry = pd_pmd_entry_user,
	.pte_entry = pd_pte_entry_user,
	.hugetlb_entry = pd_hugetlb_entry,
	.walk_lock = PGWALK_RDLOCK
};

/*
 * print information about mappings of pfn by mm, return -1 if canceled
 * return number of mappings found.
 */
static long page_detective_user_mm_info(struct mm_struct *mm, unsigned long pfn)
{
	struct pd_private_user pr = {0};
	unsigned long s, e;

	pr.pfn = pfn;
	pr.mm = mm;

	for (s = 0; s != TASK_SIZE; ) {
		e = s + PD_WALK_MAX_RANGE;
		if (e > TASK_SIZE || e < s)
			e = TASK_SIZE;

		if (mmap_read_lock_killable(mm)) {
			pr_info("Received a cancel signal from user, while scanning user mappings\n");
			return -1;
		}
		walk_page_range(mm, s, e, &pd_user_ops, &pr);
		mmap_read_unlock(mm);
		cond_resched();
		s = e;
	}
	return pr.maps;
}

/*
 * Report where/if PFN is mapped in user page tables, return -1 if canceled
 * by user.
 */
static int page_detective_usermaps(unsigned long pfn)
{
	struct task_struct *task, *t;
	struct mm_struct **mm_table, *mm;
	unsigned long proc_nr, mm_nr, i;
	bool canceled_by_user;
	long maps, ret;
	u64 s, e;

	s = sched_clock();
	/* Get the number of processes currently running */
	proc_nr = 0;
	rcu_read_lock();
	for_each_process(task)
		proc_nr++;
	rcu_read_unlock();

	/* Allocate mm_table to fit mm from every running process */
	mm_table = kvmalloc_array(proc_nr, sizeof(struct mm_struct *),
				  GFP_KERNEL);

	if (!mm_table) {
		pr_info("No memory to traverse though user mappings\n");
		return 0;
	}

	/* get mm from every processes and copy its pointer into mm_table */
	mm_nr = 0;
	rcu_read_lock();
	for_each_process(task) {
		if (mm_nr == proc_nr) {
			pr_info("Number of processes increased while scanning, some will be skipped\n");
			break;
		}

		t = find_lock_task_mm(task);
		if (!t)
			continue;

		mm = task->mm;
		if (!mm || !mmget_not_zero(mm)) {
			task_unlock(t);
			continue;
		}
		task_unlock(t);

		mm_table[mm_nr++] = mm;
	}
	rcu_read_unlock();

	/* Walk through every user page table,release mm reference afterwards */
	canceled_by_user = false;
	maps = 0;
	for (i = 0; i < mm_nr; i++) {
		if (!canceled_by_user) {
			ret = page_detective_user_mm_info(mm_table[i], pfn);
			if (ret == -1)
				canceled_by_user = true;
			else
				maps += ret;
		}
		mmput(mm_table[i]);
		cond_resched();
	}

	kvfree(mm_table);

	e = sched_clock() - s;
	pr_info("Scanned [%ld] user page tables in [%llu.%09llus]\n",
		mm_nr, e / NSEC_PER_SEC, e % NSEC_PER_SEC);
	pr_info("The page mapped into user page tables: %ld times\n", maps);

	return canceled_by_user ? -1 : 0;
}

static void page_detective_iommu(unsigned long pfn)
{
}

static void page_detective_tdp(unsigned long pfn)
{
}

static void page_detective(unsigned long pfn)
{
	if (!pfn_valid(pfn)) {
		pr_info("pfn[%lx] is invalid\n", pfn);
		return;
	}

	if (pfn == 0) {
		pr_info("Skipping look-up for pfn[0] mapped many times into kernel page table\n");
		return;
	}

	/* Report metadata information */
	page_detective_metadata(pfn);

	/*
	 * Report information about kernel mappings, and basic content
	 * information: i.e. all zero or not.
	 */
	if (page_detective_kernel(pfn) < 0)
		return;

	/* Report where/if PFN is mapped in user page tables */
	if (page_detective_usermaps(pfn) < 0)
		return;

	/* Report where/if PFN is mapped in IOMMU page tables */
	page_detective_iommu(pfn);

	/* Report where/if PFN is mapped in 2 dimensional paging */
	page_detective_tdp(pfn);
}

static u64 pid_virt_to_phys(unsigned int pid, unsigned long virt_addr)
{
	unsigned long phys_addr = -1;
	struct task_struct *task;
	struct mm_struct *mm;
	pgd_t *pgd, pgdval;
	p4d_t *p4d, p4dval;
	pud_t *pud, pudval;
	pmd_t *pmd, pmdval;
	pte_t *pte, pteval;

	if (virt_addr >= TASK_SIZE) {
		pr_err("%s: virt_addr[%lx] is above TASK_SIZE[%lx]\n",
		       __func__, virt_addr, TASK_SIZE);
		return -1;
	}

	/* Find the task_struct using the PID */
	task = find_get_task_by_vpid(pid);
	if (!task) {
		pr_err("%s: Task not found for PID %d\n", __func__, pid);
		return -1;
	}

	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm) {
		pr_err("%s: PID %d, can't get mm reference\n", __func__, pid);
		return -1;
	}

	if (mmap_read_lock_killable(mm)) {
		pr_info("Received a cancel signal from user, while convirting virt to phys\n");
		mmput(mm);
		return -1;
	}

	pgd = pgd_offset(mm, virt_addr);
	pgdval = READ_ONCE(*pgd);
	if (!pgd_present(pgdval) || unlikely(pgd_bad(pgdval))) {
		pr_err("%s: pgd[%llx] present[%d] bad[%d]\n", __func__,
		       (u64)pgd_val(pgdval), pgd_present(pgdval),
		       pgd_bad(pgdval));
		goto putmm_exit;
	}

	p4d = p4d_offset(pgd, virt_addr);
	p4dval = READ_ONCE(*p4d);
	if (!p4d_present(p4dval) || unlikely(p4d_bad(p4dval))) {
		pr_err("%s: p4d[%llx] present[%d] bad[%d]\n", __func__,
		       (u64)p4d_val(p4dval), p4d_present(p4dval),
		       p4d_bad(p4dval));
		goto putmm_exit;
	}

	pud = pud_offset(p4d, virt_addr);
	pudval = READ_ONCE(*pud);
	if (!pud_present(pudval)) {
		pr_err("%s: pud[%llx] present[%d]\n", __func__,
		       (u64)pud_val(pudval), pud_present(pudval));
		goto putmm_exit;
	}

	if (pud_leaf(pudval)) {
		phys_addr = (pud_pfn(pudval) << PAGE_SHIFT)
			| (virt_addr & ~PUD_MASK);
		goto putmm_exit;
	}

	pmd = pmd_offset(pud, virt_addr);
	pmdval = READ_ONCE(*pmd);
	if (!pmd_present(pmdval)) {
		pr_err("%s: pmd[%llx] present[%d]\n", __func__,
		       (u64)pmd_val(pmdval), pmd_present(pmdval));
		goto putmm_exit;
	}

	if (pmd_leaf(pmdval)) {
		phys_addr = (pmd_pfn(pmdval) << PAGE_SHIFT)
			| (virt_addr & ~PMD_MASK);
		goto putmm_exit;
	}

	pte = pte_offset_kernel(pmd, virt_addr);
	pteval = READ_ONCE(*pte);
	if (!pte_present(pteval)) {
		pr_err("%s: pte[%llx] present[%d]\n", __func__,
		       (u64)pte_val(pteval), pte_present(pteval));
		goto putmm_exit;
	}

	phys_addr = pte_pfn(*pte) << PAGE_SHIFT;

putmm_exit:
	mmap_read_unlock(mm);
	mmput(mm);
	return phys_addr;
}

static ssize_t page_detective_virt_write(struct file *file,
					 const char __user *data,
					 size_t count, loff_t *ppos)
{
	char *input_str, *pid_str, *virt_str;
	unsigned int pid, err, i;
	unsigned long virt_addr;
	u64 phys_addr;

	/* If canceled by user simply return without printing anything */
	err = mutex_lock_killable(&page_detective_mutex);
	if (err)
		return count;

	input_str = kzalloc(count + 1, GFP_KERNEL);
	if (!input_str) {
		pr_err("%s: Unable to allocate input_str buffer\n",
		       __func__);
		mutex_unlock(&page_detective_mutex);
		return -EAGAIN;
	}

	if (copy_from_user(input_str, data, count)) {
		kfree(input_str);
		pr_err("%s: Unable to copy user input into virt file\n",
		       __func__);
		mutex_unlock(&page_detective_mutex);
		return -EFAULT;
	}

	virt_str = NULL;
	pid_str = input_str;
	for (i = 0; i < count - 1; i++) {
		if (isspace(input_str[i])) {
			input_str[i] = '\0';
			virt_str = &input_str[i + 1];
			break;
		}
	}

	if (!virt_str) {
		kfree(input_str);
		pr_err("%s: Invalid virt file input, should be: '<pid> <virtual address>'\n",
		       __func__);
		mutex_unlock(&page_detective_mutex);
		return -EINVAL;
	}

	err = kstrtouint(pid_str, 0, &pid);
	if (err) {
		kfree(input_str);
		pr_err("%s: Failed to parse pid\n", __func__);
		mutex_unlock(&page_detective_mutex);
		return err;
	}

	err = kstrtoul(virt_str, 0, &virt_addr);
	if (err) {
		kfree(input_str);
		pr_err("%s: Failed to parse virtual address\n", __func__);
		mutex_unlock(&page_detective_mutex);
		return err;
	}

	kfree(input_str);

	phys_addr = pid_virt_to_phys(pid, virt_addr);
	if (phys_addr == -1) {
		pr_err("%s: Can't translate virtual to physical address\n",
		       __func__);
		mutex_unlock(&page_detective_mutex);
		return -EINVAL;
	}

	pr_info("Investigating pid[%u] virtual[%lx] physical[%llx] pfn[%lx]\n",
		pid, virt_addr, phys_addr, PHYS_PFN(phys_addr));
	page_detective(PHYS_PFN(phys_addr));
	pr_info("Finished investigation of virtual[%lx]\n", virt_addr);
	mutex_unlock(&page_detective_mutex);

	return count;
}

static ssize_t page_detective_phys_write(struct file *file,
					 const char __user *data,
					 size_t count, loff_t *ppos)
{
	u64 phys_addr;
	int err;

	/* If canceled by user simply return without printing anything */
	err = mutex_lock_killable(&page_detective_mutex);
	if (err)
		return count;

	err = kstrtou64_from_user(data, count, 0, &phys_addr);

	if (err) {
		pr_err("%s: Failed to parse physical address\n", __func__);
		mutex_unlock(&page_detective_mutex);
		return err;
	}

	pr_info("Investigating physical[%llx] pfn[%lx]\n", phys_addr,
		PHYS_PFN(phys_addr));
	page_detective(PHYS_PFN(phys_addr));
	pr_info("Finished investigation of physical[%llx]\n", phys_addr);
	mutex_unlock(&page_detective_mutex);

	return count;
}

static int page_detective_open(struct inode *inode, struct file *file)
{
	/* Deny access if not CAP_SYS_ADMIN */
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	return simple_open(inode, file);
}

static const struct file_operations page_detective_virt_fops = {
	.owner = THIS_MODULE,
	.open = page_detective_open,
	.write = page_detective_virt_write,
};

static const struct file_operations page_detective_phys_fops = {
	.owner = THIS_MODULE,
	.open = page_detective_open,
	.write = page_detective_phys_write,
};

static int __init page_detective_init(void)
{
	page_detective_debugfs_dir = debugfs_create_dir("page_detective", NULL);

	debugfs_create_file("virt", 0200, page_detective_debugfs_dir, NULL,
			    &page_detective_virt_fops);
	debugfs_create_file("phys", 0200, page_detective_debugfs_dir, NULL,
			    &page_detective_phys_fops);

	return 0;
}
module_init(page_detective_init);

static void page_detective_exit(void)
{
	debugfs_remove_recursive(page_detective_debugfs_dir);
}
module_exit(page_detective_exit);

MODULE_DESCRIPTION("Page Detective");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pasha Tatashin <pasha.tatashin@soleen.com>");
