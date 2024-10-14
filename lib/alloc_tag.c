// SPDX-License-Identifier: GPL-2.0-only
#include <linux/alloc_tag.h>
#include <linux/execmem.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/module.h>
#include <linux/page_ext.h>
#include <linux/proc_fs.h>
#include <linux/seq_buf.h>
#include <linux/seq_file.h>
#include <linux/vmalloc.h>

static struct codetag_type *alloc_tag_cttype;

DEFINE_PER_CPU(struct alloc_tag_counters, _shared_alloc_tag);
EXPORT_SYMBOL(_shared_alloc_tag);

DEFINE_STATIC_KEY_MAYBE(CONFIG_MEM_ALLOC_PROFILING_ENABLED_BY_DEFAULT,
			mem_alloc_profiling_key);

struct allocinfo_private {
	struct codetag_iterator iter;
	bool print_header;
};

static void *allocinfo_start(struct seq_file *m, loff_t *pos)
{
	struct allocinfo_private *priv;
	struct codetag *ct;
	loff_t node = *pos;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	m->private = priv;
	if (!priv)
		return NULL;

	priv->print_header = (node == 0);
	codetag_lock_module_list(alloc_tag_cttype, true);
	priv->iter = codetag_get_ct_iter(alloc_tag_cttype);
	while ((ct = codetag_next_ct(&priv->iter)) != NULL && node)
		node--;

	return ct ? priv : NULL;
}

static void *allocinfo_next(struct seq_file *m, void *arg, loff_t *pos)
{
	struct allocinfo_private *priv = (struct allocinfo_private *)arg;
	struct codetag *ct = codetag_next_ct(&priv->iter);

	(*pos)++;
	if (!ct)
		return NULL;

	return priv;
}

static void allocinfo_stop(struct seq_file *m, void *arg)
{
	struct allocinfo_private *priv = (struct allocinfo_private *)m->private;

	if (priv) {
		codetag_lock_module_list(alloc_tag_cttype, false);
		kfree(priv);
	}
}

static void print_allocinfo_header(struct seq_buf *buf)
{
	/* Output format version, so we can change it. */
	seq_buf_printf(buf, "allocinfo - version: 1.0\n");
	seq_buf_printf(buf, "#     <size>  <calls> <tag info>\n");
}

static void alloc_tag_to_text(struct seq_buf *out, struct codetag *ct)
{
	struct alloc_tag *tag = ct_to_alloc_tag(ct);
	struct alloc_tag_counters counter = alloc_tag_read(tag);
	s64 bytes = counter.bytes;

	seq_buf_printf(out, "%12lli %8llu ", bytes, counter.calls);
	codetag_to_text(out, ct);
	seq_buf_putc(out, ' ');
	seq_buf_putc(out, '\n');
}

static int allocinfo_show(struct seq_file *m, void *arg)
{
	struct allocinfo_private *priv = (struct allocinfo_private *)arg;
	char *bufp;
	size_t n = seq_get_buf(m, &bufp);
	struct seq_buf buf;

	seq_buf_init(&buf, bufp, n);
	if (priv->print_header) {
		print_allocinfo_header(&buf);
		priv->print_header = false;
	}
	alloc_tag_to_text(&buf, priv->iter.ct);
	seq_commit(m, seq_buf_used(&buf));
	return 0;
}

static const struct seq_operations allocinfo_seq_op = {
	.start	= allocinfo_start,
	.next	= allocinfo_next,
	.stop	= allocinfo_stop,
	.show	= allocinfo_show,
};

size_t alloc_tag_top_users(struct codetag_bytes *tags, size_t count, bool can_sleep)
{
	struct codetag_iterator iter;
	struct codetag *ct;
	struct codetag_bytes n;
	unsigned int i, nr = 0;

	if (can_sleep)
		codetag_lock_module_list(alloc_tag_cttype, true);
	else if (!codetag_trylock_module_list(alloc_tag_cttype))
		return 0;

	iter = codetag_get_ct_iter(alloc_tag_cttype);
	while ((ct = codetag_next_ct(&iter))) {
		struct alloc_tag_counters counter = alloc_tag_read(ct_to_alloc_tag(ct));

		n.ct	= ct;
		n.bytes = counter.bytes;

		for (i = 0; i < nr; i++)
			if (n.bytes > tags[i].bytes)
				break;

		if (i < count) {
			nr -= nr == count;
			memmove(&tags[i + 1],
				&tags[i],
				sizeof(tags[0]) * (nr - i));
			nr++;
			tags[i] = n;
		}
	}

	codetag_lock_module_list(alloc_tag_cttype, false);

	return nr;
}

static void __init procfs_init(void)
{
	proc_create_seq("allocinfo", 0400, NULL, &allocinfo_seq_op);
}

#ifdef CONFIG_MODULES

static struct maple_tree mod_area_mt = MTREE_INIT(mod_area_mt, MT_FLAGS_ALLOC_RANGE);
static struct vm_struct *vm_module_tags;
/* A dummy object used to indicate an unloaded module */
static struct module unloaded_mod;
/* A dummy object used to indicate a module prepended area */
static struct module prepend_mod;

static struct alloc_tag_module_section module_tags;

static bool needs_section_mem(struct module *mod, unsigned long size)
{
	return size >= sizeof(struct alloc_tag);
}

/* Called under RCU read lock */
static void clean_unused_module_areas_locked(void)
{
	MA_STATE(mas, &mod_area_mt, 0, module_tags.size);
	struct module *val;

	mas_for_each(&mas, val, module_tags.size) {
		if (val == &unloaded_mod) {
			struct alloc_tag *tag;
			struct alloc_tag *last;
			bool unused = true;

			tag = (struct alloc_tag *)(module_tags.start_addr + mas.index);
			last = (struct alloc_tag *)(module_tags.start_addr + mas.last);
			while (tag <= last) {
				struct alloc_tag_counters counter;

				counter = alloc_tag_read(tag);
				if (counter.bytes) {
					unused = false;
					break;
				}
				tag++;
			}
			if (unused)
				mas_erase(&mas);
		}
	}
}

static int vm_module_tags_grow(unsigned long addr, unsigned long bytes)
{
	struct page **next_page = vm_module_tags->pages + vm_module_tags->nr_pages;
	unsigned long more_pages = ALIGN(bytes, PAGE_SIZE) >> PAGE_SHIFT;
	unsigned long nr;

	nr = alloc_pages_bulk_array_node(GFP_KERNEL | __GFP_NOWARN,
					 NUMA_NO_NODE, more_pages, next_page);
	if (nr != more_pages)
		return -ENOMEM;

	vm_module_tags->nr_pages += nr;
	if (vmap_pages_range(addr, addr + (nr << PAGE_SHIFT),
			     PAGE_KERNEL, next_page, PAGE_SHIFT) < 0)
		return -ENOMEM;

	return 0;
}

static void *reserve_module_tags(struct module *mod, unsigned long size,
				 unsigned int prepend, unsigned long align)
{
	unsigned long section_size = module_tags.end_addr - module_tags.start_addr;
	MA_STATE(mas, &mod_area_mt, 0, section_size - 1);
	bool cleanup_done = false;
	unsigned long offset;
	void *ret = NULL;

	/* If no tags return NULL */
	if (size < sizeof(struct alloc_tag))
		return NULL;

	/*
	 * align is always power of 2, so we can use IS_ALIGNED and ALIGN.
	 * align 0 or 1 means no alignment, to simplify set to 1.
	 */
	if (!align)
		align = 1;

	mas_lock(&mas);
repeat:
	/* Try finding exact size and hope the start is aligned */
	if (mas_empty_area(&mas, 0, section_size - 1, prepend + size))
		goto cleanup;

	if (IS_ALIGNED(mas.index + prepend, align))
		goto found;

	/* Try finding larger area to align later */
	mas_reset(&mas);
	if (!mas_empty_area(&mas, 0, section_size - 1,
			    size + prepend + align - 1))
		goto found;

cleanup:
	/* No free area, try cleanup stale data and repeat the search once */
	if (!cleanup_done) {
		clean_unused_module_areas_locked();
		cleanup_done = true;
		mas_reset(&mas);
		goto repeat;
	} else {
		ret = ERR_PTR(-ENOMEM);
		goto unlock;
	}

found:
	offset = mas.index;
	offset += prepend;
	offset = ALIGN(offset, align);

	if (offset != mas.index) {
		unsigned long pad_start = mas.index;

		mas.last = offset - 1;
		mas_store(&mas, &prepend_mod);
		if (mas_is_err(&mas)) {
			ret = ERR_PTR(xa_err(mas.node));
			goto unlock;
		}
		mas.index = offset;
		mas.last = offset + size - 1;
		mas_store(&mas, mod);
		if (mas_is_err(&mas)) {
			ret = ERR_PTR(xa_err(mas.node));
			mas.index = pad_start;
			mas_erase(&mas);
			goto unlock;
		}

	} else {
		mas.last = offset + size - 1;
		mas_store(&mas, mod);
		if (mas_is_err(&mas)) {
			ret = ERR_PTR(xa_err(mas.node));
			goto unlock;
		}
	}
unlock:
	mas_unlock(&mas);
	if (IS_ERR(ret))
		return ret;

	if (module_tags.size < offset + size) {
		unsigned long phys_size = vm_module_tags->nr_pages << PAGE_SHIFT;

		module_tags.size = offset + size;
		if (phys_size < module_tags.size) {
			int grow_res;

			grow_res = vm_module_tags_grow(module_tags.start_addr + phys_size,
						       module_tags.size - phys_size);
			if (grow_res) {
				static_branch_disable(&mem_alloc_profiling_key);
				pr_warn("Failed to allocate tags memory for module %s. Memory profiling is disabled!\n",
					mod->name);
				return ERR_PTR(grow_res);
			}
		}
	}

	return (struct alloc_tag *)(module_tags.start_addr + offset);
}

static void release_module_tags(struct module *mod, bool unused)
{
	MA_STATE(mas, &mod_area_mt, module_tags.size, module_tags.size);
	struct alloc_tag *last;
	struct alloc_tag *tag;
	struct module *val;

	if (unused)
		return;

	mas_lock(&mas);
	mas_for_each_rev(&mas, val, 0)
		if (val == mod)
			break;

	if (!val) /* module not found */
		goto out;

	tag = (struct alloc_tag *)(module_tags.start_addr + mas.index);
	last = (struct alloc_tag *)(module_tags.start_addr + mas.last);

	unused = true;
	while (tag <= last) {
		struct alloc_tag_counters counter;

		counter = alloc_tag_read(tag);
		if (counter.bytes) {
			struct codetag *ct = &tag->ct;

			pr_info("%s:%u module %s func:%s has %llu allocated at module unload\n",
				ct->filename, ct->lineno, ct->modname,
				ct->function, counter.bytes);
			unused = false;
			break;
		}
		tag++;
	}

	mas_store(&mas, unused ? NULL : &unloaded_mod);
	val = mas_prev_range(&mas, 0);
	if (val == &prepend_mod)
		mas_store(&mas, NULL);
out:
	mas_unlock(&mas);
}

static void replace_module(struct module *mod, struct module *new_mod)
{
	MA_STATE(mas, &mod_area_mt, 0, module_tags.size);
	struct module *val;

	mas_lock(&mas);
	mas_for_each(&mas, val, module_tags.size) {
		if (val != mod)
			continue;

		mas_store_gfp(&mas, new_mod, GFP_KERNEL);
		break;
	}
	mas_unlock(&mas);
}

#define MODULE_ALLOC_TAG_VMAP_SIZE (100000 * sizeof(struct alloc_tag))

static int __init alloc_mod_tags_mem(void)
{
	/* Map space to copy allocation tags */
	vm_module_tags = execmem_vmap(EXECMEM_MODULE_DATA, MODULE_ALLOC_TAG_VMAP_SIZE);
	if (!vm_module_tags) {
		pr_err("Failed to map %lu bytes for module allocation tags\n",
			MODULE_ALLOC_TAG_VMAP_SIZE);
		module_tags.start_addr = 0;
		return -ENOMEM;
	}

	vm_module_tags->pages = kmalloc_array(get_vm_area_size(vm_module_tags) >> PAGE_SHIFT,
					sizeof(struct page *), GFP_KERNEL | __GFP_ZERO);
	if (!vm_module_tags->pages) {
		free_vm_area(vm_module_tags);
		return -ENOMEM;
	}

	module_tags.start_addr = (unsigned long)vm_module_tags->addr;
	module_tags.end_addr = module_tags.start_addr + MODULE_ALLOC_TAG_VMAP_SIZE;

	return 0;
}

static void __init free_mod_tags_mem(void)
{
	int i;

	module_tags.start_addr = 0;
	for (i = 0; i < vm_module_tags->nr_pages; i++)
		__free_page(vm_module_tags->pages[i]);
	kfree(vm_module_tags->pages);
	free_vm_area(vm_module_tags);
}

#else /* CONFIG_MODULES */

static inline int alloc_mod_tags_mem(void) { return 0; }
static inline void free_mod_tags_mem(void) {}

#endif /* CONFIG_MODULES */

#ifdef CONFIG_MEM_ALLOC_PROFILING_ENABLED_BY_DEFAULT
static bool mem_profiling_support __meminitdata = true;
#else
static bool mem_profiling_support __meminitdata;
#endif

static int __init setup_early_mem_profiling(char *str)
{
	bool enable;

	if (!str || !str[0])
		return -EINVAL;

	if (!strncmp(str, "never", 5)) {
		enable = false;
		mem_profiling_support = false;
	} else {
		int res;

		res = kstrtobool(str, &enable);
		if (res)
			return res;

		mem_profiling_support = true;
	}

	if (enable != static_key_enabled(&mem_alloc_profiling_key)) {
		if (enable)
			static_branch_enable(&mem_alloc_profiling_key);
		else
			static_branch_disable(&mem_alloc_profiling_key);
	}

	return 0;
}
early_param("sysctl.vm.mem_profiling", setup_early_mem_profiling);

static __init bool need_page_alloc_tagging(void)
{
	return mem_profiling_support;
}

static __init void init_page_alloc_tagging(void)
{
}

struct page_ext_operations page_alloc_tagging_ops = {
	.size = sizeof(union codetag_ref),
	.need = need_page_alloc_tagging,
	.init = init_page_alloc_tagging,
};
EXPORT_SYMBOL(page_alloc_tagging_ops);

#ifdef CONFIG_SYSCTL
static struct ctl_table memory_allocation_profiling_sysctls[] = {
	{
		.procname	= "mem_profiling",
		.data		= &mem_alloc_profiling_key,
#ifdef CONFIG_MEM_ALLOC_PROFILING_DEBUG
		.mode		= 0444,
#else
		.mode		= 0644,
#endif
		.proc_handler	= proc_do_static_key,
	},
};

static void __init sysctl_init(void)
{
	if (!mem_profiling_support)
		memory_allocation_profiling_sysctls[0].mode = 0444;

	register_sysctl_init("vm", memory_allocation_profiling_sysctls);
}
#else /* CONFIG_SYSCTL */
static inline void sysctl_init(void) {}
#endif /* CONFIG_SYSCTL */

static int __init alloc_tag_init(void)
{
	const struct codetag_type_desc desc = {
		.section		= ALLOC_TAG_SECTION_NAME,
		.tag_size		= sizeof(struct alloc_tag),
#ifdef CONFIG_MODULES
		.needs_section_mem	= needs_section_mem,
		.alloc_section_mem	= reserve_module_tags,
		.free_section_mem	= release_module_tags,
		.module_replaced	= replace_module,
#endif
	};
	int res;

	res = alloc_mod_tags_mem();
	if (res)
		return res;

	alloc_tag_cttype = codetag_register_type(&desc);
	if (IS_ERR(alloc_tag_cttype)) {
		free_mod_tags_mem();
		return PTR_ERR(alloc_tag_cttype);
	}

	sysctl_init();
	procfs_init();

	return 0;
}
module_init(alloc_tag_init);
