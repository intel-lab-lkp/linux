/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2014 ARM Ltd.
 */
#ifndef __ASM_PTDUMP_H
#define __ASM_PTDUMP_H

#include <asm/kvm_pgtable.h>

#ifdef CONFIG_PTDUMP_CORE

#include <linux/mm_types.h>
#include <linux/seq_file.h>

struct addr_marker {
	unsigned long start_address;
	char *name;
};

struct ptdump_info {
	struct mm_struct		*mm;
	const struct addr_marker	*markers;
	unsigned long			base_addr;
	void (*ptdump_walk)(struct seq_file *s, struct ptdump_info *info);
	int (*ptdump_prepare_walk)(void *file_priv);
	void (*ptdump_end_walk)(void *file_priv);
	size_t				mc_len;
	void				*priv;
};

void ptdump_walk(struct seq_file *s, struct ptdump_info *info);

struct ptdump_info_file_priv {
	struct ptdump_info	info;
	void			*file_priv;
};
#ifdef CONFIG_PTDUMP_DEBUGFS
#define EFI_RUNTIME_MAP_END	DEFAULT_MAP_WINDOW_64
void __init ptdump_debugfs_register(struct ptdump_info *info, const char *name);
void ptdump_debugfs_kvm_register(struct ptdump_info *info, const char *name,
				 struct dentry *d_entry);
#else
static inline void ptdump_debugfs_register(struct ptdump_info *info,
					   const char *name) { }
static inline void ptdump_debugfs_kvm_register(struct ptdump_info *info,
					       const char *name,
					       struct dentry *d_entry) { }
#endif
void ptdump_check_wx(void);
#endif /* CONFIG_PTDUMP_CORE */

#ifdef CONFIG_PTDUMP_STAGE2_DEBUGFS
void ptdump_register_host_stage2(void);
int ptdump_register_guest_stage2(struct kvm *kvm);
void ptdump_unregister_guest_stage2(struct kvm_pgtable *pgt);
#else
static inline void ptdump_register_host_stage2(void) { }
static inline int ptdump_register_guest_stage2(struct kvm *kvm) { return 0; }
static inline void ptdump_unregister_guest_stage2(struct kvm_pgtable *pgt) { }
#endif /* CONFIG_PTDUMP_STAGE2_DEBUGFS */

#ifdef CONFIG_DEBUG_WX
#define debug_checkwx()	ptdump_check_wx()
#else
#define debug_checkwx()	do { } while (0)
#endif

#endif /* __ASM_PTDUMP_H */
