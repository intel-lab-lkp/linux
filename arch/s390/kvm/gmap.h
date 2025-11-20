/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  KVM guest address space mapping code
 *
 *    Copyright IBM Corp. 2007, 2016, 2025
 *    Author(s): Martin Schwidefsky <schwidefsky@de.ibm.com>
 *               Claudio Imbrenda <imbrenda@linux.ibm.com>
 */

#ifndef ARCH_KVM_S390_GMAP_H
#define ARCH_KVM_S390_GMAP_H

#include "dat.h"

/**
 * struct gmap_struct - guest address space
 * @is_shadow: whether this gmap is a vsie shadow gmap
 * @owns_page_tables: whether this gmap owns all dat levels; normally 1, is 0
 *                    only for ucontrol per-cpu gmaps, since they share the page
 *                    tables with the main gmap.
 * @is_ucontrol: whether this gmap is ucontrol (main gmap or per-cpu gmap)
 * @allow_hpage_1m: whether 1M hugepages are allowed for this gmap,
 *                  independently of whatever page size is used by userspace
 * @allow_hpage_2g: whether 2G hugepages are allowed for this gmap,
 *                  independently of whatever page size is used by userspace
 * @pfault_enabled: whether pfault is enabled for this gmap
 * @removed: whether this shadow gmap is about to be disposed of
 * @initialized: flag to indicate if a shadow guest address space can be used
 * @uses_skeys: indicates if the guest uses storage keys
 * @uses_cmm: indicates if the guest uses cmm
 * @edat_level: the edat level of this shadow gmap
 * @kvm: the vm
 * @asce: the ASCE used by this gmap
 * @list: list head used in children gmaps for the children gmap list
 * @children_lock: protects children and scb_users
 * @children: list of child gmaps of this gmap
 * @scb_users: list of vsie_scb that use this shadow gmap
 * @parent: parent gmap of a child gmap
 * @guest_asce: original ASCE of this shadow gmap
 * @host_to_rmap_lock: protects host_to_rmap
 * @host_to_rmap: radix tree mapping host addresses to guest addresses
 */
struct gmap {
	unsigned char is_shadow:1;
	unsigned char owns_page_tables:1;
	unsigned char is_ucontrol:1;
	bool allow_hpage_1m;
	bool allow_hpage_2g;
	bool pfault_enabled;
	bool removed;
	bool initialized;
	bool uses_skeys;
	bool uses_cmm;
	unsigned char edat_level;
	struct kvm *kvm;
	union asce asce;
	struct list_head list;
	spinlock_t children_lock;	/* protects: children, scb_users */
	struct list_head children;
	struct list_head scb_users;
	struct gmap *parent;
	union asce guest_asce;
	spinlock_t host_to_rmap_lock;	/* protects host_to_rmap */
	struct radix_tree_root host_to_rmap;
};

#define gmap_for_each_rmap_safe(pos, n, head) \
	for (pos = (head); n = pos ? pos->next : NULL, pos; pos = n)

int s390_replace_asce(struct gmap *gmap);
bool _gmap_unmap_prefix(struct gmap *gmap, gfn_t gfn, gfn_t end, bool hint);
bool gmap_age_gfn(struct gmap *gmap, gfn_t start, gfn_t end);
bool gmap_unmap_gfn_range(struct gmap *gmap, struct kvm_memory_slot *slot, gfn_t start, gfn_t end);
int gmap_try_fixup_minor(struct gmap *gmap, struct guest_fault *fault);
struct gmap *gmap_new(struct kvm *kvm, gfn_t limit);
struct gmap *gmap_new_child(struct gmap *parent, gfn_t limit);
void gmap_remove_child(struct gmap *child);
void gmap_dispose(struct gmap *gmap);
int gmap_link(struct kvm_s390_mmu_cache *mc, struct gmap *gmap, struct guest_fault *fault);
void gmap_sync_dirty_log(struct gmap *gmap, gfn_t start, gfn_t end);
int gmap_set_limit(struct gmap *gmap, gfn_t limit);
int gmap_ucas_map(struct gmap *gmap, gfn_t p_gfn, gfn_t c_gfn, unsigned long count);
void gmap_ucas_unmap(struct gmap *gmap, gfn_t c_gfn, unsigned long count);
int gmap_enable_skeys(struct gmap *gmap);
int gmap_pv_destroy_range(struct gmap *gmap, gfn_t start, gfn_t end, bool interruptible);
int gmap_insert_rmap(struct gmap *sg, gfn_t p_gfn, gfn_t r_gfn, int level);
int gmap_protect_rmap(struct kvm_s390_mmu_cache *mc, struct gmap *sg, gfn_t p_gfn, gfn_t r_gfn,
		      kvm_pfn_t pfn, int level, bool wr);
void gmap_set_cmma_all_dirty(struct gmap *gmap);
void _gmap_handle_vsie_unshadow_event(struct gmap *parent, gfn_t gfn);
struct gmap *gmap_create_shadow(struct kvm_s390_mmu_cache *mc, struct gmap *gmap,
				union asce asce, int edat_level);
void gmap_split_huge_pages(struct gmap *gmap);

static inline void gmap_handle_vsie_unshadow_event(struct gmap *parent, gfn_t gfn)
{
	scoped_guard(spinlock, &parent->children_lock)
		_gmap_handle_vsie_unshadow_event(parent, gfn);
}

static inline bool gmap_mkold_prefix(struct gmap *gmap, gfn_t gfn, gfn_t end)
{
	return _gmap_unmap_prefix(gmap, gfn, end, true);
}

static inline bool gmap_unmap_prefix(struct gmap *gmap, gfn_t gfn, gfn_t end)
{
	return _gmap_unmap_prefix(gmap, gfn, end, false);
}

static inline union pgste gmap_ptep_xchg(struct gmap *gmap, union pte *ptep, union pte newpte,
					 union pgste pgste, gfn_t gfn)
{
	lockdep_assert_held(&gmap->kvm->mmu_lock);

	if (pgste.prefix_notif && (newpte.h.p || newpte.h.i)) {
		pgste.prefix_notif = 0;
		gmap_unmap_prefix(gmap, gfn, gfn + 1);
	}
	if (pgste.vsie_notif && (ptep->h.p != newpte.h.p || newpte.h.i)) {
		pgste.vsie_notif = 0;
		gmap_handle_vsie_unshadow_event(gmap, gfn);
	}
	return __dat_ptep_xchg(ptep, pgste, newpte, gfn, gmap->asce, gmap->uses_skeys);
}

static inline void gmap_crstep_xchg(struct gmap *gmap, union crste *crstep, union crste ne,
				    gfn_t gfn)
{
	unsigned long align = 8 + (is_pmd(*crstep) ? 0 : 11);

	lockdep_assert_held(&gmap->kvm->mmu_lock);

	gfn = ALIGN_DOWN(gfn, align);
	if (crste_prefix(*crstep) && (ne.h.p || ne.h.i || !crste_prefix(ne))) {
		ne.s.fc1.prefix_notif = 0;
		gmap_unmap_prefix(gmap, gfn, gfn + align);
	}
	if (crste_leaf(*crstep) && crstep->s.fc1.vsie_notif &&
	    (ne.h.p || ne.h.i || !ne.s.fc1.vsie_notif)) {
		ne.s.fc1.vsie_notif = 0;
		gmap_handle_vsie_unshadow_event(gmap, gfn);
	}
	dat_crstep_xchg(crstep, ne, gfn, gmap->asce);
}

/**
 * gmap_is_shadow_valid() - check if a shadow guest address space matches the
 *                          given properties and is still valid
 * @sg: pointer to the shadow guest address space structure
 * @asce: ASCE for which the shadow table is requested
 * @edat_level: edat level to be used for the shadow translation
 *
 * Returns true if the gmap shadow is still valid and matches the given
 * properties, the caller can continue using it. Returns false otherwise; the
 * caller has to request a new shadow gmap in this case.
 */
static inline bool gmap_is_shadow_valid(struct gmap *sg, union asce asce, int edat_level)
{
	if (sg->removed)
		return false;
	return sg->guest_asce.val == asce.val && sg->edat_level == edat_level;
}

#endif /* ARCH_KVM_S390_GMAP_H */
