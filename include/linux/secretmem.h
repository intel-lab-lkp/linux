/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _LINUX_SECRETMEM_H
#define _LINUX_SECRETMEM_H

struct secretmem_area {
	void *ptr;
};

#ifdef CONFIG_SECRETMEM

extern const struct address_space_operations secretmem_aops;

static inline bool secretmem_mapping(struct address_space *mapping)
{
	return mapping->a_ops == &secretmem_aops;
}

bool vma_is_secretmem(struct vm_area_struct *vma);
bool secretmem_active(void);

#else

static inline bool vma_is_secretmem(struct vm_area_struct *vma)
{
	return false;
}

static inline bool secretmem_mapping(struct address_space *mapping)
{
	return false;
}

static inline bool secretmem_active(void)
{
	return false;
}

#endif /* CONFIG_SECRETMEM */

#ifdef CONFIG_KERNEL_SECRETMEM

bool can_access_secretmem_vma(struct vm_area_struct *vma);
struct secretmem_area *secretmem_allocate_pages(unsigned int order);
void secretmem_release_pages(struct secretmem_area *data);

#else

static inline bool can_access_secretmem_vma(struct vm_area_struct *vma)
{
	return true;
}

static inline struct secretmem_area *secretmem_allocate_pages(unsigned int order)
{
	return NULL;
}

static inline void secretmem_release_pages(struct secretmem_area *data)
{
	WARN_ONCE(1, "Called secret memory release page without support\n");
}

#endif /* CONFIG_KERNEL_SECRETMEM */

#endif /* _LINUX_SECRETMEM_H */
