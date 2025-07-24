/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _KMEMDUMP_H
#define _KMEMDUMP_H

enum kmemdump_uid {
	KMEMDUMP_ID_START = 0,
	KMEMDUMP_ID_COREIMAGE_ELF,
	KMEMDUMP_ID_COREIMAGE_VMCOREINFO,
	KMEMDUMP_ID_COREIMAGE_CONFIG,
	KMEMDUMP_ID_COREIMAGE_MEMSECT,
	KMEMDUMP_ID_COREIMAGE__totalram_pages,
	KMEMDUMP_ID_COREIMAGE___cpu_possible_mask,
	KMEMDUMP_ID_COREIMAGE___cpu_present_mask,
	KMEMDUMP_ID_COREIMAGE___cpu_online_mask,
	KMEMDUMP_ID_COREIMAGE___cpu_active_mask,
	KMEMDUMP_ID_COREIMAGE_jiffies_64,
	KMEMDUMP_ID_COREIMAGE_linux_banner,
	KMEMDUMP_ID_COREIMAGE_nr_threads,
	KMEMDUMP_ID_COREIMAGE_nr_irqs,
	KMEMDUMP_ID_COREIMAGE_tainted_mask,
	KMEMDUMP_ID_COREIMAGE_taint_flags,
	KMEMDUMP_ID_COREIMAGE_mem_section,
	KMEMDUMP_ID_COREIMAGE_node_data,
	KMEMDUMP_ID_COREIMAGE_node_states,
	KMEMDUMP_ID_COREIMAGE___per_cpu_offset,
	KMEMDUMP_ID_COREIMAGE_nr_swapfiles,
	KMEMDUMP_ID_COREIMAGE_init_uts_ns,
	KMEMDUMP_ID_COREIMAGE_printk_rb_static,
	KMEMDUMP_ID_COREIMAGE_printk_rb_dynamic,
	KMEMDUMP_ID_COREIMAGE_prb,
	KMEMDUMP_ID_COREIMAGE_prb_descs,
	KMEMDUMP_ID_COREIMAGE_prb_infos,
	KMEMDUMP_ID_COREIMAGE_prb_data,
	KMEMDUMP_ID_COREIMAGE_runqueues,
	KMEMDUMP_ID_COREIMAGE_high_memory,
	KMEMDUMP_ID_COREIMAGE_init_mm,
	KMEMDUMP_ID_COREIMAGE_init_mm_pgd,
	KMEMDUMP_ID_COREIMAGE__sinittext,
	KMEMDUMP_ID_COREIMAGE__einittext,
	KMEMDUMP_ID_COREIMAGE__end,
	KMEMDUMP_ID_COREIMAGE__text,
	KMEMDUMP_ID_COREIMAGE__stext,
	KMEMDUMP_ID_COREIMAGE__etext,
	KMEMDUMP_ID_COREIMAGE_kallsyms_num_syms,
	KMEMDUMP_ID_COREIMAGE_kallsyms_relative_base,
	KMEMDUMP_ID_COREIMAGE_kallsyms_offsets,
	KMEMDUMP_ID_COREIMAGE_kallsyms_names,
	KMEMDUMP_ID_COREIMAGE_kallsyms_token_table,
	KMEMDUMP_ID_COREIMAGE_kallsyms_token_index,
	KMEMDUMP_ID_COREIMAGE_kallsyms_markers,
	KMEMDUMP_ID_COREIMAGE_kallsyms_seqs_of_names,
	KMEMDUMP_ID_COREIMAGE_swapper_pg_dir,
	KMEMDUMP_ID_COREIMAGE_init_uts_ns_name,
	KMEMDUMP_ID_USER_START,
	KMEMDUMP_ID_USER_END,
	KMEMDUMP_ID_NO_ID,
};

#ifdef CONFIG_KMEMDUMP
/**
 * struct kmemdump_zone - region mark zone information
 * @id: unique id for this zone
 * @zone: pointer to the memory area for this zone
 * @size: size of the memory area of this zone
 */
struct kmemdump_zone {
	enum kmemdump_uid	id;
	void			*zone;
	size_t			size;
};

/* kmemdump section table markers*/
extern const struct kmemdump_zone __kmemdump_table[];
extern const struct kmemdump_zone __kmemdump_table_end[];

/* Annotate a variable into the given kmemdump UID */
#define KMEMDUMP_VAR_ID(idx, sym, sz)						\
	static const struct kmemdump_zone __UNIQUE_ID(__kmemdump_entry_##sym)	\
	__used __section(".kmemdump") = { .id = idx,				\
					  .zone = (void *)&(sym),		\
					  .size = (sz),				\
					}
/* Annotate a variable into the KMEMDUMP_ID_COREIMAGE_sym UID */
#define KMEMDUMP_VAR_CORE(sym, sz)						\
	static const struct kmemdump_zone __UNIQUE_ID(__kmemdump_entry_##sym)	\
	__used __section(".kmemdump") = { .id = KMEMDUMP_ID_COREIMAGE_##sym,	\
					  .zone = (void *)&(sym),		\
					  .size = (sz),				\
					}
/* Annotate a variable into the KMEMDUMP_ID_COREIMAGE_name UID */
#define KMEMDUMP_VAR_CORE_NAMED(name, sym, sz)					\
	static const struct kmemdump_zone __UNIQUE_ID(__kmemdump_entry_##name)	\
	__used __section(".kmemdump") = { .id = KMEMDUMP_ID_COREIMAGE_##name,	\
					  .zone = (void *)&(sym),		\
					  .size = (sz),				\
					}
/* Iterate through kmemdump section entries */
#define for_each_kmemdump_entry(__entry)		\
	for (__entry = __kmemdump_table;		\
	     __entry < __kmemdump_table_end;		\
	     __entry++)

#else
#define KMEMDUMP_VAR_ID(...)
#define KMEMDUMP_VAR_CORE(...)
#define KMEMDUMP_VAR_CORE_NAMED(...)
#define KMEMDUMP_VAR_CORE_NAMED(...)
#endif
/*
 * Wrapper over an existing fn allocator
 * It will :
 *	- unregister the memory already registered into kmemdump at the given UID
 *	- register the memory into kmemdump at the given UID
 *	- take an argument for the ID and the wanted size
 */
#define kmemdump_alloc_id_size_replace(id, sz, fn, ...)			\
	({								\
		void *__p = fn(__VA_ARGS__);				\
									\
		if (__p) {						\
			kmemdump_unregister(id);			\
			kmemdump_register_id(id, __p, sz);		\
		}							\
		__p;							\
	})
/*
 * Wrapper over an existing fn allocator
 * It will :
 *	- fail if the given UID is already registered
 *	- register the memory into kmemdump at the given UID
 *	- take an argument for the ID and the wanted size
 */

#define kmemdump_alloc_id_size(id, sz, fn, ...)				\
	({								\
	void *__p = fn(__VA_ARGS__);				\
									\
		if (__p)						\
			kmemdump_register_id(id, __p, sz);		\
		__p;							\
	})

#define kmemdump_alloc_size(...)					\
	kmemdump_alloc_id_size(KMEMDUMP_ID_NO_ID, __VA_ARGS__)

#define kmemdump_phys_alloc_id_size(id, sz, fn, ...)			\
	({								\
		phys_addr_t __p = fn(__VA_ARGS__);			\
									\
		if (__p)						\
			kmemdump_register_id(id, __va(__p), sz);	\
		__p;							\
	})

#define kmemdump_phys_alloc_size(...)					\
	kmemdump_phys_alloc_id_size(KMEMDUMP_ID_NO_ID, __VA_ARGS__)

#define kmemdump_free_id(id, fn, ...)					\
	({								\
		kmemdump_unregister(id);				\
		fn(__VA_ARGS__);					\
	})

#ifdef CONFIG_KMEMDUMP

#define KMEMDUMP_BACKEND_MAX_NAME 128
/**
 * struct kmemdump_backend - region mark backend information
 * @name: the name of the backend
 * @register_region: callback to register region in the backend
 * @unregister_region: callback to unregister region in the backend
 */
struct kmemdump_backend {
	char name[KMEMDUMP_BACKEND_MAX_NAME];
	int (*register_region)(const struct kmemdump_backend *be,
			       enum kmemdump_uid uid, void *vaddr, size_t size);
	int (*unregister_region)(const struct kmemdump_backend *be,
				 enum kmemdump_uid uid);
};

int kmemdump_register_backend(const struct kmemdump_backend *backend);
void kmemdump_unregister_backend(const struct kmemdump_backend *backend);

int kmemdump_register_id(enum kmemdump_uid id, void *zone, size_t size);
void kmemdump_unregister(enum kmemdump_uid id);
#else
static inline int kmemdump_register_id(enum kmemdump_uid uid, void *area,
				       size_t size)
{
	return 0;
}

static inline void kmemdump_unregister(enum kmemdump_uid id)
{
}
#endif

#ifdef CONFIG_KMEMDUMP
#ifdef CONFIG_KMEMDUMP_COREIMAGE
int init_elfheader(void);
void update_elfheader(const struct kmemdump_zone *z);
int clear_elfheader(const struct kmemdump_zone *z);
#else
static inline int init_elfheader(void)
{
	return 0;
}

static inline void update_elfheader(const struct kmemdump_zone *z)
{
}

static inline int clear_elfheader(const struct kmemdump_zone *z)
{
	return 0;
}
#endif
#endif
#endif
