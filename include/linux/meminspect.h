/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MEMINSPECT_H
#define _MEMINSPECT_H

#include <linux/notifier.h>

enum meminspect_uid {
	MEMINSPECT_ID_STATIC = 0,
	MEMINSPECT_ID_ELF,
	MEMINSPECT_ID_VMCOREINFO,
	MEMINSPECT_ID_CONFIG,
	MEMINSPECT_ID__totalram_pages,
	MEMINSPECT_ID___cpu_possible_mask,
	MEMINSPECT_ID___cpu_present_mask,
	MEMINSPECT_ID___cpu_online_mask,
	MEMINSPECT_ID___cpu_active_mask,
	MEMINSPECT_ID_mem_section,
	MEMINSPECT_ID_jiffies_64,
	MEMINSPECT_ID_linux_banner,
	MEMINSPECT_ID_nr_threads,
	MEMINSPECT_ID_nr_irqs,
	MEMINSPECT_ID_tainted_mask,
	MEMINSPECT_ID_taint_flags,
	MEMINSPECT_ID_node_states,
	MEMINSPECT_ID___per_cpu_offset,
	MEMINSPECT_ID_nr_swapfiles,
	MEMINSPECT_ID_init_uts_ns,
	MEMINSPECT_ID_printk_rb_static,
	MEMINSPECT_ID_printk_rb_dynamic,
	MEMINSPECT_ID_prb,
	MEMINSPECT_ID_prb_descs,
	MEMINSPECT_ID_prb_infos,
	MEMINSPECT_ID_prb_data,
	MEMINSPECT_ID_high_memory,
	MEMINSPECT_ID_init_mm,
	MEMINSPECT_ID_init_mm_pgd,
	MEMINSPECT_ID__sinittext,
	MEMINSPECT_ID__einittext,
	MEMINSPECT_ID__end,
	MEMINSPECT_ID__text,
	MEMINSPECT_ID__stext,
	MEMINSPECT_ID__etext,
	MEMINSPECT_ID_kallsyms_num_syms,
	MEMINSPECT_ID_kallsyms_relative_base,
	MEMINSPECT_ID_kallsyms_offsets,
	MEMINSPECT_ID_kallsyms_names,
	MEMINSPECT_ID_kallsyms_token_table,
	MEMINSPECT_ID_kallsyms_token_index,
	MEMINSPECT_ID_kallsyms_markers,
	MEMINSPECT_ID_kallsyms_seqs_of_names,
	MEMINSPECT_ID_swapper_pg_dir,
	MEMINSPECT_ID_DYNAMIC,
	MEMINSPECT_ID_MAX = 201,
};

#define MEMINSPECT_TYPE_REGULAR		0

#define MEMINSPECT_NOTIFIER_ADD		0
#define MEMINSPECT_NOTIFIER_REMOVE	1

/**
 * struct inspect_entry - memory inspect entry information
 * @id: unique id for this entry
 * @va: virtual address for the memory (pointer)
 * @pa: physical address for the memory
 * @size: size of the memory area of this entry
 * @type: type of the entry (class)
 */
struct inspect_entry {
	enum meminspect_uid	id;
	void			*va;
	phys_addr_t		pa;
	size_t			size;
	unsigned int		type;
};

typedef void (*MEMINSPECT_ITERATOR_CB)(void *priv, const struct inspect_entry *ie);

#ifdef CONFIG_MEMINSPECT
/* .inspect_table section table markers*/
extern const struct inspect_entry __inspect_table[];
extern const struct inspect_entry __inspect_table_end[];

/*
 * Annotate a static variable into inspection table.
 * Can be called multiple times for the same ID, in which case
 * multiple table entries will be created
 */
#define MEMINSPECT_ENTRY(idx, sym, sz)						\
	static const struct inspect_entry __UNIQUE_ID(__inspect_entry_##idx)	\
	__used __section(".inspect_table") = { .id = idx,			\
					       .va = (void *)&(sym),		\
					       .size = (sz),			\
					     }
/*
 * A simple entry is just a variable, the size of the entry is the variable size
 * The variable can also be a pointer, the pointer itself is being added in this
 * case.
 */
#define MEMINSPECT_SIMPLE_ENTRY(sym)	\
	MEMINSPECT_ENTRY(MEMINSPECT_ID_##sym, sym, sizeof(sym))
/*
 * In the case when `sym` is not a variable, but a member of a struct e.g.,
 * and we cannot derive a name from it, a name must be provided.
 */
#define MEMINSPECT_NAMED_ENTRY(name, sym)	\
	MEMINSPECT_ENTRY(MEMINSPECT_ID_##name, sym, sizeof(sym))
/*
 * Create a more complex entry, by registering an arbitrary memory starting
 * at sym. The size is provided as a parameter.
 * This is used e.g. when the symbol is a start of an unknown sized array.
 */
#define MEMINSPECT_AREA_ENTRY(sym, sz) \
	MEMINSPECT_ENTRY(MEMINSPECT_ID_##sym, sym, sz)

/* Iterate through .inspect_table section entries */
#define for_each_meminspect_entry(__entry)		\
	for (__entry = __inspect_table;			\
	     __entry < __inspect_table_end;		\
	     __entry++)

#else
#define MEMINSPECT_ENTRY(...)
#define MEMINSPECT_SIMPLE_ENTRY(...)
#define MEMINSPECT_NAMED_ENTRY(...)
#define MEMINSPECT_AREA_ENTRY(...)
#endif

#ifdef CONFIG_MEMINSPECT

/*
 * Dynamic helpers to register entries.
 * These do not lock the table, so use with caution.
 */
void meminspect_register_id_pa(enum meminspect_uid id, phys_addr_t zone,
			       size_t size, unsigned int type);
void meminspect_table_lock(void);
void meminspect_table_unlock(void);

#define meminspect_register_pa(...) \
	meminspect_register_id_pa(MEMINSPECT_ID_DYNAMIC, __VA_ARGS__, MEMINSPECT_TYPE_REGULAR)

#define meminspect_register_id_va(id, va, size) \
	meminspect_register_id_pa(id, virt_to_phys(va), size, MEMINSPECT_TYPE_REGULAR)

#define meminspect_register_va(...) \
	meminspect_register_id_va(MEMINSPECT_ID_DYNAMIC, __VA_ARGS__)

void meminspect_unregister_pa(phys_addr_t zone, size_t size);
void meminspect_unregister_id(enum meminspect_uid id);

#define meminspect_unregister_va(va, size) \
	meminspect_unregister_pa(virt_to_phys(va), size)

void meminspect_traverse(void *priv, MEMINSPECT_ITERATOR_CB cb);

/*
 * Producers, or registrators, are advised to use the locked API below
 */
#define meminspect_lock_register_pa(...)		\
	{						\
		meminspect_table_lock();		\
		meminspect_register_pa(__VA_ARGS__);	\
		meminspect_table_unlock();		\
	}

#define meminspect_lock_register_id_va(...)		\
	{						\
		meminspect_table_lock();		\
		meminspect_register_id_va(__VA_ARGS__);	\
		meminspect_table_unlock();		\
	}

#define meminspect_lock_register_va(...)		\
	{						\
		meminspect_table_lock();		\
		meminspect_register_va(__VA_ARGS__);	\
		meminspect_table_unlock();		\
	}

#define meminspect_lock_unregister_pa(...)		\
	{						\
		meminspect_table_lock();		\
		meminspect_unregister_pa(__VA_ARGS__);	\
		meminspect_table_unlock();		\
	}

#define meminspect_lock_unregister_va(...)		\
	{						\
		meminspect_table_lock();		\
		meminspect_unregister_va(__VA_ARGS__);	\
		meminspect_table_unlock();		\
	}

#define meminspect_lock_unregister_id(...)		\
	{						\
		meminspect_table_lock();		\
		meminspect_unregister_id(__VA_ARGS__);	\
		meminspect_table_unlock();		\
	}

#define meminspect_lock_traverse(...)			\
	{						\
		meminspect_table_lock();		\
		meminspect_traverse(__VA_ARGS__);	\
		meminspect_table_unlock();		\
	}

int meminspect_notifier_register(struct notifier_block *n);
int meminspect_notifier_unregister(struct notifier_block *n);

#else
static inline void meminspect_register_id_pa(enum meminspect_uid id,
					     phys_addr_t zone,
					     size_t size, unsigned int type)
{
}

static inline void meminspect_table_lock(void)
{
}

static inline void meminspect_table_unlock(void)
{
}

static inline void meminspect_unregister(phys_addr_t zone, size_t size)
{
}

static inline void meminspect_unregister_id(enum meminspect_uid id)
{
}

static inline void meminspect_traverse(MEMINSPECT_ITERATOR_CB cb)
{
}

static inline int meminspect_notifier_register(struct notifier_block *n)
{
	return 0;
}

static inline int meminspect_notifier_unregister(struct notifier_block *n)
{
	return 0;
}

#define meminspect_register_pa(...)
#define meminspect_register_id_va(...)
#define meminspect_register_va(...)
#define meminspect_lock_register_pa(...)
#define meminspect_lock_register_va(...)
#define meminspect_lock_register_id_va(...)
#define meminspect_lock_traverse(...)
#define meminspect_lock_unregister_va(...)
#define meminspect_lock_unregister_pa(...)
#define meminspect_lock_unregister_id(...)
#endif

#endif
