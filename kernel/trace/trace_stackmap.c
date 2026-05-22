// SPDX-License-Identifier: GPL-2.0
/*
 * Ftrace Stack Map - Lock-free stack trace deduplication for ftrace
 *
 * Modeled after tracing_map.c (used by hist triggers), this provides
 * a lock-free hash map optimized for the ftrace hot path. The design
 * is based on Dr. Cliff Click's non-blocking hash table algorithm.
 *
 * Key properties:
 * - Lock-free insert via cmpxchg, safe in NMI/IRQ/any context
 * - Pre-allocated element pool (zero allocation on hot path)
 * - Linear probing with 2x over-provisioned table; probe length
 *   bounded by FTRACE_STACKMAP_MAX_PROBE to keep worst-case lookup
 *   cost constant even when the table is heavily loaded
 * - Single global instance (initialized for the global trace array)
 *
 * Reset is a control-path operation, only allowed when tracing is
 * stopped on the owning trace_array. The protocol is:
 *
 *   - atomic_cmpxchg(&resetting, 0, 1) atomically claims reset rights
 *     and blocks new get_id() callers (they observe resetting=1 and
 *     return -EINVAL).
 *   - tracer_tracing_is_on() is checked AFTER the cmpxchg, so the
 *     resetting flag itself prevents new insertions even if userspace
 *     re-enables tracing immediately after the check.
 *   - synchronize_rcu() drains in-flight get_id() callers from the
 *     ftrace callback path, which runs with preemption disabled.
 *
 * Online reset (with tracing active) is intentionally not supported
 * to keep the design simple and the proof obligations small.
 *
 * The 32-bit jhash of the stack IPs is the hash table key. On hash
 * collision, linear probing finds the next slot and full memcmp
 * confirms the match.
 *
 * Concurrent userspace readers (cat stack_map / stack_map_bin) get
 * a best-effort snapshot. They are coherent with the hot path
 * (smp_load_acquire on entry->val), but they are not coherent with
 * a concurrent reset; since reset requires tracing to be stopped,
 * mid-iteration reset can produce truncated or partial output but
 * never crashes.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/jhash.h>
#include <linux/seq_file.h>
#include <linux/kallsyms.h>
#include <linux/vmalloc.h>
#include <linux/atomic.h>
#include <linux/random.h>
#include <linux/rcupdate.h>
#include <linux/log2.h>

#include "trace.h"
#include "trace_stackmap.h"

/*
 * Bound the linear-probe scan length. With a 2x over-provisioned table,
 * a well-distributed hash gives very short probe chains. Capping at 64
 * keeps worst-case lookup O(1) even when the table is heavily loaded
 * with claimed-but-empty slots from pool exhaustion.
 */
#define FTRACE_STACKMAP_MAX_PROBE	64

/*
 * Each pre-allocated element holds one unique stack trace.
 * Fixed size: MAX_DEPTH entries regardless of actual depth.
 */
struct stackmap_elt {
	u32		nr;		/* actual number of IPs */
	atomic_t	ref_count;
	unsigned long	ips[FTRACE_STACKMAP_MAX_DEPTH];
};

/*
 * Hash table entry: a 32-bit key (jhash of stack) + pointer to elt.
 * key == 0 means the slot is free.
 */
struct stackmap_entry {
	u32			key;	/* 0 = free, non-zero = jhash */
	struct stackmap_elt	*val;	/* NULL until fully published */
};

struct ftrace_stackmap {
	struct trace_array	*tr;		/* owning trace_array */
	unsigned int		map_bits;
	unsigned int		map_size;	/* 1 << (map_bits + 1) */
	unsigned int		max_elts;	/* 1 << map_bits */
	u32			hash_seed;	/* per-instance jhash seed */
	atomic_t		next_elt;	/* index into elts pool */
	struct stackmap_entry	*entries;	/* hash table */
	struct stackmap_elt	*elts;		/* flat element pool */
	atomic_t		resetting;
	atomic64_t		successes;	/* events served (hits + new inserts) */
	atomic64_t		drops;
};

/*
 * Cap the bits parameter to keep worst-case allocations bounded:
 *   bits=18 → 256K elts, 512K slots, ~130 MB elt pool, ~130 MB bin
 *             export.
 * Smaller workloads should use the default (14) which gives 16K elts
 * (~8 MB pool); bump bits via the ftrace_stackmap.bits= kernel
 * parameter for higher unique-stack capacity.
 */
#define FTRACE_STACKMAP_BITS_MIN	10
#define FTRACE_STACKMAP_BITS_MAX	18
#define FTRACE_STACKMAP_BITS_DEFAULT	14

static unsigned int stackmap_map_bits = FTRACE_STACKMAP_BITS_DEFAULT;
static int __init stackmap_bits_setup(char *str)
{
	unsigned long val;

	if (kstrtoul(str, 0, &val))
		return -EINVAL;
	val = clamp_val(val, FTRACE_STACKMAP_BITS_MIN, FTRACE_STACKMAP_BITS_MAX);
	stackmap_map_bits = val;
	return 0;
}
early_param("ftrace_stackmap.bits", stackmap_bits_setup);

/* --- Element pool --- */

static struct stackmap_elt *stackmap_get_elt(struct ftrace_stackmap *smap)
{
	int idx;

	/*
	 * Fast-path early-out once the pool is fully consumed. Avoids
	 * the contended atomic RMW on next_elt for every traced event
	 * after the pool is exhausted.
	 */
	if (atomic_read(&smap->next_elt) >= smap->max_elts)
		return NULL;

	idx = atomic_fetch_add_unless(&smap->next_elt, 1, smap->max_elts);
	if (idx < smap->max_elts)
		return &smap->elts[idx];
	return NULL;
}

/* --- Create / Destroy / Reset --- */

struct ftrace_stackmap *ftrace_stackmap_create(struct trace_array *tr)
{
	struct ftrace_stackmap *smap;
	unsigned int bits;

	smap = kzalloc(sizeof(*smap), GFP_KERNEL);
	if (!smap)
		return ERR_PTR(-ENOMEM);

	/* Defensive clamp: reject bogus bits even if early_param is bypassed. */
	bits = clamp_val(stackmap_map_bits,
			 FTRACE_STACKMAP_BITS_MIN,
			 FTRACE_STACKMAP_BITS_MAX);

	smap->tr = tr;
	smap->map_bits = bits;
	smap->max_elts = 1U << bits;
	smap->map_size = 1U << (bits + 1);	/* 2x over-provision */
	BUG_ON(!is_power_of_2(smap->map_size));

	smap->entries = vzalloc(sizeof(*smap->entries) * smap->map_size);
	if (!smap->entries) {
		kfree(smap);
		return ERR_PTR(-ENOMEM);
	}

	/*
	 * Single large vmalloc of the element pool, indexed flat.
	 * At bits=16 this is 64K * sizeof(struct stackmap_elt). The
	 * struct is ~520 B (8 + 4 + 4 + 64*8), so total ~33 MB.
	 */
	smap->elts = vzalloc(sizeof(*smap->elts) * (size_t)smap->max_elts);
	if (!smap->elts) {
		vfree(smap->entries);
		kfree(smap);
		return ERR_PTR(-ENOMEM);
	}

	smap->hash_seed = get_random_u32();
	atomic_set(&smap->next_elt, 0);
	atomic_set(&smap->resetting, 0);
	atomic64_set(&smap->successes, 0);
	atomic64_set(&smap->drops, 0);

	return smap;
}

void ftrace_stackmap_destroy(struct ftrace_stackmap *smap)
{
	if (!smap || IS_ERR(smap))
		return;
	vfree(smap->elts);
	vfree(smap->entries);
	kfree(smap);
}

/**
 * ftrace_stackmap_reset - clear all entries in the stackmap
 * @smap: the stackmap to reset
 *
 * Returns 0 on success, -EBUSY if another reset is already in
 * progress, or if tracing is currently active on the owning
 * trace_array.
 *
 * Online reset (with tracing active) is not supported. Caller must
 * stop tracing first (echo 0 > tracing_on).
 *
 * Caller is process context (typically sysfs write handler).
 *
 * Protocol:
 *   1. Atomically claim reset rights via cmpxchg on @resetting.
 *   2. Verify tracing is stopped on @smap->tr; if not, release the
 *      claim and return -EBUSY. The resetting flag itself blocks
 *      any subsequent get_id() callers.
 *   3. synchronize_rcu() drains in-flight get_id() callers from the
 *      ftrace callback path (which runs preempt-disabled).
 *   4. memset entries, elts, and counters.
 *   5. Release the resetting flag with release semantics so any new
 *      get_id() observes a fully cleared map.
 */
int ftrace_stackmap_reset(struct ftrace_stackmap *smap)
{
	if (!smap)
		return 0;

	if (atomic_cmpxchg(&smap->resetting, 0, 1) != 0)
		return -EBUSY;

	if (smap->tr && tracer_tracing_is_on(smap->tr)) {
		atomic_set(&smap->resetting, 0);
		return -EBUSY;
	}

	/*
	 * synchronize_rcu() itself is a full barrier; no extra smp_mb()
	 * is needed before it. It drains in-flight ftrace callbacks that
	 * may have already passed the resetting check with the old value.
	 */
	synchronize_rcu();

	memset(smap->entries, 0, sizeof(*smap->entries) * smap->map_size);
	memset(smap->elts, 0, sizeof(*smap->elts) * (size_t)smap->max_elts);

	atomic_set(&smap->next_elt, 0);
	atomic64_set(&smap->successes, 0);
	atomic64_set(&smap->drops, 0);

	/* Release resetting=0 so new get_id() observes a cleared map. */
	atomic_set_release(&smap->resetting, 0);
	return 0;
}

/* --- Core: get_id (lock-free, NMI-safe) --- */

int ftrace_stackmap_get_id(struct ftrace_stackmap *smap,
			   unsigned long *ips, unsigned int nr_entries)
{
	u32 key_hash, idx, test_key, trace_len;
	struct stackmap_entry *entry;
	struct stackmap_elt *val;
	int probes = 0;

	if (!smap || !nr_entries || atomic_read(&smap->resetting))
		return -EINVAL;
	if (nr_entries > FTRACE_STACKMAP_MAX_DEPTH)
		nr_entries = FTRACE_STACKMAP_MAX_DEPTH;

	trace_len = nr_entries * sizeof(unsigned long);
	/*
	 * jhash2() requires the length in u32 units and the data to be
	 * u32-aligned. On 64-bit kernels sizeof(unsigned long)==8, so
	 * trace_len is always a multiple of 8 (hence of 4). Use jhash2
	 * directly; the cast to u32* is safe because ips[] is naturally
	 * aligned to sizeof(unsigned long) >= 4.
	 */
	key_hash = jhash2((const u32 *)ips, trace_len / sizeof(u32),
			  smap->hash_seed);
	if (key_hash == 0)
		key_hash = 1;	/* 0 means free slot */

	idx = key_hash >> (32 - (smap->map_bits + 1));

	while (probes < FTRACE_STACKMAP_MAX_PROBE) {
		idx &= (smap->map_size - 1);
		entry = &smap->entries[idx];
		test_key = entry->key;

		if (test_key == key_hash) {
			/*
			 * smp_load_acquire pairs with smp_store_release in
			 * the publisher below; ensures we see fully-formed
			 * elt fields (nr, ips, ref_count) before dereference.
			 */
			val = smp_load_acquire(&entry->val);
			if (val && val->nr == nr_entries &&
			    memcmp(val->ips, ips, trace_len) == 0) {
				atomic_inc(&val->ref_count);
				atomic64_inc(&smap->successes);
				return (int)idx;
			}
			/*
			 * val == NULL: another CPU is mid-insert, or this
			 * slot is "claimed but empty" (pool exhausted).
			 * val != NULL but mismatch: 32-bit hash collision
			 * with a different stack. In both cases, advance.
			 */
		} else if (!test_key) {
			/* Free slot: try to claim it */
			if (cmpxchg(&entry->key, 0, key_hash) == 0) {
				struct stackmap_elt *elt;

				elt = stackmap_get_elt(smap);
				if (!elt) {
					/*
					 * Pool exhausted. We claimed this
					 * slot with cmpxchg but cannot fill
					 * it. Leave key set so the slot
					 * stays "claimed but empty" — future
					 * lookups treat val==NULL as a miss
					 * and probe past it. Cannot revert
					 * key=0 without racing other CPUs.
					 */
					atomic64_inc(&smap->drops);
					return -ENOSPC;
				}

				elt->nr = nr_entries;
				atomic_set(&elt->ref_count, 1);
				memcpy(elt->ips, ips, trace_len);

				/*
				 * Publish elt with release semantics so the
				 * reader's smp_load_acquire can safely
				 * dereference val->nr / val->ips.
				 */
				smp_store_release(&entry->val, elt);
				atomic64_inc(&smap->successes);
				return (int)idx;
			}
			/* cmpxchg failed; another CPU claimed this slot. */
		}

		idx++;
		probes++;
	}

	atomic64_inc(&smap->drops);
	return -ENOSPC;
}

/* --- Text export: /sys/kernel/debug/tracing/stack_map --- */

struct stackmap_seq_private {
	struct ftrace_stackmap	*smap;
};

static void *stackmap_seq_start(struct seq_file *m, loff_t *pos)
{
	struct stackmap_seq_private *priv = m->private;
	struct ftrace_stackmap *smap = priv->smap;
	u32 i;

	if (!smap)
		return NULL;
	for (i = *pos; i < smap->map_size; i++) {
		if (smap->entries[i].key && READ_ONCE(smap->entries[i].val)) {
			*pos = i;
			return &smap->entries[i];
		}
	}
	return NULL;
}

static void *stackmap_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct stackmap_seq_private *priv = m->private;
	struct ftrace_stackmap *smap = priv->smap;
	u32 i;

	if (!smap)
		return NULL;
	for (i = *pos + 1; i < smap->map_size; i++) {
		if (smap->entries[i].key && READ_ONCE(smap->entries[i].val)) {
			*pos = i;
			return &smap->entries[i];
		}
	}
	return NULL;
}

static void stackmap_seq_stop(struct seq_file *m, void *v) { }

static int stackmap_seq_show(struct seq_file *m, void *v)
{
	struct stackmap_entry *entry = v;
	struct stackmap_elt *elt = smp_load_acquire(&entry->val);
	struct stackmap_seq_private *priv = m->private;
	u32 idx = entry - priv->smap->entries;
	u32 i, nr;

	if (!elt)
		return 0;

	nr = READ_ONCE(elt->nr);
	if (nr > FTRACE_STACKMAP_MAX_DEPTH)
		nr = FTRACE_STACKMAP_MAX_DEPTH;

	seq_printf(m, "stack_id %u [ref %u, depth %u]\n",
		   idx, atomic_read(&elt->ref_count), nr);
	for (i = 0; i < nr; i++)
		seq_printf(m, "  [%u] %pS\n", i, (void *)elt->ips[i]);
	seq_putc(m, '\n');
	return 0;
}

static const struct seq_operations stackmap_seq_ops = {
	.start	= stackmap_seq_start,
	.next	= stackmap_seq_next,
	.stop	= stackmap_seq_stop,
	.show	= stackmap_seq_show,
};

static int stackmap_open(struct inode *inode, struct file *file)
{
	struct stackmap_seq_private *priv;
	struct seq_file *m;
	int ret;

	ret = seq_open_private(file, &stackmap_seq_ops,
			       sizeof(struct stackmap_seq_private));
	if (ret)
		return ret;
	m = file->private_data;
	priv = m->private;
	priv->smap = inode->i_private;
	return 0;
}

/*
 * Accept exactly "0" or "reset" (optionally followed by a single newline).
 */
static bool stackmap_write_is_reset(const char *buf, size_t n)
{
	if (n > 0 && buf[n - 1] == '\n')
		n--;
	return (n == 1 && buf[0] == '0') ||
	       (n == 5 && memcmp(buf, "reset", 5) == 0);
}

static ssize_t stackmap_write(struct file *file, const char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct stackmap_seq_private *priv = m->private;
	char buf[8];
	size_t n = min(count, sizeof(buf) - 1);
	int ret;

	if (n == 0)
		return -EINVAL;
	if (copy_from_user(buf, ubuf, n))
		return -EFAULT;
	buf[n] = '\0';

	if (!stackmap_write_is_reset(buf, n))
		return -EINVAL;

	/*
	 * ftrace_stackmap_reset() atomically claims reset rights via
	 * cmpxchg and returns -EBUSY if another reset is in progress
	 * or if tracing is active.
	 */
	ret = ftrace_stackmap_reset(priv->smap);
	if (ret)
		return ret;
	return count;
}

const struct file_operations ftrace_stackmap_fops = {
	.open		= stackmap_open,
	.read		= seq_read,
	.write		= stackmap_write,
	.llseek		= seq_lseek,
	.release	= seq_release_private,
};

/* --- Stats --- */

static int stackmap_stat_show(struct seq_file *m, void *v)
{
	struct ftrace_stackmap *smap = m->private;
	u32 entries;
	u64 successes, drops;

	if (!smap) {
		seq_puts(m, "stackmap not initialized\n");
		return 0;
	}

	entries = atomic_read(&smap->next_elt);
	successes = atomic64_read(&smap->successes);
	drops = atomic64_read(&smap->drops);

	seq_printf(m, "entries:      %u / %u\n", entries, smap->max_elts);
	seq_printf(m, "table_size:   %u\n", smap->map_size);
	seq_printf(m, "successes:    %llu\n", successes);
	seq_printf(m, "drops:        %llu\n", drops);
	if (successes + drops > 0)
		seq_printf(m, "success_rate: %llu%%\n",
			   successes * 100 / (successes + drops));
	return 0;
}

static int stackmap_stat_open(struct inode *inode, struct file *file)
{
	return single_open(file, stackmap_stat_show, inode->i_private);
}

const struct file_operations ftrace_stackmap_stat_fops = {
	.open		= stackmap_stat_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* --- Binary export --- */

struct stackmap_bin_snapshot {
	/*
	 * Use u64 (not size_t) so data[] is 8-byte aligned on both
	 * 32-bit and 64-bit architectures. The IP array within data[]
	 * is accessed as u64*, which would alignment-fault on strict
	 * architectures (e.g. older ARM, SPARC) if data[] started at
	 * a 4-byte boundary.
	 */
	u64	size;
	char	data[];
};

static int stackmap_bin_open(struct inode *inode, struct file *file)
{
	struct ftrace_stackmap *smap = inode->i_private;
	struct stackmap_bin_snapshot *snap;
	struct ftrace_stackmap_bin_header *hdr;
	size_t alloc_size, off;
	u32 nr_entries, i, nr_stacks;

	if (!smap)
		return -ENODEV;

	/*
	 * Worst-case allocation size: every populated entry uses a
	 * full-depth stack. The (+1) gives one slack slot in case a
	 * concurrent insert lands between this snapshot and iteration.
	 * The loop below performs an explicit bounds check anyway.
	 *
	 * At bits=16 this caps at ~33 MB. The file is mode 0440
	 * (TRACE_MODE_READ), so only privileged users can open it.
	 */
	nr_entries = atomic_read(&smap->next_elt);
	alloc_size = sizeof(*hdr) + (nr_entries + 1) *
		     (sizeof(struct ftrace_stackmap_bin_entry) +
		      FTRACE_STACKMAP_MAX_DEPTH * sizeof(u64));

	snap = vmalloc(sizeof(*snap) + alloc_size);
	if (!snap)
		return -ENOMEM;

	hdr = (struct ftrace_stackmap_bin_header *)snap->data;
	hdr->magic = FTRACE_STACKMAP_BIN_MAGIC;
	hdr->version = FTRACE_STACKMAP_BIN_VERSION;
	hdr->reserved = 0;
	off = sizeof(*hdr);
	nr_stacks = 0;

	for (i = 0; i < smap->map_size; i++) {
		struct stackmap_entry *entry = &smap->entries[i];
		struct stackmap_elt *elt;
		struct ftrace_stackmap_bin_entry *e;
		u64 *ips_out;
		u32 k, nr;

		if (!entry->key)
			continue;
		elt = smp_load_acquire(&entry->val);
		if (!elt)
			continue;

		nr = READ_ONCE(elt->nr);
		if (nr > FTRACE_STACKMAP_MAX_DEPTH)
			nr = FTRACE_STACKMAP_MAX_DEPTH;

		/* Bounds check: stop if we would overflow the allocation. */
		if (off + sizeof(*e) + nr * sizeof(u64) > alloc_size)
			break;

		e = (struct ftrace_stackmap_bin_entry *)(snap->data + off);
		e->stack_id = i;
		e->nr = nr;
		e->ref_count = atomic_read(&elt->ref_count);
		e->reserved = 0;
		off += sizeof(*e);

		ips_out = (u64 *)(snap->data + off);
		for (k = 0; k < nr; k++)
			ips_out[k] = (u64)elt->ips[k];
		off += nr * sizeof(u64);
		nr_stacks++;
	}

	hdr->nr_stacks = nr_stacks;
	snap->size = off;
	file->private_data = snap;
	return 0;
}

static ssize_t stackmap_bin_read(struct file *file, char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	struct stackmap_bin_snapshot *snap = file->private_data;

	if (!snap)
		return -EINVAL;
	return simple_read_from_buffer(ubuf, count, ppos, snap->data, snap->size);
}

static int stackmap_bin_release(struct inode *inode, struct file *file)
{
	vfree(file->private_data);
	return 0;
}

const struct file_operations ftrace_stackmap_bin_fops = {
	.open		= stackmap_bin_open,
	.read		= stackmap_bin_read,
	.llseek		= default_llseek,
	.release	= stackmap_bin_release,
};
