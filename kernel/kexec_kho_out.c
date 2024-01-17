// SPDX-License-Identifier: GPL-2.0-only
/*
 * kexec_kho_out.c - kexec handover code to egest metadata.
 * Copyright (C) 2023 Alexander Graf <graf@amazon.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cma.h>
#include <linux/kexec.h>
#include <linux/device.h>
#include <linux/compiler.h>
#include <linux/kmsg_dump.h>

struct kho_out {
	struct kobject *kobj;
	bool active;
	struct cma *cma;
	struct blocking_notifier_head chain_head;
	void *dt;
	u64 dt_len;
	u64 dt_max;
	struct mutex lock;
};

static struct kho_out kho = {
	.dt_max = (1024 * 1024 * 10),
	.chain_head = BLOCKING_NOTIFIER_INIT(kho.chain_head),
	.lock = __MUTEX_INITIALIZER(kho.lock),
};

/*
 * Size for scratch (non-KHO) memory. With KHO enabled, memory can become
 * fragmented because KHO regions may be anywhere in physical address
 * space. The scratch region gives us a safe zone that we will never see
 * KHO allocations from. This is where we can later safely load our new kexec
 * images into.
 */
static phys_addr_t kho_scratch_size __initdata;

int register_kho_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&kho.chain_head, nb);
}
EXPORT_SYMBOL_GPL(register_kho_notifier);

int unregister_kho_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&kho.chain_head, nb);
}
EXPORT_SYMBOL_GPL(unregister_kho_notifier);

static int kho_mem_cache_add(void *fdt, struct kho_mem *mem_cache, int size,
			     struct kho_mem *new_mem)
{
	int entries = size / sizeof(*mem_cache);
	u64 new_start = new_mem->addr;
	u64 new_end = new_mem->addr + new_mem->len;
	u64 prev_start = 0;
	u64 prev_end = 0;
	int i;

	if (WARN_ON((new_start < (kho_scratch_phys + kho_scratch_len)) &&
		    (new_end > kho_scratch_phys))) {
		pr_err("KHO memory runs over scratch memory");
		return -EINVAL;
	}

	/*
	 * We walk the existing sorted mem cache and find the spot where this
	 * new entry would start, so we can insert it right there.
	 */
	for (i = 0; i < entries; i++) {
		struct kho_mem *mem = &mem_cache[i];
		u64 mem_end = (mem->addr + mem->len);

		if (mem_end < new_start) {
			/* No overlap */
			prev_start = mem->addr;
			prev_end = mem->addr + mem->len;
			continue;
		} else if ((new_start >= mem->addr) && (new_end <= mem_end)) {
			/* new_mem fits into mem, skip */
			return size;
		} else if ((new_end >= mem->addr) && (new_start <= mem_end)) {
			/* new_mem and mem overlap, fold them */
			bool remove = false;

			mem->addr = min(new_start, mem->addr);
			mem->len = max(mem_end, new_end) - mem->addr;
			mem_end = (mem->addr + mem->len);

			if (i > 0 && prev_end >= mem->addr) {
				/* We now overlap with the previous mem, fold */
				struct kho_mem *prev = &mem_cache[i - 1];

				prev->addr = min(prev->addr, mem->addr);
				prev->len = max(mem_end, prev_end) - prev->addr;
				remove = true;
			} else if (i < (entries - 1) && mem_end >= mem_cache[i + 1].addr) {
				/* We now overlap with the next mem, fold */
				struct kho_mem *next = &mem_cache[i + 1];
				u64 next_end = (next->addr + next->len);

				next->addr = min(next->addr, mem->addr);
				next->len = max(mem_end, next_end) - next->addr;
				remove = true;
			}

			if (remove) {
				/* We folded this mem into another, remove it */
				memmove(mem, mem + 1, (entries - i - 1) * sizeof(*mem));
				size -= sizeof(*new_mem);
			}

			return size;
		} else if (mem->addr > new_end) {
			/*
			 * The mem cache is sorted. If we find the current
			 * entry start after our new_mem's end, we shot over
			 * which means we need to add it by creating a new
			 * hole right after the current entry.
			 */
			memmove(mem + 1, mem, (entries - i) * sizeof(*mem));
			break;
		}
	}

	mem_cache[i] = *new_mem;
	size += sizeof(*new_mem);

	return size;
}

/**
 * kho_alloc_mem_cache - Allocate and initialize the mem cache kexec_buf
 */
static int kho_alloc_mem_cache(struct kimage *image, void *fdt)
{
	int offset, depth, initial_depth, len;
	void *mem_cache;
	int size;

	/* Count the elements inside all "mem" properties in the DT */
	size = offset = depth = initial_depth = 0;
	for (offset = 0;
	     offset >= 0 && depth >= initial_depth;
	     offset = fdt_next_node(fdt, offset, &depth)) {
		const struct kho_mem *mems;

		mems = fdt_getprop(fdt, offset, "mem", &len);
		if (!mems || len & (sizeof(*mems) - 1))
			continue;
		size += len;
	}

	/* Allocate based on the max size we determined */
	mem_cache = kvmalloc(size, GFP_KERNEL);
	if (!mem_cache)
		return -ENOMEM;

	/* And populate the array */
	size = offset = depth = initial_depth = 0;
	for (offset = 0;
	     offset >= 0 && depth >= initial_depth;
	     offset = fdt_next_node(fdt, offset, &depth)) {
		const struct kho_mem *mems;
		int nr_mems, i;

		mems = fdt_getprop(fdt, offset, "mem", &len);
		if (!mems || len & (sizeof(*mems) - 1))
			continue;

		for (i = 0, nr_mems = len / sizeof(*mems); i < nr_mems; i++) {
			const struct kho_mem *mem = &mems[i];
			ulong mstart = PAGE_ALIGN_DOWN(mem->addr);
			ulong mend = PAGE_ALIGN(mem->addr + mem->len);
			struct kho_mem cmem = {
				.addr = mstart,
				.len = (mend - mstart),
			};

			size = kho_mem_cache_add(fdt, mem_cache, size, &cmem);
			if (size < 0)
				return size;
		}
	}

	image->kho.mem_cache.buffer = mem_cache;
	image->kho.mem_cache.bufsz = size;
	image->kho.mem_cache.memsz = size;

	return 0;
}

int kho_fill_kimage(struct kimage *image)
{
	int err = 0;
	void *dt;

	mutex_lock(&kho.lock);

	if (!kho.active)
		goto out;

	/* Initialize kexec_buf for mem_cache */
	image->kho.mem_cache = (struct kexec_buf) {
		.image = image,
		.buffer = NULL,
		.bufsz = 0,
		.mem = KEXEC_BUF_MEM_UNKNOWN,
		.memsz = 0,
		.buf_align = SZ_64K, /* Makes it easier to map */
		.buf_max = ULONG_MAX,
		.top_down = true,
	};

	/*
	 * We need to make all allocations visible here via the mem_cache so that
	 * kho_is_destination_range() can identify overlapping regions and ensure
	 * that no kimage (including the DT one) lands on handed over memory.
	 *
	 * Since we conveniently already built an array of all allocations, let's
	 * pass that on to the target kernel so that reuse it to initialize its
	 * memory blocks.
	 */
	err = kho_alloc_mem_cache(image, kho.dt);
	if (err)
		goto out;

	err = kexec_add_buffer(&image->kho.mem_cache);
	if (err)
		goto out;

	/*
	 * Create a kexec copy of the DT here. We need this because lifetime may
	 * be different between kho.dt and the kimage
	 */
	dt = kvmemdup(kho.dt, kho.dt_len, GFP_KERNEL);
	if (!dt) {
		err = -ENOMEM;
		goto out;
	}

	/* Allocate target memory for kho dt */
	image->kho.dt = (struct kexec_buf) {
		.image = image,
		.buffer = dt,
		.bufsz = kho.dt_len,
		.mem = KEXEC_BUF_MEM_UNKNOWN,
		.memsz = kho.dt_len,
		.buf_align = SZ_64K, /* Makes it easier to map */
		.buf_max = ULONG_MAX,
		.top_down = true,
	};
	err = kexec_add_buffer(&image->kho.dt);

out:
	mutex_unlock(&kho.lock);
	return err;
}

bool kho_is_active(void)
{
	return kho.active;
}
EXPORT_SYMBOL_GPL(kho_is_active);

static ssize_t raw_read(struct file *file, struct kobject *kobj,
			struct bin_attribute *attr, char *buf,
			loff_t pos, size_t count)
{
	mutex_lock(&kho.lock);
	memcpy(buf, attr->private + pos, count);
	mutex_unlock(&kho.lock);

	return count;
}

static BIN_ATTR(dt, 0400, raw_read, NULL, 0);

static int kho_expose_dt(void *fdt)
{
	long fdt_len = fdt_totalsize(fdt);
	int err;

	kho.dt = fdt;
	kho.dt_len = fdt_len;

	bin_attr_dt.size = fdt_totalsize(fdt);
	bin_attr_dt.private = fdt;
	err = sysfs_create_bin_file(kho.kobj, &bin_attr_dt);

	return err;
}

static void kho_abort(void)
{
	if (!kho.active)
		return;

	sysfs_remove_bin_file(kho.kobj, &bin_attr_dt);

	kvfree(kho.dt);
	kho.dt = NULL;
	kho.dt_len = 0;

	blocking_notifier_call_chain(&kho.chain_head, KEXEC_KHO_ABORT, NULL);

	kho.active = false;
}

static int kho_serialize(void)
{
	void *fdt = NULL;
	int err;

	kho.active = true;
	err = -ENOMEM;

	fdt = kvmalloc(kho.dt_max, GFP_KERNEL);
	if (!fdt)
		goto out;

	if (fdt_create(fdt, kho.dt_max)) {
		err = -EINVAL;
		goto out;
	}

	err = fdt_finish_reservemap(fdt);
	if (err)
		goto out;

	err = fdt_begin_node(fdt, "");
	if (err)
		goto out;

	err = fdt_property_string(fdt, "compatible", "kho-v1");
	if (err)
		goto out;

	/* Loop through all kho dump functions */
	err = blocking_notifier_call_chain(&kho.chain_head, KEXEC_KHO_DUMP, fdt);
	err = notifier_to_errno(err);
	if (err)
		goto out;

	/* Close / */
	err =  fdt_end_node(fdt);
	if (err)
		goto out;

	err = fdt_finish(fdt);
	if (err)
		goto out;

	if (WARN_ON(fdt_check_header(fdt))) {
		err = -EINVAL;
		goto out;
	}

	err = kho_expose_dt(fdt);

out:
	if (err) {
		pr_err("kho failed to serialize state: %d", err);
		kho_abort();
	}
	return err;
}

/* Handling for /sys/kernel/kho */

#define KHO_ATTR_RO(_name) static struct kobj_attribute _name##_attr = __ATTR_RO_MODE(_name, 0400)
#define KHO_ATTR_RW(_name) static struct kobj_attribute _name##_attr = __ATTR_RW_MODE(_name, 0600)

static ssize_t active_store(struct kobject *dev, struct kobj_attribute *attr,
			    const char *buf, size_t size)
{
	ssize_t retsize = size;
	bool val = false;
	int ret;

	if (kstrtobool(buf, &val) < 0)
		return -EINVAL;

	if (!kho_scratch_len)
		return -ENOMEM;

	mutex_lock(&kho.lock);
	if (val != kho.active) {
		if (val) {
			ret = kho_serialize();
			if (ret) {
				retsize = -EINVAL;
				goto out;
			}
		} else {
			kho_abort();
		}
	}

out:
	mutex_unlock(&kho.lock);
	return retsize;
}

static ssize_t active_show(struct kobject *dev, struct kobj_attribute *attr,
			   char *buf)
{
	ssize_t ret;

	mutex_lock(&kho.lock);
	ret = sysfs_emit(buf, "%d\n", kho.active);
	mutex_unlock(&kho.lock);

	return ret;
}
KHO_ATTR_RW(active);

static ssize_t dt_max_store(struct kobject *dev, struct kobj_attribute *attr,
			    const char *buf, size_t size)
{
	u64 val;

	if (kstrtoull(buf, 0, &val))
		return -EINVAL;

	kho.dt_max = val;

	return size;
}

static ssize_t dt_max_show(struct kobject *dev, struct kobj_attribute *attr,
			   char *buf)
{
	return sysfs_emit(buf, "0x%llx\n", kho.dt_max);
}
KHO_ATTR_RW(dt_max);

static ssize_t scratch_len_show(struct kobject *dev, struct kobj_attribute *attr,
				char *buf)
{
	return sysfs_emit(buf, "0x%llx\n", kho_scratch_len);
}
KHO_ATTR_RO(scratch_len);

static ssize_t scratch_phys_show(struct kobject *dev, struct kobj_attribute *attr,
				 char *buf)
{
	return sysfs_emit(buf, "0x%llx\n", kho_scratch_phys);
}
KHO_ATTR_RO(scratch_phys);

static __init int kho_out_init(void)
{
	int ret = 0;

	kho.kobj = kobject_create_and_add("kho", kernel_kobj);
	if (!kho.kobj) {
		ret = -ENOMEM;
		goto err;
	}

	ret = sysfs_create_file(kho.kobj, &active_attr.attr);
	if (ret)
		goto err;

	ret = sysfs_create_file(kho.kobj, &dt_max_attr.attr);
	if (ret)
		goto err;

	ret = sysfs_create_file(kho.kobj, &scratch_phys_attr.attr);
	if (ret)
		goto err;

	ret = sysfs_create_file(kho.kobj, &scratch_len_attr.attr);
	if (ret)
		goto err;

err:
	return ret;
}
late_initcall(kho_out_init);

static int __init early_kho_scratch(char *p)
{
	kho_scratch_size = memparse(p, &p);
	return 0;
}
early_param("kho_scratch", early_kho_scratch);

/**
 * kho_reserve_scratch - Reserve a contiguous chunk of memory for kexec
 *
 * With KHO we can preserve arbitrary pages in the system. To ensure we still
 * have a large contiguous region of memory when we search the physical address
 * space for target memory, let's make sure we always have a large CMA region
 * active. This CMA region will only be used for movable pages which are not a
 * problem for us during KHO because we can just move them somewhere else.
 */
__init void kho_reserve_scratch(void)
{
	int r;

	if (kho_get_fdt()) {
		/*
		 * We came from a previous KHO handover, so we already have
		 * a known good scratch region that we preserve. No need to
		 * allocate another.
		 */
		return;
	}

	/* Only allocate KHO scratch memory when we're asked to */
	if (!kho_scratch_size)
		return;

	r = cma_declare_contiguous_nid(0, kho_scratch_size, 0, PAGE_SIZE, 0,
				       false, "kho", &kho.cma, NUMA_NO_NODE);
	if (WARN_ON(r))
		return;

	kho_scratch_phys = cma_get_base(kho.cma);
	kho_scratch_len = cma_get_size(kho.cma);
}
