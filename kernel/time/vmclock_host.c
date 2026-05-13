// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/vmclock_host - Expose host NTP-disciplined time as a vmclock page.
 *
 * This provides a vmclock_abi structure populated from the host's
 * CLOCK_REALTIME (TAI), allowing a VMM to efficiently relay precision
 * time to guests without per-tick overhead.
 *
 * The page is updated only when the NTP frequency (ntp_tick) changes
 * or the clocksource changes — not on every timekeeping tick.
 * Userspace can poll() for changes.
 *
 * Copyright © 2026 Amazon.com, Inc. or its affiliates.
 */

#include <linux/clocksource_ids.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/timekeeper_internal.h>
#include <linux/wait.h>

#include <uapi/linux/vmclock-abi.h>

extern void (*vmclock_host_update_fn)(struct timekeeper *tk);
extern bool ntp_synced(void);

static struct vmclock_abi *vmclock_page;
static DECLARE_WAIT_QUEUE_HEAD(vmclock_wait);
static u64 vmclock_last_ntp_tick = 1; /* Sentinel: force first update */
static enum clocksource_ids vmclock_last_cs_id;

/*
 * Compute counter_period_frac_sec from ntp_tick and cycle_interval.
 *
 * ntp_tick is ns_per_tick << 32.
 * cycle_interval is counter cycles per tick.
 *
 * vmclock wants: period = frac_sec / 2^(64 + shift) in seconds.
 *
 * ns_per_cycle = ntp_tick / cycle_interval (in <<32 fixed point)
/*
 * Compute counter_period_frac_sec from ntp_tick and cycle_interval.
 *
 * period = ntp_tick / (cycle_interval * 10^9 * 2^32) seconds/cycle
 * frac_sec = ntp_tick * 2^(32+shift) / (cycle_interval * 10^9)
 *
 * Use div64_u64 with maximum pre-shift for precision.
 * The key: do TWO divisions to get 64 bits of quotient.
 */
static void vmclock_compute_period(struct timekeeper *tk,
				   u64 *period_frac, u8 *period_shift)
{
	u64 ntp_tick = tk->ntp_tick;
	u64 cycle_interval = tk->cycle_interval;
	u64 divisor = cycle_interval * 1000000000ULL;
	int headroom = __builtin_clzll(ntp_tick);
	u64 rem, result;
	int bits_so_far, need;

	/*
	 * Compute ntp_tick * 2^(headroom + N) / divisor with 64 bits
	 * of precision, using iterative 32-bit chunk divisions.
	 *
	 * First division: ntp_tick << headroom / divisor
	 */
	result = div64_u64_rem(ntp_tick << headroom, divisor, &rem);
	bits_so_far = 64 - __builtin_clzll(result ?: 1);

	/* Fill remaining bits 32 at a time from the remainder */
	while (bits_so_far < 64 && rem) {
		int chunk = min(32, 64 - bits_so_far);
		int rem_headroom = __builtin_clzll(rem);
		u64 extra;

		if (rem_headroom < chunk)
			chunk = rem_headroom;

		extra = div64_u64_rem(rem << chunk, divisor, &rem);
		result = (result << chunk) | extra;
		bits_so_far += chunk;
		headroom += chunk;
	}

	/* Pad with zeros if we ran out of remainder */
	if (bits_so_far < 64) {
		result <<= (64 - bits_so_far);
		headroom += (64 - bits_so_far);
	}

	/*
	 * result = ntp_tick * 2^headroom / divisor
	 *        = (ntp_tick / (cycle_interval * 10^9)) * 2^headroom
	 *        = period_seconds * 2^32 * 2^headroom
	 *        = period_seconds * 2^(32 + headroom)
	 *
	 * vmclock: frac_sec / 2^(64 + shift) = period_seconds
	 * So: shift = 32 + headroom - 64 = headroom - 32
	 */
	*period_frac = result;
	*period_shift = (u8)(headroom - 32);
}


static u8 vmclock_counter_id(struct timekeeper *tk)
{
	enum clocksource_ids id = tk->cs_id;

	if (IS_ENABLED(CONFIG_X86) && id == CSID_X86_TSC)
		return VMCLOCK_COUNTER_X86_TSC;
	if (IS_ENABLED(CONFIG_ARM64) && id == CSID_ARM_ARCH_COUNTER)
		return VMCLOCK_COUNTER_ARM_VCNT;
	return VMCLOCK_COUNTER_INVALID;
}

/*
 * Called from timekeeping_adjust() when ntp_tick changes.
 * Also needs to be called on clocksource change.
 */
static void vmclock_host_do_update(struct timekeeper *tk)
{
	struct vmclock_abi *clk = vmclock_page;
	u64 period_frac;
	u8 period_shift, counter_id;

	if (!clk)
		return;

	counter_id = vmclock_counter_id(tk);

	/* Only do a full update when something meaningful changes */
	if (tk->ntp_tick == vmclock_last_ntp_tick &&
	    tk->cs_id == vmclock_last_cs_id)
		return;

	vmclock_last_ntp_tick = tk->ntp_tick;
	vmclock_last_cs_id = tk->cs_id;

	/* Increment seq_count to odd (update in progress) */
	WRITE_ONCE(clk->seq_count, cpu_to_le32(le32_to_cpu(clk->seq_count) + 1));
	smp_wmb();

	clk->counter_id = counter_id;

	if (counter_id != VMCLOCK_COUNTER_INVALID) {
		u64 ns = tk->tkr_mono.xtime_nsec >> tk->tkr_mono.shift;
		u64 hi, rem;

		/* Adjust for ntp_error: represent where the clock is
		 * converging TO, not where it is right now. */
		ns += tk->ntp_error >> (tk->tkr_mono.shift + tk->ntp_error_shift);

		clk->counter_value = cpu_to_le64(tk->tkr_mono.cycle_last);
		clk->time_sec = cpu_to_le64(tk->xtime_sec + tk->tai_offset);

		hi = div64_u64_rem(ns << 32, 1000000000ULL, &rem);
		clk->time_frac_sec = cpu_to_le64(
			(hi << 32) | div64_u64(rem << 32, 1000000000ULL));

		vmclock_compute_period(tk,
				       &period_frac, &period_shift);
		clk->counter_period_frac_sec = cpu_to_le64(period_frac);
		clk->counter_period_shift = period_shift;

		clk->clock_status = ntp_synced() ?
			VMCLOCK_STATUS_SYNCHRONIZED :
			VMCLOCK_STATUS_FREERUNNING;
	} else {
		clk->clock_status = VMCLOCK_STATUS_UNKNOWN;
	}

	clk->tai_offset_sec = cpu_to_le16((s16)tk->tai_offset);
	clk->flags = cpu_to_le64(VMCLOCK_FLAG_TAI_OFFSET_VALID |
				 VMCLOCK_FLAG_TIME_MONOTONIC |
				 VMCLOCK_FLAG_NOTIFICATION_PRESENT);

	smp_wmb();
	WRITE_ONCE(clk->seq_count, cpu_to_le32(le32_to_cpu(clk->seq_count) + 1));

	wake_up_interruptible(&vmclock_wait);
}

/* File operations */

struct vmclock_host_file {
	u32 last_seq;
};

static int vmclock_host_open(struct inode *inode, struct file *fp)
{
	struct vmclock_host_file *fst;

	fst = kzalloc(sizeof(*fst), GFP_KERNEL);
	if (!fst)
		return -ENOMEM;

	fp->private_data = fst;
	return 0;
}

static int vmclock_host_release(struct inode *inode, struct file *fp)
{
	kfree(fp->private_data);
	return 0;
}

static int vmclock_host_mmap(struct file *fp, struct vm_area_struct *vma)
{
	if ((vma->vm_flags & (VM_READ | VM_WRITE)) != VM_READ)
		return -EROFS;

	if (vma->vm_end - vma->vm_start != PAGE_SIZE || vma->vm_pgoff)
		return -EINVAL;

	return remap_pfn_range(vma, vma->vm_start,
			       virt_to_phys(vmclock_page) >> PAGE_SHIFT,
			       PAGE_SIZE, vma->vm_page_prot);
}

static ssize_t vmclock_host_read(struct file *fp, char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct vmclock_host_file *fst = fp->private_data;
	u32 seq;

	if (*ppos >= PAGE_SIZE)
		return 0;
	if (count > PAGE_SIZE - *ppos)
		count = PAGE_SIZE - *ppos;

	do {
		seq = le32_to_cpu(READ_ONCE(vmclock_page->seq_count));
		if (seq & 1) {
			cpu_relax();
			continue;
		}
		smp_rmb();
		if (copy_to_user(buf, (char *)vmclock_page + *ppos, count))
			return -EFAULT;
		smp_rmb();
	} while (le32_to_cpu(READ_ONCE(vmclock_page->seq_count)) != seq);

	fst->last_seq = seq;
	*ppos += count;
	return count;
}

static __poll_t vmclock_host_poll(struct file *fp, poll_table *wait)
{
	struct vmclock_host_file *fst = fp->private_data;
	u32 seq;

	poll_wait(fp, &vmclock_wait, wait);

	seq = le32_to_cpu(READ_ONCE(vmclock_page->seq_count));
	if (fst->last_seq != seq)
		return EPOLLIN | EPOLLRDNORM;

	return 0;
}

static const struct file_operations vmclock_host_fops = {
	.owner = THIS_MODULE,
	.open = vmclock_host_open,
	.release = vmclock_host_release,
	.mmap = vmclock_host_mmap,
	.read = vmclock_host_read,
	.poll = vmclock_host_poll,
};

static struct miscdevice vmclock_host_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "vmclock_host",
	.fops = &vmclock_host_fops,
};

static int __init vmclock_host_init(void)
{
	int ret;

	vmclock_page = (struct vmclock_abi *)get_zeroed_page(GFP_KERNEL);
	if (!vmclock_page)
		return -ENOMEM;

	/* Set constant fields */
	vmclock_page->magic = cpu_to_le32(VMCLOCK_MAGIC);
	vmclock_page->size = cpu_to_le32(PAGE_SIZE);
	vmclock_page->version = cpu_to_le16(1);
	vmclock_page->time_type = VMCLOCK_TIME_TAI;

	ret = misc_register(&vmclock_host_miscdev);
	if (ret) {
		free_page((unsigned long)vmclock_page);
		vmclock_page = NULL;
		return ret;
	}

	WRITE_ONCE(vmclock_host_update_fn, vmclock_host_do_update);
	pr_info("vmclock_host: registered /dev/vmclock_host\n");
	return 0;
}

static void __exit vmclock_host_exit(void)
{
	WRITE_ONCE(vmclock_host_update_fn, NULL);
	synchronize_rcu();
	misc_deregister(&vmclock_host_miscdev);
	free_page((unsigned long)vmclock_page);
	vmclock_page = NULL;
}

module_init(vmclock_host_init);
module_exit(vmclock_host_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Woodhouse <dwmw@amazon.co.uk>");
MODULE_DESCRIPTION("VMClock host time provider");
