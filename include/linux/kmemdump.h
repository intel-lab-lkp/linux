/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _KMEMDUMP_H
#define _KMEMDUMP_H

enum kmemdump_uid {
	KMEMDUMP_ID_START = 0,
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

/* Iterate through kmemdump section entries */
#define for_each_kmemdump_entry(__entry)		\
	for (__entry = __kmemdump_table;		\
	     __entry < __kmemdump_table_end;		\
	     __entry++)

#else
#define KMEMDUMP_VAR_ID(...)
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

#endif
