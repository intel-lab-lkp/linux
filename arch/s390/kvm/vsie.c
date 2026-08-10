// SPDX-License-Identifier: GPL-2.0
/*
 * kvm nested virtualization support for s390x
 *
 * Copyright IBM Corp. 2016, 2018
 *
 *    Author(s): David Hildenbrand <dahi@linux.vnet.ibm.com>
 */
#include <linux/align.h>
#include <linux/vmalloc.h>
#include <linux/kvm_host.h>
#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/list.h>
#include <linux/bitmap.h>
#include <linux/sched/signal.h>
#include <linux/stddef.h>
#include <linux/io.h>
#include <linux/mman.h>

#include <asm/mmu_context.h>
#include <asm/sclp.h>
#include <asm/nmi.h>
#include <asm/dis.h>
#include <asm/facility.h>
#include "kvm-s390.h"
#include "gaccess.h"
#include "gmap.h"

enum vsie_page_flags {
	VSIE_PAGE_IN_USE = 0,
	VSIE_PAGE_SCB_PINNED = 1,
};

struct vsie_page {
	struct kvm_s390_sie_block scb_s;	/* 0x0000 */
	/* backup info for machine check */
	struct mcck_volatile_info mcck_info;    /* 0x0200 */
	/*
	 * The pinned original scb. Be aware that other VCPUs can modify
	 * it while we read from it. Values that are used for conditions or
	 * are reused conditionally, should be accessed via READ_ONCE.
	 */
	struct kvm_s390_sie_block *scb_o;	/* 0x0218 */
	/*
	 * Flags: must be set/cleared atomically after the vsie page can be
	 * looked up by other CPUs.
	 */
	unsigned long flags;			/* 0x0220 */
	/* address of the last reported fault to guest2 */
	unsigned long fault_addr;		/* 0x0228 */
	/* calculated guest addresses of satellite control blocks */
	gpa_t sca_gpa;				/* 0x0230 */
	gpa_t itdba_gpa;			/* 0x0238 */
	gpa_t gvrd_gpa;				/* 0x0240 */
	gpa_t riccbd_gpa;			/* 0x0248 */
	gpa_t sdnx_gpa;				/* 0x0250 */
	/*
	 * guest address of the original SCB. Remains set for free vsie
	 * pages, so we can properly look them up in our addr_to_page map.
	 */
	gpa_t scb_gpa;				/* 0x0258 */
	/* the shadow gmap in use by the vsie_page */
	struct gmap_cache gmap_cache;		/* 0x0260 */
	struct vsie_sca *vsie_sca;		/* 0x0278 */
	__u8 reserved[0x06f8 - 0x0280];		/* 0x0280 */
	struct kvm_s390_crypto_cb crycb;	/* 0x06f8 */
	__u8 fac[8 + S390_ARCH_FAC_LIST_SIZE_BYTE];/* 0x07f8 */
};

static_assert(sizeof(struct vsie_page) == PAGE_SIZE);
static_assert(offsetof(struct vsie_page, mcck_info) == offsetof(struct sie_page, mcck_info));
static_assert(IS_ALIGNED(offsetof(struct vsie_page, crycb), 8));

struct kvm_address_pair {
	gpa_t gpa;
	hpa_t hpa;
};

enum vsie_sca_flags {
	VSIE_SCA_ESCA = 0,
	VSIE_SCA_SCA_PINNED = 1,
};

struct vsie_sca {
	struct_group(head,
		struct ssca_block	ssca;
	);
	struct vsie_page	*pages[KVM_S390_MAX_VSIE_VCPUS];
	/* The mutex is used to synchronize access to the pages[] */
	struct mutex		mutex;
	atomic_t		ref_count;
	struct_group(tail,
		gpa_t			sca_gpa;
		unsigned long		flags;
		u64			mcn[4];
		unsigned int		sca_o_nr_pages;
		struct kvm_address_pair	sca_o_pages[KVM_S390_MAX_SCA_PAGES];
	);
};

/*
 * SSCA needs to be 32-byte aligned and members before the cpu field need to be on the same page.
 * Forcing it to offset 0 will always fulfill the requirements.
 */
static_assert(!(offsetof(struct vsie_sca, ssca)));

static inline hpa_t sca_o_hpa(struct vsie_sca *vsie_sca)
{
	return vsie_sca->sca_o_pages[0].hpa | (vsie_sca->sca_gpa & ~PAGE_MASK);
}

static inline bool sie_uses_esca(struct kvm_s390_sie_block *scb)
{
	return (scb->ecb2 & ECB2_ESCA);
}

static unsigned long read_scao(struct kvm *kvm, struct kvm_s390_sie_block *scb)
{
	unsigned long vsie_sca = READ_ONCE(scb->scaol) & ~0xfUL;

	if (test_kvm_cpu_feat(kvm, KVM_S390_VM_CPU_FEAT_64BSCAO))
		vsie_sca |= (u64)READ_ONCE(scb->scaoh) << 32;

	return vsie_sca;
}

static void write_scao(struct kvm_s390_sie_block *scb, unsigned long hpa)
{
	scb->scaoh = (u32)((u64)hpa >> 32);
	scb->scaol = (u32)(u64)hpa;
}

static inline bool use_ssca(struct kvm *kvm, struct kvm_s390_sie_block *scb)
{
	if (!kvm->arch.use_ssca)
		return false;
	if (!(scb->eca & ECA_SIGPI) && !(scb->ecb & ECB_SRSI))
		return false;
	if (!read_scao(kvm, scb))
		return false;
	return true;
}

/* trigger a validity icpt for the given scb */
static int set_validity_icpt(struct kvm_s390_sie_block *scb,
			     __u16 reason_code)
{
	scb->ipa = 0x1000;
	scb->ipb = ((__u32) reason_code) << 16;
	scb->icptcode = ICPT_VALIDITY;
	return 1;
}

/* The sca header must not cross pages etc. */
static int validate_scao(struct kvm_vcpu *vcpu, struct kvm_s390_sie_block *scb, gpa_t gpa)
{
	int offset;

	if (gpa < 2 * PAGE_SIZE)
		return set_validity_icpt(scb, 0x0038U);
	if ((gpa & ~0x1fffUL) == kvm_s390_get_prefix(vcpu))
		return set_validity_icpt(scb, 0x0011U);

	if (sie_uses_esca(scb))
		offset = offsetof(struct esca_block, cpu[0]) - 1;
	else
		offset = offsetof(struct bsca_block, cpu[0]) - 1;
	if ((gpa & PAGE_MASK) != ((gpa + offset) & PAGE_MASK))
		return set_validity_icpt(scb, 0x003bU);
	return 0;
}

/* mark the prefix as unmapped, this will block the VSIE */
static void prefix_unmapped(struct vsie_page *vsie_page)
{
	atomic_or(PROG_REQUEST, &vsie_page->scb_s.prog20);
}

/* mark the prefix as unmapped and wait until the VSIE has been left */
static void prefix_unmapped_sync(struct vsie_page *vsie_page)
{
	prefix_unmapped(vsie_page);
	if (vsie_page->scb_s.prog0c & PROG_IN_SIE)
		atomic_or(CPUSTAT_STOP_INT, &vsie_page->scb_s.cpuflags);
	while (vsie_page->scb_s.prog0c & PROG_IN_SIE)
		cpu_relax();
}

/* mark the prefix as mapped, this will allow the VSIE to run */
static void prefix_mapped(struct vsie_page *vsie_page)
{
	atomic_andnot(PROG_REQUEST, &vsie_page->scb_s.prog20);
}

/* test if the prefix is mapped into the gmap shadow */
static int prefix_is_mapped(struct vsie_page *vsie_page)
{
	return !(atomic_read(&vsie_page->scb_s.prog20) & PROG_REQUEST);
}

static void release_gmap_shadow(struct vsie_page *vsie_page)
{
	struct gmap *gmap = vsie_page->gmap_cache.gmap;

	lockdep_assert_held(&gmap->kvm->arch.gmap->children_lock);

	list_del(&vsie_page->gmap_cache.list);
	vsie_page->gmap_cache.gmap = NULL;
	prefix_unmapped(vsie_page);

	if (list_empty(&gmap->scb_users)) {
		gmap_remove_child(gmap);
		gmap_put(gmap);
	}
}

static void release_gmap_shadow_safe(struct kvm *kvm, struct vsie_page *vsie_page)
{
	if (vsie_page->gmap_cache.gmap) {
		guard(spinlock)(&kvm->arch.gmap->children_lock);
		if (vsie_page->gmap_cache.gmap)
			release_gmap_shadow(vsie_page);
	}
}

static struct gmap *acquire_gmap_shadow(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page)
{
	union ctlreg0 cr0;
	struct gmap *gmap;
	union asce asce;
	int edat;

	asce.val = vcpu->arch.sie_block->gcr[1];
	cr0.val = vcpu->arch.sie_block->gcr[0];
	edat = cr0.edat && test_kvm_facility(vcpu->kvm, 8);
	edat += edat && test_kvm_facility(vcpu->kvm, 78);

	scoped_guard(spinlock, &vcpu->kvm->arch.gmap->children_lock) {
		gmap = vsie_page->gmap_cache.gmap;
		if (gmap) {
			/*
			 * ASCE or EDAT could have changed since last icpt, or the gmap
			 * we're holding has been unshadowed. If the gmap is still valid,
			 * we can safely reuse it.
			 */
			if (gmap_is_shadow_valid(gmap, asce, edat)) {
				vcpu->kvm->stat.gmap_shadow_reuse++;
				gmap_get(gmap);
				return gmap;
			}
			/* release the old shadow and mark the prefix as unmapped */
			release_gmap_shadow(vsie_page);
		}
	}
again:
	gmap = gmap_create_shadow(vcpu->arch.mc, vcpu->kvm->arch.gmap, asce, edat);
	if (IS_ERR(gmap))
		return gmap;
	scoped_guard(spinlock, &vcpu->kvm->arch.gmap->children_lock) {
		/* unlikely race condition, remove the previous shadow */
		if (vsie_page->gmap_cache.gmap)
			release_gmap_shadow(vsie_page);
		if (!gmap->parent) {
			gmap_put(gmap);
			goto again;
		}
		vcpu->kvm->stat.gmap_shadow_create++;
		list_add(&vsie_page->gmap_cache.list, &gmap->scb_users);
		vsie_page->gmap_cache.gmap = gmap;
		prefix_unmapped(vsie_page);
	}
	return gmap;
}

/*
 * Pin the guest page given by gpa and set hpa to the pinned host address.
 * Will always be pinned writable.
 *
 * Returns: - 0 on success
 *          - -EINVAL if the gpa is not valid guest storage
 */
static int pin_guest_page(struct kvm *kvm, gpa_t gpa, hpa_t *hpa)
{
	struct page *page;

	page = gfn_to_page(kvm, gpa_to_gfn(gpa));
	if (!page)
		return -EINVAL;
	*hpa = (hpa_t)page_to_phys(page) + (gpa & ~PAGE_MASK);
	return 0;
}

/* Unpins a page previously pinned via pin_guest_page, marking it as dirty. */
static void unpin_guest_page(struct kvm *kvm, gpa_t gpa, hpa_t hpa)
{
	kvm_release_page_dirty(pfn_to_page(phys_to_pfn(hpa)));
	/* mark the page always as dirty for migration */
	mark_page_dirty(kvm, gpa_to_gfn(gpa));
}

/* unpin multiple guest pages pinned with pin_guest_pages() */
static void unpin_guest_pages(struct kvm *kvm, struct kvm_address_pair *addr, unsigned int nr_pages)
{
	int i;

	for (i = 0; i < nr_pages; i++) {
		unpin_guest_page(kvm, addr[i].gpa, addr[i].hpa);
		addr[i].gpa = 0;
		addr[i].hpa = 0;
	}
}

/*
 * pin nr_pages consecutive guest pages
 *
 * Returns number of pages pinned or -EFAULT.
 */
static int pin_guest_pages(struct kvm *kvm, gpa_t gpa, unsigned int nr_pages,
			   struct kvm_address_pair *addr)
{
	hpa_t hpa;
	int i, rc;

	gpa = gpa & PAGE_MASK;

	/* the guest pages may not be mapped continuously, so pin each page */
	for (i = 0; i < nr_pages; i++) {
		rc = pin_guest_page(kvm, gpa + PAGE_SIZE * i, &hpa);
		if (rc)
			goto err;
		addr[i].gpa = gpa + PAGE_SIZE * i;
		addr[i].hpa = hpa;
	}
	return i;

err:
	unpin_guest_pages(kvm, addr, i);
	return -EFAULT;
}

/* copy the updated intervention request bits into the shadow scb */
static void update_intervention_requests(struct vsie_page *vsie_page)
{
	const int bits = CPUSTAT_STOP_INT | CPUSTAT_IO_INT | CPUSTAT_EXT_INT;
	int cpuflags;

	cpuflags = atomic_read(&vsie_page->scb_o->cpuflags);
	atomic_andnot(bits, &vsie_page->scb_s.cpuflags);
	atomic_or(cpuflags & bits, &vsie_page->scb_s.cpuflags);
}

/* shadow (filter and validate) the cpuflags  */
static int prepare_cpuflags(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page)
{
	struct kvm_s390_sie_block *scb_s = &vsie_page->scb_s;
	struct kvm_s390_sie_block *scb_o = vsie_page->scb_o;
	int newflags, cpuflags = atomic_read(&scb_o->cpuflags);

	/* we don't allow ESA/390 guests unless explicitly enabled */
	if (!(cpuflags & CPUSTAT_ZARCH) && !vcpu->kvm->arch.allow_vsie_esamode)
		return set_validity_icpt(scb_s, 0x0001U);

	if (cpuflags & (CPUSTAT_RRF | CPUSTAT_MCDS))
		return set_validity_icpt(scb_s, 0x0001U);
	else if (cpuflags & (CPUSTAT_SLSV | CPUSTAT_SLSR))
		return set_validity_icpt(scb_s, 0x0007U);

	/* intervention requests will be set later */
	newflags = 0;
	if (cpuflags & CPUSTAT_ZARCH)
		newflags = CPUSTAT_ZARCH;
	if (cpuflags & CPUSTAT_GED && test_kvm_facility(vcpu->kvm, 8))
		newflags |= CPUSTAT_GED;
	if (cpuflags & CPUSTAT_GED2 && test_kvm_facility(vcpu->kvm, 78)) {
		if (cpuflags & CPUSTAT_GED)
			return set_validity_icpt(scb_s, 0x0001U);
		newflags |= CPUSTAT_GED2;
	}
	if (test_kvm_cpu_feat(vcpu->kvm, KVM_S390_VM_CPU_FEAT_GPERE))
		newflags |= cpuflags & CPUSTAT_P;
	if (test_kvm_cpu_feat(vcpu->kvm, KVM_S390_VM_CPU_FEAT_GSLS))
		newflags |= cpuflags & CPUSTAT_SM;
	if (test_kvm_cpu_feat(vcpu->kvm, KVM_S390_VM_CPU_FEAT_IBS))
		newflags |= cpuflags & CPUSTAT_IBS;
	if (test_kvm_cpu_feat(vcpu->kvm, KVM_S390_VM_CPU_FEAT_KSS))
		newflags |= cpuflags & CPUSTAT_KSS;

	atomic_set(&scb_s->cpuflags, newflags);
	return 0;
}
/* Copy to APCB FORMAT1 from APCB FORMAT0 */
static int setup_apcb10(struct kvm_vcpu *vcpu, struct kvm_s390_apcb1 *apcb_s,
			unsigned long crycb_gpa, struct kvm_s390_apcb1 *apcb_h)
{
	struct kvm_s390_apcb0 tmp;
	unsigned long apcb_gpa;

	apcb_gpa = crycb_gpa + offsetof(struct kvm_s390_crypto_cb, apcb0);

	if (read_guest_real(vcpu, apcb_gpa, &tmp,
			    sizeof(struct kvm_s390_apcb0)))
		return -EFAULT;

	apcb_s->apm[0] = apcb_h->apm[0] & tmp.apm[0];
	apcb_s->aqm[0] = apcb_h->aqm[0] & tmp.aqm[0] & 0xffff000000000000UL;
	apcb_s->adm[0] = apcb_h->adm[0] & tmp.adm[0] & 0xffff000000000000UL;

	return 0;

}

/**
 * setup_apcb00 - Copy to APCB FORMAT0 from APCB FORMAT0
 * @vcpu: pointer to the virtual CPU
 * @apcb_s: pointer to start of apcb in the shadow crycb
 * @crycb_gpa: guest physical address to start of original guest crycb
 * @apcb_h: pointer to start of apcb in the guest1
 *
 * Returns 0 and -EFAULT on error reading guest apcb
 */
static int setup_apcb00(struct kvm_vcpu *vcpu, unsigned long *apcb_s,
			unsigned long crycb_gpa, unsigned long *apcb_h)
{
	unsigned long apcb_gpa;

	apcb_gpa = crycb_gpa + offsetof(struct kvm_s390_crypto_cb, apcb0);

	if (read_guest_real(vcpu, apcb_gpa, apcb_s,
			    sizeof(struct kvm_s390_apcb0)))
		return -EFAULT;

	bitmap_and(apcb_s, apcb_s, apcb_h,
		   BITS_PER_BYTE * sizeof(struct kvm_s390_apcb0));

	return 0;
}

/**
 * setup_apcb11 - Copy the FORMAT1 APCB from the guest to the shadow CRYCB
 * @vcpu: pointer to the virtual CPU
 * @apcb_s: pointer to start of apcb in the shadow crycb
 * @crycb_gpa: guest physical address to start of original guest crycb
 * @apcb_h: pointer to start of apcb in the host
 *
 * Returns 0 and -EFAULT on error reading guest apcb
 */
static int setup_apcb11(struct kvm_vcpu *vcpu, unsigned long *apcb_s,
			unsigned long crycb_gpa,
			unsigned long *apcb_h)
{
	unsigned long apcb_gpa;

	apcb_gpa = crycb_gpa + offsetof(struct kvm_s390_crypto_cb, apcb1);

	if (read_guest_real(vcpu, apcb_gpa, apcb_s,
			    sizeof(struct kvm_s390_apcb1)))
		return -EFAULT;

	bitmap_and(apcb_s, apcb_s, apcb_h,
		   BITS_PER_BYTE * sizeof(struct kvm_s390_apcb1));

	return 0;
}

/**
 * setup_apcb - Create a shadow copy of the apcb.
 * @vcpu: pointer to the virtual CPU
 * @crycb_s: pointer to shadow crycb
 * @crycb_gpa: guest physical address of original guest crycb
 * @crycb_h: pointer to the host crycb
 * @fmt_o: format of the original guest crycb.
 * @fmt_h: format of the host crycb.
 *
 * Checks the compatibility between the guest and host crycb and calls the
 * appropriate copy function.
 *
 * Return 0 or an error number if the guest and host crycb are incompatible.
 */
static int setup_apcb(struct kvm_vcpu *vcpu, struct kvm_s390_crypto_cb *crycb_s,
	       const u32 crycb_gpa,
	       struct kvm_s390_crypto_cb *crycb_h,
	       int fmt_o, int fmt_h)
{
	switch (fmt_o) {
	case CRYCB_FORMAT2:
		if ((crycb_gpa & PAGE_MASK) != ((crycb_gpa + 256) & PAGE_MASK))
			return -EACCES;
		if (fmt_h != CRYCB_FORMAT2)
			return -EINVAL;
		return setup_apcb11(vcpu, (unsigned long *)&crycb_s->apcb1,
				    crycb_gpa,
				    (unsigned long *)&crycb_h->apcb1);
	case CRYCB_FORMAT1:
		switch (fmt_h) {
		case CRYCB_FORMAT2:
			return setup_apcb10(vcpu, &crycb_s->apcb1,
					    crycb_gpa,
					    &crycb_h->apcb1);
		case CRYCB_FORMAT1:
			return setup_apcb00(vcpu,
					    (unsigned long *) &crycb_s->apcb0,
					    crycb_gpa,
					    (unsigned long *) &crycb_h->apcb0);
		}
		break;
	case CRYCB_FORMAT0:
		if ((crycb_gpa & PAGE_MASK) != ((crycb_gpa + 32) & PAGE_MASK))
			return -EACCES;

		switch (fmt_h) {
		case CRYCB_FORMAT2:
			return setup_apcb10(vcpu, &crycb_s->apcb1,
					    crycb_gpa,
					    &crycb_h->apcb1);
		case CRYCB_FORMAT1:
		case CRYCB_FORMAT0:
			return setup_apcb00(vcpu,
					    (unsigned long *) &crycb_s->apcb0,
					    crycb_gpa,
					    (unsigned long *) &crycb_h->apcb0);
		}
	}
	return -EINVAL;
}

/**
 * shadow_crycb - Create a shadow copy of the crycb block
 * @vcpu: a pointer to the virtual CPU
 * @vsie_page: a pointer to internal date used for the vSIE
 *
 * Create a shadow copy of the crycb block and setup key wrapping, if
 * requested for guest 3 and enabled for guest 2.
 *
 * We accept format-1 or format-2, but we convert format-1 into format-2
 * in the shadow CRYCB.
 * Using format-2 enables the firmware to choose the right format when
 * scheduling the SIE.
 * There is nothing to do for format-0.
 *
 * This function centralize the issuing of set_validity_icpt() for all
 * the subfunctions working on the crycb.
 *
 * Returns: - 0 if shadowed or nothing to do
 *          - > 0 if control has to be given to guest 2
 */
static int shadow_crycb(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page)
{
	struct kvm_s390_sie_block *scb_s = &vsie_page->scb_s;
	struct kvm_s390_sie_block *scb_o = vsie_page->scb_o;
	const uint32_t crycbd_o = READ_ONCE(scb_o->crycbd);
	const u32 crycb_addr = crycbd_o & 0x7ffffff8U;
	unsigned long *b1, *b2;
	u8 ecb3_flags;
	u32 ecd_flags;
	int apie_h;
	int apie_s;
	int key_msk = test_kvm_facility(vcpu->kvm, 76);
	int fmt_o = crycbd_o & CRYCB_FORMAT_MASK;
	int fmt_h = vcpu->arch.sie_block->crycbd & CRYCB_FORMAT_MASK;
	int ret = 0;

	scb_s->crycbd = 0;

	apie_h = vcpu->arch.sie_block->eca & ECA_APIE;
	apie_s = apie_h & scb_o->eca;
	if (!apie_s && (!key_msk || (fmt_o == CRYCB_FORMAT0)))
		return 0;

	if (!crycb_addr)
		return set_validity_icpt(scb_s, 0x0039U);

	if (fmt_o == CRYCB_FORMAT1)
		if ((crycb_addr & PAGE_MASK) !=
		    ((crycb_addr + 128) & PAGE_MASK))
			return set_validity_icpt(scb_s, 0x003CU);

	if (apie_s) {
		ret = setup_apcb(vcpu, &vsie_page->crycb, crycb_addr,
				 vcpu->kvm->arch.crypto.crycb,
				 fmt_o, fmt_h);
		if (ret)
			goto end;
		scb_s->eca |= scb_o->eca & ECA_APIE;
	}

	/* we may only allow it if enabled for guest 2 */
	ecb3_flags = scb_o->ecb3 & vcpu->arch.sie_block->ecb3 &
		     (ECB3_AES | ECB3_DEA);
	ecd_flags = scb_o->ecd & vcpu->arch.sie_block->ecd &
		     (ECD_ECC | ECD_HMAC);
	if (!ecb3_flags && !ecd_flags)
		goto end;

	/* copy only the wrapping keys */
	if (read_guest_real(vcpu, crycb_addr + 72,
			    vsie_page->crycb.dea_wrapping_key_mask, 56))
		return set_validity_icpt(scb_s, 0x0035U);

	scb_s->ecb3 |= ecb3_flags;
	scb_s->ecd |= ecd_flags;

	/* xor both blocks in one run */
	b1 = (unsigned long *) vsie_page->crycb.dea_wrapping_key_mask;
	b2 = (unsigned long *)
			    vcpu->kvm->arch.crypto.crycb->dea_wrapping_key_mask;
	/* as 56%8 == 0, bitmap_xor won't overwrite any data */
	bitmap_xor(b1, b1, b2, BITS_PER_BYTE * 56);
end:
	switch (ret) {
	case -EINVAL:
		return set_validity_icpt(scb_s, 0x0022U);
	case -EFAULT:
		return set_validity_icpt(scb_s, 0x0035U);
	case -EACCES:
		return set_validity_icpt(scb_s, 0x003CU);
	}
	scb_s->crycbd = (u32)virt_to_phys(&vsie_page->crycb) | CRYCB_FORMAT2;
	return 0;
}

static void shadow_esa(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page)
{
	struct kvm_s390_sie_block *scb_s = &vsie_page->scb_s;

	/* Ensure these bits are indeed turned off */
	scb_s->eca &= ~ECA_VX;
	scb_s->ecb &= ~(ECB_GS | ECB_TE);
	scb_s->ecb3 &= ~ECB3_RI;
	scb_s->ecd &= ~ECD_HOSTREGMGMT;
}

/* shadow (round up/down) the ibc to avoid validity icpt */
static void prepare_ibc(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page)
{
	struct kvm_s390_sie_block *scb_s = &vsie_page->scb_s;
	struct kvm_s390_sie_block *scb_o = vsie_page->scb_o;
	/* READ_ONCE does not work on bitfields - use a temporary variable */
	const uint32_t __new_ibc = scb_o->ibc;
	const uint32_t new_ibc = READ_ONCE(__new_ibc) & 0x0fffU;
	__u64 min_ibc = (sclp.ibc >> 16) & 0x0fffU;

	scb_s->ibc = 0;
	/* ibc installed in g2 and requested for g3 */
	if (vcpu->kvm->arch.model.ibc && new_ibc) {
		scb_s->ibc = new_ibc;
		/* takte care of the minimum ibc level of the machine */
		if (scb_s->ibc < min_ibc)
			scb_s->ibc = min_ibc;
		/* take care of the maximum ibc level set for the guest */
		if (scb_s->ibc > vcpu->kvm->arch.model.ibc)
			scb_s->ibc = vcpu->kvm->arch.model.ibc;
	}
}

/* unshadow the scb, copying parameters back to the real scb */
static void unshadow_scb(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page)
{
	struct kvm_s390_sie_block *scb_s = &vsie_page->scb_s;
	struct kvm_s390_sie_block *scb_o = vsie_page->scb_o;

	/* interception */
	scb_o->icptcode = scb_s->icptcode;
	scb_o->icptstatus = scb_s->icptstatus;
	scb_o->ipa = scb_s->ipa;
	scb_o->ipb = scb_s->ipb;
	scb_o->gbea = scb_s->gbea;

	/* timer */
	scb_o->cputm = scb_s->cputm;
	scb_o->ckc = scb_s->ckc;
	scb_o->todpr = scb_s->todpr;

	/* guest state */
	scb_o->gpsw = scb_s->gpsw;
	scb_o->gg14 = scb_s->gg14;
	scb_o->gg15 = scb_s->gg15;
	memcpy(scb_o->gcr, scb_s->gcr, 128);
	scb_o->pp = scb_s->pp;

	/* branch prediction */
	if (test_kvm_facility(vcpu->kvm, 82)) {
		scb_o->fpf &= ~FPF_BPBC;
		scb_o->fpf |= scb_s->fpf & FPF_BPBC;
	}

	/* interrupt intercept */
	switch (scb_s->icptcode) {
	case ICPT_PROGI:
	case ICPT_INSTPROGI:
	case ICPT_EXTINT:
		memcpy((void *)((u64)scb_o + 0xc0),
		       (void *)((u64)scb_s + 0xc0), 0xf0 - 0xc0);
		break;
	}

	if (scb_s->ihcpu != 0xffffU)
		scb_o->ihcpu = scb_s->ihcpu;
}

/*
 * Setup the shadow scb by copying and checking the relevant parts of the g2
 * provided scb.
 *
 * Returns: - 0 if the scb has been shadowed
 *          - > 0 if control has to be given to guest 2
 */
static int shadow_scb(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page)
{
	struct kvm_s390_sie_block *scb_o = vsie_page->scb_o;
	struct kvm_s390_sie_block *scb_s = &vsie_page->scb_s;
	/* READ_ONCE does not work on bitfields - use a temporary variable */
	const uint32_t __new_prefix = scb_o->prefix;
	uint32_t new_prefix = READ_ONCE(__new_prefix);
	const bool wants_tx = READ_ONCE(scb_o->ecb) & ECB_TE;
	bool had_tx = scb_s->ecb & ECB_TE;
	unsigned long new_mso = 0;
	int rc;

	/* make sure we don't have any leftovers when reusing the scb */
	scb_s->icptcode = 0;
	scb_s->eca = 0;
	scb_s->ecb = 0;
	scb_s->ecb2 = 0;
	scb_s->ecb3 = 0;
	scb_s->ecd = 0;
	scb_s->fac = 0;
	scb_s->fpf = 0;

	rc = prepare_cpuflags(vcpu, vsie_page);
	if (rc)
		goto out;

	/* timer */
	scb_s->cputm = scb_o->cputm;
	scb_s->ckc = scb_o->ckc;
	scb_s->todpr = scb_o->todpr;
	scb_s->epoch = scb_o->epoch;

	/* guest state */
	scb_s->gpsw = scb_o->gpsw;
	scb_s->gg14 = scb_o->gg14;
	scb_s->gg15 = scb_o->gg15;
	memcpy(scb_s->gcr, scb_o->gcr, 128);
	scb_s->pp = scb_o->pp;

	/* interception / execution handling */
	scb_s->gbea = scb_o->gbea;
	scb_s->lctl = scb_o->lctl;
	scb_s->svcc = scb_o->svcc;
	scb_s->ictl = scb_o->ictl;
	/*
	 * SKEY handling functions can't deal with false setting of PTE invalid
	 * bits. Therefore we cannot provide interpretation and would later
	 * have to provide own emulation handlers.
	 */
	if (!(atomic_read(&scb_s->cpuflags) & CPUSTAT_KSS))
		scb_s->ictl |= ICTL_ISKE | ICTL_SSKE | ICTL_RRBE;

	scb_s->icpua = scb_o->icpua;

	if (!(atomic_read(&scb_s->cpuflags) & CPUSTAT_ZARCH))
		new_prefix &= GUEST_PREFIX_MASK_ESA;
	else
		new_prefix &= GUEST_PREFIX_MASK_ZARCH;

	if (!(atomic_read(&scb_s->cpuflags) & CPUSTAT_SM))
		new_mso = READ_ONCE(scb_o->mso) & 0xfffffffffff00000UL;
	/* if the hva of the prefix changes, we have to remap the prefix */
	if (scb_s->mso != new_mso || scb_s->prefix != new_prefix)
		prefix_unmapped(vsie_page);
	 /* SIE will do mso/msl validity and exception checks for us */
	scb_s->msl = scb_o->msl & 0xfffffffffff00000UL;
	scb_s->mso = new_mso;
	scb_s->prefix = new_prefix;

	/* We have to definitely flush the tlb if this scb never ran */
	if (scb_s->ihcpu != 0xffffU)
		scb_s->ihcpu = scb_o->ihcpu;

	/* MVPG and Protection Exception Interpretation are always available */
	scb_s->eca |= scb_o->eca & (ECA_MVPGI | ECA_PROTEXCI);
	/* Host-protection-interruption introduced with ESOP */
	if (test_kvm_cpu_feat(vcpu->kvm, KVM_S390_VM_CPU_FEAT_ESOP))
		scb_s->ecb |= scb_o->ecb & ECB_HOSTPROTINT;
	/*
	 * CPU Topology
	 * This facility only uses the utility field of the SCA and none of
	 * the cpu entries that are problematic with the other interpretation
	 * facilities so we can pass it through
	 */
	if (test_kvm_facility(vcpu->kvm, 11))
		scb_s->ecb |= scb_o->ecb & ECB_PTF;
	/* transactional execution */
	if (test_kvm_facility(vcpu->kvm, 73) && wants_tx) {
		/* remap the prefix is tx is toggled on */
		if (!had_tx)
			prefix_unmapped(vsie_page);
		scb_s->ecb |= ECB_TE;
	}
	/* specification exception interpretation */
	scb_s->ecb |= scb_o->ecb & ECB_SPECI;
	/* branch prediction */
	if (test_kvm_facility(vcpu->kvm, 82))
		scb_s->fpf |= scb_o->fpf & FPF_BPBC;
	/* SIMD */
	if (test_kvm_facility(vcpu->kvm, 129)) {
		scb_s->eca |= scb_o->eca & ECA_VX;
		scb_s->ecd |= scb_o->ecd & ECD_HOSTREGMGMT;
	}
	/* Run-time-Instrumentation */
	if (test_kvm_facility(vcpu->kvm, 64))
		scb_s->ecb3 |= scb_o->ecb3 & ECB3_RI;
	/* Instruction Execution Prevention */
	if (test_kvm_facility(vcpu->kvm, 130))
		scb_s->ecb2 |= scb_o->ecb2 & ECB2_IEP;
	/* Guarded Storage */
	if (test_kvm_facility(vcpu->kvm, 133)) {
		scb_s->ecb |= scb_o->ecb & ECB_GS;
		scb_s->ecd |= scb_o->ecd & ECD_HOSTREGMGMT;
	}
	if (test_kvm_cpu_feat(vcpu->kvm, KVM_S390_VM_CPU_FEAT_SIIF))
		scb_s->eca |= scb_o->eca & ECA_SII;
	if (test_kvm_cpu_feat(vcpu->kvm, KVM_S390_VM_CPU_FEAT_IB))
		scb_s->eca |= scb_o->eca & ECA_IB;
	if (test_kvm_cpu_feat(vcpu->kvm, KVM_S390_VM_CPU_FEAT_CEI))
		scb_s->eca |= scb_o->eca & ECA_CEI;
	/* Epoch Extension */
	if (test_kvm_facility(vcpu->kvm, 139)) {
		scb_s->ecd |= scb_o->ecd & ECD_MEF;
		scb_s->epdx = scb_o->epdx;
	}

	/* etoken */
	if (test_kvm_facility(vcpu->kvm, 156))
		scb_s->ecd |= scb_o->ecd & ECD_ETOKENF;

	scb_s->hpid = HPID_VSIE;
	scb_s->cpnc = scb_o->cpnc;

	if (!(atomic_read(&scb_s->cpuflags) & CPUSTAT_ZARCH))
		shadow_esa(vcpu, vsie_page);

	prepare_ibc(vcpu, vsie_page);
	rc = shadow_crycb(vcpu, vsie_page);
out:
	if (rc)
		unshadow_scb(vcpu, vsie_page);
	return rc;
}

/* unpin the scb provided by guest 2, marking it as dirty */
static void unpin_scb(struct kvm *kvm, struct vsie_page *vsie_page)
{
	hpa_t hpa;

	if (!test_bit(VSIE_PAGE_SCB_PINNED, &vsie_page->flags))
		return;

	hpa = virt_to_phys(vsie_page->scb_o);
	if (hpa)
		unpin_guest_page(kvm, vsie_page->scb_gpa, hpa);
	vsie_page->scb_o = NULL;
	__clear_bit(VSIE_PAGE_SCB_PINNED, &vsie_page->flags);
}

/*
 * Pin the scb at gpa provided by guest 2 at vsie_page->scb_o.
 *
 * Returns: - 0 if the scb was pinned.
 *          - > 0 if control has to be given to guest 2
 */
static int pin_scb(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page)
{
	hpa_t hpa;
	int rc;

	if (test_bit(VSIE_PAGE_SCB_PINNED, &vsie_page->flags))
		return 0;

	rc = pin_guest_page(vcpu->kvm, vsie_page->scb_gpa, &hpa);
	if (rc) {
		rc = kvm_s390_inject_program_int(vcpu, PGM_ADDRESSING);
		WARN_ON_ONCE(rc);
		return 1;
	}
	vsie_page->scb_o = phys_to_virt(hpa);
	__set_bit(VSIE_PAGE_SCB_PINNED, &vsie_page->flags);
	return 0;
}

/*
 * Unpin g2 original sca in g1 memory.
 *
 * Called with vsie_sca_lock held.
 */
static void unpin_sca(struct kvm *kvm, struct vsie_sca *vsie_sca)
{
	if (!test_bit(VSIE_SCA_SCA_PINNED, &vsie_sca->flags))
		return;

	unpin_guest_pages(kvm, vsie_sca->sca_o_pages, vsie_sca->sca_o_nr_pages);
	vsie_sca->sca_o_nr_pages = 0;

	__clear_bit(VSIE_SCA_SCA_PINNED, &vsie_sca->flags);
}

/*
 * Pin g2 original sca in g1 memory.
 *
 * Called with vsie_sca_lock held.
 */
static int pin_sca(struct kvm *kvm, struct vsie_sca *vsie_sca)
{
	bool is_esca = test_bit(VSIE_SCA_ESCA, &vsie_sca->flags);
	gpa_t offset = vsie_sca->sca_gpa & ~PAGE_MASK;
	int nr_pages;

	if (test_bit(VSIE_SCA_SCA_PINNED, &vsie_sca->flags))
		return 0;

	if (is_esca) {
		nr_pages = 4;
		if (offset + sizeof(struct esca_block) > 4 * PAGE_SIZE)
			nr_pages = 5;
	} else {
		nr_pages = 1;
		if (offset + sizeof(struct bsca_block) > PAGE_SIZE)
			nr_pages = 2;
	}

	vsie_sca->sca_o_nr_pages = pin_guest_pages(kvm, vsie_sca->sca_gpa, nr_pages,
						   vsie_sca->sca_o_pages);
	if (WARN_ON_ONCE(vsie_sca->sca_o_nr_pages != nr_pages))
		return -EIO;
	__set_bit(VSIE_SCA_SCA_PINNED, &vsie_sca->flags);

	return 0;
}

static int get_sca_entry_addr(struct kvm *kvm, struct vsie_sca *vsie_sca, u16 cpu_nr, gpa_t *gpa,
			      hpa_t *hpa)
{
	hpa_t cpu_offset, offset;
	int pn;

	/*
	 * We cannot simply access the hva since the esca_block has typically
	 * 4 pages (arch max 5 pages) that might not be continuous in g1 memory.
	 * The bsca_block may also be stretched over two pages. Only the header
	 * is guaranteed to be on the same page.
	 */
	if (test_bit(VSIE_SCA_ESCA, &vsie_sca->flags))
		cpu_offset = offsetof(struct esca_block, cpu[cpu_nr]);
	else
		cpu_offset = offsetof(struct bsca_block, cpu[cpu_nr]);
	pn = ((vsie_sca->sca_gpa & ~PAGE_MASK) + cpu_offset) >> PAGE_SHIFT;
	offset = (vsie_sca->sca_gpa + cpu_offset) & ~PAGE_MASK;
	if (WARN_ON_ONCE(pn >= vsie_sca->sca_o_nr_pages))
		return -EINVAL;

	if (gpa)
		*gpa = vsie_sca->sca_o_pages[pn].gpa | offset;
	if (hpa)
		*hpa = vsie_sca->sca_o_pages[pn].hpa | offset;
	return 0;
}

static void put_vsie_sca(struct vsie_sca *vsie_sca)
{
	if (!vsie_sca)
		return;

	WARN_ON_ONCE(atomic_dec_return(&vsie_sca->ref_count) < 0);
}

/*
 * Try to find the address of an existing shadow system control area.
 * @sca_o_gpa: original system control area address; guest-2 physical
 *
 * Called with lock on vsie_sca_lock.
 */
static struct vsie_sca *get_existing_vsie_sca(struct kvm *kvm, gpa_t sca_o_gpa)
{
	struct vsie_sca *vsie_sca = xa_load(&kvm->arch.vsie.osca_to_sca,
					    sca_o_gpa >> SCA_ALIGNMENT_SHIFT);

	WARN_ON_ONCE(vsie_sca && atomic_inc_return(&vsie_sca->ref_count) < 1);
	return vsie_sca;
}

/* Try to find and get a currently unused vsie_sca from the vsie struct. */
static struct vsie_sca *get_reuseable_vsie_sca(struct kvm *kvm)
{
	struct vsie_sca *vsie_sca;
	int i, ref_count;

	lockdep_assert_held_write(&kvm->arch.vsie.vsie_sca_lock);

	for (i = 0; i < kvm->arch.vsie.sca_count; i++) {
		vsie_sca = READ_ONCE(kvm->arch.vsie.scas[kvm->arch.vsie.sca_next]);
		kvm->arch.vsie.sca_next++;
		kvm->arch.vsie.sca_next %= kvm->arch.vsie.sca_count;
		ref_count = atomic_inc_return(&vsie_sca->ref_count);
		WARN_ON_ONCE(ref_count < 1);
		if (ref_count == 1)
			return vsie_sca;
		put_vsie_sca(vsie_sca);
	}
	return ERR_PTR(-EAGAIN);
}

static void free_vsie_sca(struct kvm *kvm, struct vsie_sca *vsie_sca)
{
	free_pages_exact(vsie_sca, sizeof(*vsie_sca));
}

static struct vsie_sca *alloc_vsie_sca(void)
{
	struct vsie_sca *vsie_sca;

	vsie_sca = alloc_pages_exact(sizeof(*vsie_sca), GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	if (!vsie_sca)
		return NULL;

	mutex_init(&vsie_sca->mutex);
	atomic_set(&vsie_sca->ref_count, 0);

	return vsie_sca;
}

/* Clear the vsie_sca struct but keep the vsie_page references, mutex and ref_count */
static void clear_vsie_sca(struct vsie_sca *vsie_sca)
{
	memset(&vsie_sca->head, 0, sizeof(vsie_sca->head));
	memset(&vsie_sca->tail, 0, sizeof(vsie_sca->tail));
}

/* Pin and get an existing or new guest-3 system control area.*/
static struct vsie_sca *get_vsie_sca(struct kvm_vcpu *vcpu, struct kvm_s390_sie_block *scb_o)
{
	struct vsie_sca *vsie_sca, *vsie_sca_new = NULL;
	gpa_t sca_gpa = read_scao(vcpu->kvm, scb_o);
	struct vsie_page *vsie_page_n;
	struct kvm *kvm = vcpu->kvm;
	unsigned int max_vsie_sca;
	int rc, cpu_nr;

	/* validate scb_o as we do not unshadow on error here */
	rc = validate_scao(vcpu, scb_o, sca_gpa);
	if (rc)
		return ERR_PTR(-EINVAL);

	down_read(&kvm->arch.vsie.vsie_sca_lock);
	vsie_sca = get_existing_vsie_sca(kvm, sca_gpa);
	up_read(&kvm->arch.vsie.vsie_sca_lock);
	if (vsie_sca)
		return vsie_sca;

	/*
	 * Allocate new vsie_sca, it will likely be needed below.
	 * We want at least #online_vcpus shadows, so every VCPU can execute the
	 * VSIE in parallel. (Worst case all single core VMs.)
	 */
	max_vsie_sca = MIN(atomic_read(&kvm->online_vcpus), KVM_S390_MAX_VSIE_VCPUS);

	if (kvm->arch.vsie.sca_count < max_vsie_sca) {
		vsie_sca_new = alloc_vsie_sca();
		if (!vsie_sca_new)
			return ERR_PTR(-ENOMEM);
	}

	/*
	 * Now we're taking the vsie_sca_lock in write mode so that we can manipulate
	 * the radix tree and recheck for existing SCAs with exclusive access.
	 *
	 * In the next lines we try three things to get an SCA:
	 *   - Retry getting an existing vsie_sca
	 *   - Using our newly allocated vsie_sca if we're under the limit
	 *   - Reusing an vsie_sca including ssca to shadow a different osca
	 */
	down_write(&kvm->arch.vsie.vsie_sca_lock);
	vsie_sca = get_existing_vsie_sca(kvm, sca_gpa);
	if (vsie_sca)
		goto out;

	/* check again under write lock if we are still under our vsie_sca limit */
	if (vsie_sca_new && kvm->arch.vsie.sca_count < max_vsie_sca) {
		/* make use of vsie_sca just created */
		vsie_sca = vsie_sca_new;
		vsie_sca_new = NULL;

		kvm->arch.vsie.scas[kvm->arch.vsie.sca_count] = vsie_sca;
		kvm->arch.vsie.sca_count++;
		atomic_set(&vsie_sca->ref_count, 1);
	} else {
		/* reuse previously created vsie_sca allocation for different osca */
		vsie_sca = get_reuseable_vsie_sca(kvm);
		/* with nr_vcpus scas one must be reusable */
		if (IS_ERR(vsie_sca))
			goto out;
		WARN_ON_ONCE(atomic_read(&vsie_sca->ref_count) != 1);

		xa_erase(&kvm->arch.vsie.osca_to_sca, vsie_sca->sca_gpa >> SCA_ALIGNMENT_SHIFT);
		for (cpu_nr = 0; cpu_nr < KVM_S390_MAX_VSIE_VCPUS; cpu_nr++) {
			vsie_page_n = vsie_sca->pages[cpu_nr];
			if (!vsie_page_n)
				continue;

			/* unpin but keep the vsie_page for reuse */
			unpin_scb(kvm, vsie_page_n);
			release_gmap_shadow_safe(kvm, vsie_page_n);
			memset(vsie_page_n, 0, sizeof(struct vsie_page));
			vsie_page_n->scb_gpa = ULONG_MAX;
		}
		unpin_sca(kvm, vsie_sca);
		clear_vsie_sca(vsie_sca);
	}

	if (sie_uses_esca(scb_o))
		__set_bit(VSIE_SCA_ESCA, &vsie_sca->flags);
	vsie_sca->sca_gpa = sca_gpa;

	/*
	 * The pinned original sca will only be unpinned lazily to limit the
	 * required amount of pins/unpins on each vsie entry/exit.
	 * The unpin is done in the reuse vsie_sca allocation path above and
	 * kvm_s390_vsie_destroy().
	 */
	rc = pin_sca(kvm, vsie_sca);
	if (rc) {
		put_vsie_sca(vsie_sca);
		vsie_sca = ERR_PTR(rc);
		goto out;
	}

	WARN_ON_ONCE(xa_store(&kvm->arch.vsie.osca_to_sca,
			      vsie_sca->sca_gpa >> SCA_ALIGNMENT_SHIFT, vsie_sca, GFP_KERNEL));

out:
	up_write(&kvm->arch.vsie.vsie_sca_lock);
	if (vsie_sca_new)
		free_vsie_sca(kvm, vsie_sca_new);
	return vsie_sca;
}

void kvm_s390_vsie_gmap_notifier(struct gmap *gmap, gpa_t start, gpa_t end)
{
	struct vsie_page *cur, *next;
	unsigned long prefix;

	KVM_BUG_ON(!test_bit(GMAP_FLAG_SHADOW, &gmap->flags), gmap->kvm);
	/*
	 * Only new shadow blocks are added to the list during runtime,
	 * therefore we can safely reference them all the time.
	 */
	list_for_each_entry_safe(cur, next, &gmap->scb_users, gmap_cache.list) {
		prefix = cur->scb_s.prefix << GUEST_PREFIX_SHIFT;
		/* with mso/msl, the prefix lies at an offset */
		prefix += cur->scb_s.mso;
		if (prefix <= end && start <= prefix + 2 * PAGE_SIZE - 1)
			prefix_unmapped_sync(cur);
	}
}

/*
 * Map the first prefix page and if tx is enabled also the second prefix page.
 *
 * The prefix will be protected, a gmap notifier will inform about unmaps.
 * The shadow scb must not be executed until the prefix is remapped, this is
 * guaranteed by properly handling PROG_REQUEST.
 *
 * Returns: - 0 on if successfully mapped or already mapped
 *          - > 0 if control has to be given to guest 2
 *          - -EAGAIN if the caller can retry immediately
 *          - -ENOMEM if out of memory
 */
static int map_prefix(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page, struct gmap *sg)
{
	struct kvm_s390_sie_block *scb_s = &vsie_page->scb_s;
	u64 prefix = scb_s->prefix << GUEST_PREFIX_SHIFT;
	int rc;

	if (prefix_is_mapped(vsie_page))
		return 0;

	/* mark it as mapped so we can catch any concurrent unmappers */
	prefix_mapped(vsie_page);

	/* with mso/msl, the prefix lies at offset *mso* */
	prefix += scb_s->mso;

	rc = gaccess_shadow_fault(vcpu, sg, prefix, NULL, true);
	if (!rc && (scb_s->ecb & ECB_TE))
		rc = gaccess_shadow_fault(vcpu, sg, prefix + PAGE_SIZE, NULL, true);
	/*
	 * We don't have to mprotect, we will be called for all unshadows.
	 * SIE will detect if protection applies and trigger a validity.
	 */
	if (rc)
		prefix_unmapped(vsie_page);
	if (rc > 0 || rc == -EFAULT)
		rc = set_validity_icpt(scb_s, 0x0037U);
	return rc;
}

/* unpin all blocks previously pinned by pin_blocks(), marking them dirty */
static void unpin_blocks(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page)
{
	struct kvm_s390_sie_block *scb_s = &vsie_page->scb_s;
	hpa_t hpa;

	if (!vsie_page->vsie_sca) {
		hpa = (u64) scb_s->scaoh << 32 | scb_s->scaol;
		if (hpa) {
			unpin_guest_page(vcpu->kvm, vsie_page->sca_gpa, hpa);
			vsie_page->sca_gpa = 0;
			write_scao(scb_s, 0);
		}
	}

	hpa = scb_s->itdba;
	if (hpa) {
		unpin_guest_page(vcpu->kvm, vsie_page->itdba_gpa, hpa);
		vsie_page->itdba_gpa = 0;
		scb_s->itdba = 0;
	}

	hpa = scb_s->gvrd;
	if (hpa) {
		unpin_guest_page(vcpu->kvm, vsie_page->gvrd_gpa, hpa);
		vsie_page->gvrd_gpa = 0;
		scb_s->gvrd = 0;
	}

	hpa = scb_s->riccbd;
	if (hpa) {
		unpin_guest_page(vcpu->kvm, vsie_page->riccbd_gpa, hpa);
		vsie_page->riccbd_gpa = 0;
		scb_s->riccbd = 0;
	}

	hpa = scb_s->sdnxo;
	if (hpa) {
		unpin_guest_page(vcpu->kvm, vsie_page->sdnx_gpa, hpa);
		vsie_page->sdnx_gpa = 0;
		scb_s->sdnxo = 0;
	}
}

/*
 * Instead of shadowing some blocks, we can simply forward them because the
 * addresses in the scb are 64 bit long.
 *
 * This works as long as the data lies in one page. If blocks ever exceed one
 * page, we have to fall back to shadowing.
 *
 * Returns: - 0 if all blocks were pinned.
 *          - > 0 if control has to be given to guest 2
 *          - -ENOMEM if out of memory
 */
static int pin_blocks(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page)
{
	struct kvm_s390_sie_block *scb_o = vsie_page->scb_o;
	struct kvm_s390_sie_block *scb_s = &vsie_page->scb_s;
	hpa_t hpa;
	gpa_t gpa;
	int rc = 0;

	gpa = vsie_page->sca_gpa;
	if (gpa && !vsie_page->vsie_sca) {
		rc = validate_scao(vcpu, scb_s, gpa);
		if (rc)
			goto unpin;
		rc = pin_guest_page(vcpu->kvm, gpa, &hpa);
		if (rc) {
			rc = set_validity_icpt(scb_s, 0x0034U);
			goto unpin;
		}
		write_scao(scb_s, hpa);
	}

	gpa = READ_ONCE(scb_o->itdba) & ~0xffUL;
	if (gpa && (scb_s->ecb & ECB_TE)) {
		if (gpa < 2 * PAGE_SIZE) {
			rc = set_validity_icpt(scb_s, 0x0080U);
			goto unpin;
		}
		/* 256 bytes cannot cross page boundaries */
		rc = pin_guest_page(vcpu->kvm, gpa, &hpa);
		if (rc) {
			rc = set_validity_icpt(scb_s, 0x0080U);
			goto unpin;
		}
		vsie_page->itdba_gpa = gpa;
		scb_s->itdba = hpa;
	}

	gpa = READ_ONCE(scb_o->gvrd) & ~0x1ffUL;
	if (gpa && (scb_s->eca & ECA_VX) && !(scb_s->ecd & ECD_HOSTREGMGMT)) {
		if (gpa < 2 * PAGE_SIZE) {
			rc = set_validity_icpt(scb_s, 0x1310U);
			goto unpin;
		}
		/*
		 * 512 bytes vector registers cannot cross page boundaries
		 * if this block gets bigger, we have to shadow it.
		 */
		rc = pin_guest_page(vcpu->kvm, gpa, &hpa);
		if (rc) {
			rc = set_validity_icpt(scb_s, 0x1310U);
			goto unpin;
		}
		vsie_page->gvrd_gpa = gpa;
		scb_s->gvrd = hpa;
	}

	gpa = READ_ONCE(scb_o->riccbd) & ~0x3fUL;
	if (gpa && (scb_s->ecb3 & ECB3_RI)) {
		if (gpa < 2 * PAGE_SIZE) {
			rc = set_validity_icpt(scb_s, 0x0043U);
			goto unpin;
		}
		/* 64 bytes cannot cross page boundaries */
		rc = pin_guest_page(vcpu->kvm, gpa, &hpa);
		if (rc) {
			rc = set_validity_icpt(scb_s, 0x0043U);
			goto unpin;
		}
		/* Validity 0x0044 will be checked by SIE */
		vsie_page->riccbd_gpa = gpa;
		scb_s->riccbd = hpa;
	}
	if (((scb_s->ecb & ECB_GS) && !(scb_s->ecd & ECD_HOSTREGMGMT)) ||
	    (scb_s->ecd & ECD_ETOKENF)) {
		unsigned long sdnxc;

		gpa = READ_ONCE(scb_o->sdnxo) & ~0xfUL;
		sdnxc = READ_ONCE(scb_o->sdnxo) & 0xfUL;
		if (!gpa || gpa < 2 * PAGE_SIZE) {
			rc = set_validity_icpt(scb_s, 0x10b0U);
			goto unpin;
		}
		if (sdnxc < 6 || sdnxc > 12) {
			rc = set_validity_icpt(scb_s, 0x10b1U);
			goto unpin;
		}
		if (gpa & ((1 << sdnxc) - 1)) {
			rc = set_validity_icpt(scb_s, 0x10b2U);
			goto unpin;
		}
		/* Due to alignment rules (checked above) this cannot
		 * cross page boundaries
		 */
		rc = pin_guest_page(vcpu->kvm, gpa, &hpa);
		if (rc) {
			rc = set_validity_icpt(scb_s, 0x10b0U);
			goto unpin;
		}
		vsie_page->sdnx_gpa = gpa;
		scb_s->sdnxo = hpa | sdnxc;
	}
	return 0;
unpin:
	unpin_blocks(vcpu, vsie_page);
	return rc;
}

/*
 * Inject a fault into guest 2.
 *
 * Returns: - > 0 if control has to be given to guest 2
 *            < 0 if an error occurred during injection.
 */
static int inject_fault(struct kvm_vcpu *vcpu, __u16 code, __u64 vaddr,
			bool write_flag)
{
	struct kvm_s390_pgm_info pgm = {
		.code = code,
		.trans_exc_code =
			/* 0-51: virtual address */
			(vaddr & 0xfffffffffffff000UL) |
			/* 52-53: store / fetch */
			(((unsigned int) !write_flag) + 1) << 10,
			/* 62-63: asce id (always primary == 0) */
		.exc_access_id = 0, /* always primary */
		.op_access_id = 0, /* not MVPG */
	};
	int rc;

	if (code == PGM_PROTECTION)
		pgm.trans_exc_code |= 0x4UL;

	rc = kvm_s390_inject_prog_irq(vcpu, &pgm);
	return rc ? rc : 1;
}

/*
 * Handle a fault during vsie execution on a gmap shadow.
 *
 * Returns: - 0 if the fault was resolved
 *          - > 0 if control has to be given to guest 2
 *          - < 0 if an error occurred
 */
static int handle_fault(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page, struct gmap *sg)
{
	bool wr = kvm_s390_cur_gmap_fault_is_write();
	int rc;

	if ((current->thread.gmap_int_code & PGM_INT_CODE_MASK) == PGM_PROTECTION)
		/* we can directly forward all protection exceptions */
		return inject_fault(vcpu, PGM_PROTECTION,
				    current->thread.gmap_teid.addr * PAGE_SIZE, 1);

	rc = gaccess_shadow_fault(vcpu, sg, current->thread.gmap_teid.addr * PAGE_SIZE, NULL, wr);
	if (rc > 0) {
		rc = inject_fault(vcpu, rc,
				  current->thread.gmap_teid.addr * PAGE_SIZE, wr);
		if (rc >= 0)
			vsie_page->fault_addr = current->thread.gmap_teid.addr * PAGE_SIZE;
	}
	return rc;
}

/*
 * Retry the previous fault that required guest 2 intervention. This avoids
 * one superfluous SIE re-entry and direct exit.
 *
 * Will ignore any errors. The next SIE fault will do proper fault handling.
 */
static void handle_last_fault(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page, struct gmap *sg)
{
	if (vsie_page->fault_addr)
		gaccess_shadow_fault(vcpu, sg, vsie_page->fault_addr, NULL, true);
	vsie_page->fault_addr = 0;
}

static inline void clear_vsie_icpt(struct vsie_page *vsie_page)
{
	vsie_page->scb_s.icptcode = 0;
}

/* rewind the psw and clear the vsie icpt, so we can retry execution */
static void retry_vsie_icpt(struct vsie_page *vsie_page)
{
	struct kvm_s390_sie_block *scb_s = &vsie_page->scb_s;
	int ilen = insn_length(scb_s->ipa >> 8);

	/* take care of EXECUTE instructions */
	if (scb_s->icptstatus & 1) {
		ilen = (scb_s->icptstatus >> 4) & 0x6;
		if (!ilen)
			ilen = 4;
	}
	scb_s->gpsw.addr = __rewind_psw(scb_s->gpsw, ilen);
	clear_vsie_icpt(vsie_page);
}

static int handle_stfle_0(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page,
			  u32 fac_list_origin)
{
	struct kvm_s390_sie_block *scb_s = &vsie_page->scb_s;

	/*
	 * format-0 -> size of nested guest's facility list == guest's size
	 * guest's size == host's size, since STFLE is interpretatively executed
	 * using a format-0 for the guest, too.
	 */
	if (read_guest_real(vcpu, fac_list_origin, &vsie_page->fac,
			    stfle_size() * sizeof(u64)))
		return set_validity_icpt(scb_s, 0x1090U);
	scb_s->fac = (u32)virt_to_phys(&vsie_page->fac);
	return 0;
}

static int handle_stfle_2(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page, u32 fac_list_origin)
{
	struct kvm_s390_flcb2 *flcb_s = (struct kvm_s390_flcb2 *)vsie_page->fac;
	struct kvm_s390_sie_block *scb_s = &vsie_page->scb_s;
	u64 len;

	if (read_guest_real(vcpu, fac_list_origin, &len, sizeof(len)))
		return set_validity_icpt(scb_s, 0x1090U);

	/* discard reserved bits */
	len = (len & U8_MAX);
	flcb_s->header_val = len;
	len += 1;

	if (read_guest_real(vcpu, fac_list_origin + offsetof(struct kvm_s390_flcb2, facilities),
			    &flcb_s->facilities, len * sizeof(u64)))
		return set_validity_icpt(scb_s, 0x1090U);

	scb_s->fac = (u32)virt_to_phys(&vsie_page->fac) | S390_ARCH_FAC_FORMAT_2;
	return 0;
}

/*
 * Try to shadow + enable the guest 2 provided facility list.
 * Retry instruction execution if enabled for and provided by guest 2.
 *
 * Returns: - 0 if handled (retry or guest 2 icpt)
 *          - > 0 if control has to be given to guest 2
 */
static int handle_stfle(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page)
{
	bool has_astfleie2 = test_kvm_cpu_feat(vcpu->kvm, KVM_S390_VM_CPU_FEAT_ASTFLEIE2);
	u32 fac = READ_ONCE(vsie_page->scb_o->fac);
	int format_mask, format;
	u32 origin;

	/* assert no overflow with maximum len */
	BUILD_BUG_ON(sizeof(vsie_page->fac) < ((S390_ARCH_FAC_LIST_SIZE_U64 + 1) * sizeof(u64)));
	BUILD_BUG_ON(!IS_ALIGNED(offsetof(struct vsie_page, fac), 8));

	if (fac && test_kvm_facility(vcpu->kvm, 7)) {
		retry_vsie_icpt(vsie_page);
		/*
		 * The facility list origin (FLO) is in bits 1 - 28 of the FLD
		 * so we need to mask here before reading.
		 */
		origin = fac & 0x7ffffff8U;
		format_mask = has_astfleie2 ? 3 : 0;
		format = fac & format_mask;
		switch (format) {
		case 0:
			return handle_stfle_0(vcpu, vsie_page, origin);
		case 1:
			return set_validity_icpt(&vsie_page->scb_s, 0x1330U);
		case 2:
			return handle_stfle_2(vcpu, vsie_page, origin);
		case 3:
			return set_validity_icpt(&vsie_page->scb_s, 0x1330U);
		}
	}
	return 0;
}

/*
 * Get a register for a nested guest.
 * @vcpu the vcpu of the guest
 * @vsie_page the vsie_page for the nested guest
 * @reg the register number, the upper 4 bits are ignored.
 * returns: the value of the register.
 */
static u64 vsie_get_register(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page, u8 reg)
{
	/* no need to validate the parameter and/or perform error handling */
	reg &= 0xf;
	switch (reg) {
	case 15:
		return vsie_page->scb_s.gg15;
	case 14:
		return vsie_page->scb_s.gg14;
	default:
		return vcpu->run->s.regs.gprs[reg];
	}
}

static int vsie_handle_mvpg(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page, struct gmap *sg)
{
	struct kvm_s390_sie_block *scb_s = &vsie_page->scb_s;
	unsigned long src, dest, mask, prefix;
	u64 *pei_block = &vsie_page->scb_o->mcic;
	union mvpg_pei pei_dest, pei_src;
	int edat, rc_dest, rc_src;
	union ctlreg0 cr0;

	cr0.val = vcpu->arch.sie_block->gcr[0];
	edat = cr0.edat && test_kvm_facility(vcpu->kvm, 8);
	mask = _kvm_s390_logical_to_effective(&scb_s->gpsw, PAGE_MASK);
	prefix = scb_s->prefix << GUEST_PREFIX_SHIFT;

	dest = vsie_get_register(vcpu, vsie_page, scb_s->ipb >> 20) & mask;
	dest = _kvm_s390_real_to_abs(prefix, dest) + scb_s->mso;
	src = vsie_get_register(vcpu, vsie_page, scb_s->ipb >> 16) & mask;
	src = _kvm_s390_real_to_abs(prefix, src) + scb_s->mso;

	rc_dest = gaccess_shadow_fault(vcpu, sg, dest, &pei_dest, true);
	rc_src = gaccess_shadow_fault(vcpu, sg, src, &pei_src, false);
	/*
	 * Either everything went well, or something non-critical went wrong
	 * e.g. because of a race. In either case, simply retry.
	 */
	if (rc_dest == -EAGAIN || rc_src == -EAGAIN || (!rc_dest && !rc_src)) {
		retry_vsie_icpt(vsie_page);
		return -EAGAIN;
	}
	/* Something more serious went wrong, propagate the error */
	if (rc_dest < 0)
		return rc_dest;
	if (rc_src < 0)
		return rc_src;

	/* The only possible suppressing exception: just deliver it */
	if (rc_dest == PGM_TRANSLATION_SPEC || rc_src == PGM_TRANSLATION_SPEC) {
		clear_vsie_icpt(vsie_page);
		rc_dest = kvm_s390_inject_program_int(vcpu, PGM_TRANSLATION_SPEC);
		WARN_ON_ONCE(rc_dest);
		return 1;
	}

	/*
	 * Forward the PEI intercept to the guest if it was a page fault, or
	 * also for segment and region table faults if EDAT applies.
	 */
	if (edat) {
		rc_dest = rc_dest == PGM_ASCE_TYPE ? rc_dest : 0;
		rc_src = rc_src == PGM_ASCE_TYPE ? rc_src : 0;
	} else {
		rc_dest = rc_dest != PGM_PAGE_TRANSLATION ? rc_dest : 0;
		rc_src = rc_src != PGM_PAGE_TRANSLATION ? rc_src : 0;
	}
	if (!rc_dest && !rc_src) {
		pei_block[0] = pei_dest.val;
		pei_block[1] = pei_src.val;
		return 1;
	}

	retry_vsie_icpt(vsie_page);

	/*
	 * The host has edat, and the guest does not, or it was an ASCE type
	 * exception. The host needs to inject the appropriate DAT interrupts
	 * into the guest.
	 */
	if (rc_dest)
		return inject_fault(vcpu, rc_dest, dest, 1);
	return inject_fault(vcpu, rc_src, src, 0);
}

/*
 * Run the vsie on a shadow scb and a shadow gmap, without any further
 * sanity checks, handling SIE faults.
 *
 * Returns: - 0 everything went fine
 *          - > 0 if control has to be given to guest 2
 *          - < 0 if an error occurred
 */
static int do_vsie_run(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page, struct gmap *sg)
	__releases(vcpu->kvm->srcu)
	__acquires(vcpu->kvm->srcu)
{
	struct kvm_s390_sie_block *scb_s = &vsie_page->scb_s;
	struct kvm_s390_sie_block *scb_o = vsie_page->scb_o;
	unsigned long sie_return = SIE64_RETURN_NORMAL;
	int guest_bp_isolation;
	int rc = 0;

	handle_last_fault(vcpu, vsie_page, sg);

	kvm_vcpu_srcu_read_unlock(vcpu);

	/* save current guest state of bp isolation override */
	guest_bp_isolation = test_thread_flag(TIF_ISOLATE_BP_GUEST);

	/*
	 * The guest is running with BPBC, so we have to force it on for our
	 * nested guest. This is done by enabling BPBC globally, so the BPBC
	 * control in the SCB (which the nested guest can modify) is simply
	 * ignored.
	 */
	if (test_kvm_facility(vcpu->kvm, 82) &&
	    vcpu->arch.sie_block->fpf & FPF_BPBC)
		set_thread_flag(TIF_ISOLATE_BP_GUEST);

	/*
	 * Simulate a SIE entry of the VCPU (see sie64a), so VCPU blocking
	 * and VCPU requests also hinder the vSIE from running and lead
	 * to an immediate exit. kvm_s390_vsie_kick() has to be used to
	 * also kick the vSIE.
	 */
	vcpu->arch.sie_block->prog0c |= PROG_IN_SIE;
	current->thread.gmap_int_code = 0;
	barrier();
	if (!kvm_s390_vcpu_sie_inhibited(vcpu)) {
xfer_to_guest_mode_check:
		local_irq_disable();
		xfer_to_guest_mode_prepare();
		if (xfer_to_guest_mode_work_pending()) {
			local_irq_enable();
			rc = kvm_xfer_to_guest_mode_handle_work(vcpu);
			if (rc)
				goto skip_sie;
			goto xfer_to_guest_mode_check;
		}
		guest_timing_enter_irqoff();
		sie_return = kvm_s390_enter_exit_sie(scb_s, vcpu->run->s.regs.gprs, sg->asce.val);
		guest_timing_exit_irqoff();
		local_irq_enable();
	}

skip_sie:
	barrier();
	vcpu->arch.sie_block->prog0c &= ~PROG_IN_SIE;

	/* restore guest state for bp isolation override */
	if (!guest_bp_isolation)
		clear_thread_flag(TIF_ISOLATE_BP_GUEST);

	kvm_vcpu_srcu_read_lock(vcpu);

	if (sie_return == SIE64_RETURN_MCCK) {
		kvm_s390_reinject_machine_check(vcpu, &vsie_page->mcck_info);
		return 0;
	}

	WARN_ON_ONCE(sie_return != SIE64_RETURN_NORMAL);

	if (rc > 0)
		rc = 0; /* we could still have an icpt */
	else if (current->thread.gmap_int_code)
		return handle_fault(vcpu, vsie_page, sg);

	switch (scb_s->icptcode) {
	case ICPT_INST:
		if (scb_s->ipa == 0xb2b0)
			rc = handle_stfle(vcpu, vsie_page);
		break;
	case ICPT_STOP:
		/* stop not requested by g2 - must have been a kick */
		if (!(atomic_read(&scb_o->cpuflags) & CPUSTAT_STOP_INT))
			clear_vsie_icpt(vsie_page);
		break;
	case ICPT_VALIDITY:
		if ((scb_s->ipa & 0xf000) != 0xf000)
			scb_s->ipa += 0x1000;
		break;
	case ICPT_PARTEXEC:
		if (scb_s->ipa == 0xb254)
			rc = vsie_handle_mvpg(vcpu, vsie_page, sg);
		break;
	}
	return rc;
}

/*
 * Register the shadow scb at the VCPU, e.g. for kicking out of vsie.
 */
static void register_shadow_scb(struct kvm_vcpu *vcpu,
				struct vsie_page *vsie_page)
{
	struct kvm_s390_sie_block *scb_s = &vsie_page->scb_s;

	WRITE_ONCE(vcpu->arch.vsie_block, &vsie_page->scb_s);
	/*
	 * External calls have to lead to a kick of the vcpu and
	 * therefore the vsie -> Simulate Wait state.
	 */
	kvm_s390_set_cpuflags(vcpu, CPUSTAT_WAIT);
	/*
	 * We have to adjust the g3 epoch by the g2 epoch. The epoch will
	 * automatically be adjusted on tod clock changes via kvm_sync_clock.
	 */
	preempt_disable();
	scb_s->epoch += vcpu->kvm->arch.epoch;

	if (scb_s->ecd & ECD_MEF) {
		scb_s->epdx += vcpu->kvm->arch.epdx;
		if (scb_s->epoch < vcpu->kvm->arch.epoch)
			scb_s->epdx += 1;
	}

	preempt_enable();
}

/*
 * Unregister a shadow scb from a VCPU.
 */
static void unregister_shadow_scb(struct kvm_vcpu *vcpu)
{
	kvm_s390_clear_cpuflags(vcpu, CPUSTAT_WAIT);
	WRITE_ONCE(vcpu->arch.vsie_block, NULL);
}

/*
 * Run the vsie on a shadowed scb, managing the gmap shadow, handling
 * prefix pages and faults.
 *
 * Returns: - 0 if no errors occurred
 *          - > 0 if control has to be given to guest 2
 *          - -ENOMEM if out of memory
 */
static int vsie_run(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page)
{
	struct kvm_s390_sie_block *scb_s = &vsie_page->scb_s;
	struct gmap *sg = NULL;
	int rc = 0;

	while (1) {
		sg = acquire_gmap_shadow(vcpu, vsie_page);
		if (IS_ERR(sg)) {
			rc = PTR_ERR(sg);
			sg = NULL;
		}
		if (!rc)
			rc = map_prefix(vcpu, vsie_page, sg);
		if (!rc) {
			update_intervention_requests(vsie_page);
			rc = do_vsie_run(vcpu, vsie_page, sg);
		}
		atomic_andnot(PROG_BLOCK_SIE, &scb_s->prog20);

		if (rc == -EAGAIN)
			rc = 0;

		/*
		 * Exit the loop if the guest needs to process the intercept
		 */
		if (rc || scb_s->icptcode)
			break;

		/*
		 * Exit the loop if the host needs to process an intercept,
		 * but rewind the PSW to re-enter SIE once that's completed
		 * instead of passing a "no action" intercept to the guest.
		 */
		if (kvm_s390_vcpu_has_irq(vcpu, 0) ||
		    kvm_s390_vcpu_sie_inhibited(vcpu)) {
			rc = -EAGAIN;
			break;
		}
		if (sg)
			sg = gmap_put(sg);
		cond_resched();
	}
	if (sg)
		sg = gmap_put(sg);

	if (rc == -EFAULT) {
		/*
		 * Addressing exceptions are always presentes as intercepts.
		 * As addressing exceptions are suppressing and our guest 3 PSW
		 * points at the responsible instruction, we have to
		 * forward the PSW and set the ilc. If we can't read guest 3
		 * instruction, we can use an arbitrary ilc. Let's always use
		 * ilen = 4 for now, so we can avoid reading in guest 3 virtual
		 * memory. (we could also fake the shadow so the hardware
		 * handles it).
		 */
		scb_s->icptcode = ICPT_PROGI;
		scb_s->iprcc = PGM_ADDRESSING;
		scb_s->pgmilc = 4;
		scb_s->gpsw.addr = __rewind_psw(scb_s->gpsw, 4);
		rc = 1;
	}
	return rc;
}

/* Try getting a given vsie page, returning "true" on success. */
static inline bool try_get_vsie_page(struct vsie_page *vsie_page)
{
	if (test_bit(VSIE_PAGE_IN_USE, &vsie_page->flags))
		return false;
	return !test_and_set_bit(VSIE_PAGE_IN_USE, &vsie_page->flags);
}

/* Put a vsie page acquired through get_vsie_page / try_get_vsie_page. */
static void put_vsie_page(struct vsie_page *vsie_page)
{
	clear_bit(VSIE_PAGE_IN_USE, &vsie_page->flags);
}

static void free_vsie_page(struct vsie_page *vsie_page)
{
	free_page((unsigned long)vsie_page);
}

static struct vsie_page *alloc_vsie_page(struct kvm *kvm)
{
	struct vsie_page *vsie_page;

	vsie_page = (struct vsie_page *)__get_free_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO | GFP_DMA);
	if (!vsie_page)
		return vsie_page;

	/* Mark it as invalid until it resides in the tree. */
	vsie_page->scb_gpa = ULONG_MAX;
	return vsie_page;
}

static int vsie_page_init(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page, unsigned long scb_gpa)
{
	struct vsie_page *vsie_page_old;
	struct kvm *kvm = vcpu->kvm;
	int rc;

	vsie_page->scb_gpa = scb_gpa;
	rc = pin_scb(vcpu, vsie_page);
	if (rc) {
		vsie_page->scb_gpa = ULONG_MAX;
		return -ENOMEM;
	}

	vsie_page->sca_gpa = read_scao(kvm, vsie_page->scb_o);

	/*
	 * store the vsie_page in addr_to_page
	 * mind that g2 may have reused the sca - make sure we do not remove the sca from
	 * the new config when reusing the vsie_page_old
	 */
	vsie_page_old = xa_store(&kvm->arch.vsie.addr_to_page, scb_gpa >> SCB_ALIGNMENT_SHIFT,
				 vsie_page, GFP_KERNEL_ACCOUNT);
	if (WARN_ON_ONCE(xa_err(vsie_page_old)))
		return 0;
	if (vsie_page_old && vsie_page_old != vsie_page)
		WRITE_ONCE(vsie_page_old->scb_gpa, ULONG_MAX);

	return 0;
}

/*
 * Get or create a vsie page for a scb address.
 *
 * Original control blocks are pinned when the vsie_page pointing to them is
 * returned.
 * Newly created vsie_pages only have vsie_page->scb_gpa and vsie_page->sca_gpa
 * set.
 *
 * Returns: - address of a vsie page (cached or new one)
 *          - NULL if the same scb address is already used by another VCPU
 *          - ERR_PTR(-ENOMEM) if out of memory
 */
static struct vsie_page *get_vsie_page(struct kvm_vcpu *vcpu, unsigned long addr)
{
	struct vsie_page *vsie_page, *vsie_page_new = NULL;
	struct kvm *kvm = vcpu->kvm;
	unsigned int max_vsie_page;
	int rc, pages_idx;

	vsie_page = xa_load(&kvm->arch.vsie.addr_to_page, addr >> SCB_ALIGNMENT_SHIFT);
	if (vsie_page && try_get_vsie_page(vsie_page)) {
		if (vsie_page->scb_gpa == addr)
			return vsie_page;
		/*
		 * We raced with someone reusing + putting this vsie
		 * page before we grabbed it.
		 */
		put_vsie_page(vsie_page);
	}

	max_vsie_page = atomic_read(&kvm->online_vcpus);

	/* allocate new vsie_page - we will likely need it */
	if (kvm->arch.vsie.page_count < max_vsie_page) {
		vsie_page_new = alloc_vsie_page(kvm);
		if (!vsie_page_new)
			return ERR_PTR(-ENOMEM);
		__set_bit(VSIE_PAGE_IN_USE, &vsie_page_new->flags);
	}

	mutex_lock(&kvm->arch.vsie.mutex);
	vsie_page = xa_load(&kvm->arch.vsie.addr_to_page, addr >> SCB_ALIGNMENT_SHIFT);
	if (vsie_page && try_get_vsie_page(vsie_page)) {
		if (vsie_page->scb_gpa == addr) {
			mutex_unlock(&kvm->arch.vsie.mutex);
			if (vsie_page_new)
				free_vsie_page(vsie_page_new);
			return vsie_page;
		}
		/*
		 * We raced with someone reusing + putting this vsie
		 * page before we grabbed it.
		 */
		put_vsie_page(vsie_page);
	}

	if (kvm->arch.vsie.page_count < max_vsie_page) {
		pages_idx = kvm->arch.vsie.page_count;
		vsie_page = vsie_page_new;
		vsie_page_new = NULL;
		WRITE_ONCE(kvm->arch.vsie.pages[kvm->arch.vsie.page_count], vsie_page);
		kvm->arch.vsie.page_count++;
	} else {
		/* reuse an existing entry that belongs to nobody */
		while (true) {
			pages_idx = kvm->arch.vsie.next;
			kvm->arch.vsie.next++;
			kvm->arch.vsie.next %= kvm->arch.vsie.page_count;
			vsie_page = kvm->arch.vsie.pages[pages_idx];
			if (try_get_vsie_page(vsie_page))
				break;
		}

		unpin_scb(kvm, vsie_page);
	}

	rc = vsie_page_init(vcpu, vsie_page, addr);
	mutex_unlock(&kvm->arch.vsie.mutex);
	if (vsie_page_new)
		free_vsie_page(vsie_page_new);
	if (WARN_ON_ONCE(rc)) {
		unpin_scb(kvm, vsie_page);
		vsie_page->scb_gpa = ULONG_MAX;
		put_vsie_page(vsie_page);
		return ERR_PTR(rc);
	}

	memset(&vsie_page->scb_s, 0, sizeof(struct kvm_s390_sie_block));
	release_gmap_shadow_safe(kvm, vsie_page);
	prefix_unmapped(vsie_page);
	vsie_page->fault_addr = 0;
	vsie_page->scb_s.ihcpu = 0xffffU;

	return vsie_page;
}

static struct vsie_page *get_vsie_page_cpu_nr(struct kvm_vcpu *vcpu, struct vsie_sca *vsie_sca,
					      gpa_t scb_gpa, u16 cpu_nr)
{
	struct vsie_page *vsie_page, *vsie_page_new = NULL;
	int rc;

	vsie_page = vsie_sca->pages[cpu_nr];
	if (!vsie_page) {
		vsie_page_new = alloc_vsie_page(vcpu->kvm);
		if (!vsie_page_new)
			return ERR_PTR(-ENOMEM);
		vsie_page_new->vsie_sca = vsie_sca;
		__set_bit(VSIE_PAGE_IN_USE, &vsie_page_new->flags);

		/* be careful to not loose a page here if we raced */
		scoped_guard(mutex, &vsie_sca->mutex) {
			vsie_page = vsie_sca->pages[cpu_nr];
			if (!vsie_page) {
				WRITE_ONCE(vsie_sca->pages[cpu_nr], vsie_page_new);
				vsie_page = vsie_page_new;
			}
		}
	}
	if (vsie_page != vsie_page_new) {
		if (vsie_page_new)
			free_vsie_page(vsie_page_new);

		/* not a new vsie_page so get it */
		if (!try_get_vsie_page(vsie_page))
			return ERR_PTR(-EAGAIN);
		vsie_page->vsie_sca = vsie_sca;
	}
	if (vsie_page->scb_gpa != scb_gpa || vsie_page->sca_gpa != vsie_sca->sca_gpa) {
		scoped_guard(mutex, &vcpu->kvm->arch.vsie.mutex) {
			unpin_scb(vcpu->kvm, vsie_page);
			rc = vsie_page_init(vcpu, vsie_page, scb_gpa);
		}
		if (WARN_ON_ONCE(rc)) {
			put_vsie_page(vsie_page);
			return ERR_PTR(rc);
		}
	}

	return vsie_page;
}

/*
 * Copy the mcn from the osca to the vsie_sca to be able to detect mcn changes later on.
 *
 * @vsie_sca: vsie_sca to copy mcn to.
 * @sca: Pointer to a struct bsca_block or struct esca_block to read from.
 */
static void sca_mcn_copy(struct vsie_sca *vsie_sca, void *sca)
{
	int offset = offsetof(struct bsca_block, mcn);
	int size = sizeof(unsigned long);

	if (test_bit(VSIE_SCA_ESCA, &vsie_sca->flags)) {
		offset = offsetof(struct esca_block, mcn);
		size = size * 4;
	}
	memcpy(&vsie_sca->mcn, sca + offset, size);
}

/*
 * Compare the mcn from the given sca to the vsie_sca to be able to detect mcn changes.
 *
 * @vsie_sca: vsie_sca to compare mcn to.
 * @sca: Pointer to a struct bsca_block or struct esca_block to compare to.
 */
static bool sca_mcn_equals(struct vsie_sca *vsie_sca, void *sca)
{
	int offset = offsetof(struct bsca_block, mcn);
	int size = sizeof(unsigned long);

	if (test_bit(VSIE_SCA_ESCA, &vsie_sca->flags)) {
		size = size * 4;
		offset = offsetof(struct esca_block, mcn);
	}

	return !memcmp(&vsie_sca->mcn, sca + offset, size);
}

static void vsie_sca_update(struct vsie_sca *vsie_sca, unsigned int cpu_nr,
			    struct vsie_page *vsie_page_n, hpa_t sca_o_entry_hpa)
{
	guard(mutex)(&vsie_sca->mutex);

	WRITE_ONCE(vsie_sca->ssca.cpu[cpu_nr].ssda, virt_to_phys(&vsie_page_n->scb_s));
	WRITE_ONCE(vsie_sca->ssca.cpu[cpu_nr].ossea, sca_o_entry_hpa);
	WRITE_ONCE(vsie_sca->pages[cpu_nr], vsie_page_n);
}

/* Fill the shadow system control area used for VSIE SIGPI. */
static int _shadow_sca(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page,
		       struct vsie_sca *vsie_sca)
{
	bool is_esca = sie_uses_esca(vsie_page->scb_o);
	unsigned int cpu_nr, cpu_slots;
	struct vsie_page *vsie_page_n;
	hpa_t sca_o_entry_hpa;
	hva_t sca_o_entry_hva;
	gpa_t scb_o_gpa;
	int rc;

	if (is_esca)
		__set_bit(VSIE_SCA_ESCA, &vsie_sca->flags);
	sca_mcn_copy(vsie_sca, phys_to_virt(sca_o_hpa(vsie_sca)));

	/* pin and make shadow for ALL scb in the sca */
	cpu_slots = is_esca ? KVM_S390_MAX_VSIE_VCPUS : KVM_S390_BSCA_CPU_SLOTS;
	for_each_set_bit_inv(cpu_nr, (unsigned long *)&vsie_sca->mcn, cpu_slots) {
		rc = get_sca_entry_addr(vcpu->kvm, vsie_sca, cpu_nr, NULL, &sca_o_entry_hpa);
		if (rc)
			goto err;

		if (vsie_page->scb_o->icpua == cpu_nr) {
			vsie_sca_update(vsie_sca, cpu_nr, vsie_page, sca_o_entry_hpa);
		} else {
			sca_o_entry_hva = (hva_t)phys_to_virt(sca_o_entry_hpa);
			if (is_esca)
				scb_o_gpa = ((struct esca_entry *)sca_o_entry_hva)->sda;
			else
				scb_o_gpa = ((struct bsca_entry *)sca_o_entry_hva)->sda;
			if (scb_o_gpa & 0x1ffUL) {
				rc = -EINVAL;
				goto err;
			}
			vsie_page_n = get_vsie_page_cpu_nr(vcpu, vsie_sca, scb_o_gpa, cpu_nr);
			if (!vsie_page_n)
				rc = -EAGAIN;
			if (IS_ERR(vsie_page_n))
				rc = PTR_ERR(vsie_page_n);
			if (rc)
				goto err;
			rc = shadow_scb(vcpu, vsie_page_n);
			vsie_sca_update(vsie_sca, cpu_nr, vsie_page_n, sca_o_entry_hpa);
			put_vsie_page(vsie_page_n);
			if (rc)
				goto err;
		}
	}
	vsie_sca->ssca.osca = sca_o_hpa(vsie_sca);

	return 0;

err:
	vsie_sca->ssca.osca = 0;
	for_each_set_bit_inv(cpu_nr, (unsigned long *)&vsie_sca->mcn, cpu_slots) {
		vsie_sca->ssca.cpu[cpu_nr].ssda = 0;
		vsie_sca->ssca.cpu[cpu_nr].ossea = 0;
	}
	return rc;
}

static bool config_changed(struct vsie_page *vsie_page, struct vsie_sca *vsie_sca)
{
	bool changed = !vsie_sca->ssca.osca;

	changed = changed || sie_uses_esca(vsie_page->scb_o) !=
			     test_bit(VSIE_SCA_ESCA, &vsie_sca->flags);
	return changed || !sca_mcn_equals(vsie_sca, phys_to_virt(sca_o_hpa(vsie_sca)));
}

/* Shadow or reshadow the SCA on VSIE enter. */
static int shadow_sca(struct kvm_vcpu *vcpu, struct vsie_page *vsie_page, struct vsie_sca *vsie_sca)
{
	scoped_guard(rwsem_read, &vcpu->kvm->arch.vsie.vsie_sca_lock) {
		if (!config_changed(vsie_page, vsie_sca))
			return 0;
	}

	guard(rwsem_write)(&vcpu->kvm->arch.vsie.vsie_sca_lock);
	if (!config_changed(vsie_page, vsie_sca))
		return 0;

	return _shadow_sca(vcpu, vsie_page, vsie_sca);
}

int kvm_s390_handle_vsie(struct kvm_vcpu *vcpu)
{
	struct kvm_s390_sie_block *scb_o;
	struct vsie_sca *vsie_sca = NULL;
	struct vsie_page *vsie_page;
	gpa_t scb_addr;
	hpa_t scb_hpa;
	int rc = 0;

	vcpu->stat.instruction_sie++;
	if (!test_kvm_cpu_feat(vcpu->kvm, KVM_S390_VM_CPU_FEAT_SIEF2))
		return -EOPNOTSUPP;
	if (vcpu->arch.sie_block->gpsw.mask & PSW_MASK_PSTATE)
		return kvm_s390_inject_program_int(vcpu, PGM_PRIVILEGED_OP);

	scb_addr = kvm_s390_get_base_disp_s(vcpu, NULL);

	/* 512 byte alignment */
	if (unlikely(scb_addr & 0x1ffUL))
		return kvm_s390_inject_program_int(vcpu, PGM_SPECIFICATION);

	if (kvm_s390_vcpu_has_irq(vcpu, 0) || kvm_s390_vcpu_sie_inhibited(vcpu)) {
		kvm_s390_rewind_psw(vcpu, 4);
		return 0;
	}

	rc = pin_guest_page(vcpu->kvm, scb_addr, &scb_hpa);
	if (rc)
		return kvm_s390_inject_program_int(vcpu, PGM_ADDRESSING);
	scb_o = (struct kvm_s390_sie_block *)phys_to_virt(scb_hpa);

	if (scb_o->icpua >= KVM_S390_MAX_VSIE_VCPUS) {
		rc = kvm_s390_inject_program_int(vcpu, PGM_SPECIFICATION);
		goto out_unpin;
	}

	if (!use_ssca(vcpu->kvm, scb_o)) {
		/* get the vsie_page with pinned scb_o */
		vsie_page = get_vsie_page(vcpu, scb_addr);
		if (IS_ERR(vsie_page)) {
			rc = PTR_ERR(vsie_page);
			goto out_unpin;
		}
		vsie_page->vsie_sca = NULL;
	} else {
		/* get the vsie_sca with pinned original sca */
		vsie_sca = get_vsie_sca(vcpu, scb_o);
		if (IS_ERR(vsie_sca)) {
			rc = PTR_ERR(vsie_sca);
			goto out_unpin;
		}
		vsie_page = get_vsie_page_cpu_nr(vcpu, vsie_sca, scb_addr, scb_o->icpua);
		if (IS_ERR(vsie_page)) {
			rc = PTR_ERR(vsie_page);
			goto out_put_sca;
		}
	}
	if (!vsie_page) {
		/* double use of sie control block - simply do nothing */
		rc = -EAGAIN;
		goto out_put_sca;
	}

	rc = shadow_scb(vcpu, vsie_page);
	if (rc)
		goto out_put;
	if (vsie_sca) {
		/* pin and shadow the sca including all scb_o in the g3 conf */
		rc = shadow_sca(vcpu, vsie_page, vsie_sca);
		if (rc)
			goto out_put;
	}

	rc = pin_blocks(vcpu, vsie_page);
	if (rc)
		goto out_unshadow;
	register_shadow_scb(vcpu, vsie_page);

	rc = vsie_run(vcpu, vsie_page);

	unregister_shadow_scb(vcpu);
	unpin_blocks(vcpu, vsie_page);
out_unshadow:
	unshadow_scb(vcpu, vsie_page);
out_put:
	put_vsie_page(vsie_page);
out_put_sca:
	put_vsie_sca(vsie_sca);
out_unpin:
	unpin_guest_page(vcpu->kvm, scb_addr, scb_hpa);

	if (rc == -EAGAIN) {
		kvm_s390_rewind_psw(vcpu, 4);
		rc = 0;
	}
	return rc < 0 ? rc : 0;
}

/* Init the vsie data structures. To be called when a vm is initialized. */
void kvm_s390_vsie_init(struct kvm *kvm)
{
	mutex_init(&kvm->arch.vsie.mutex);
	xa_init_flags(&kvm->arch.vsie.addr_to_page, XA_FLAGS_ACCOUNT);
	init_rwsem(&kvm->arch.vsie.vsie_sca_lock);
	xa_init_flags(&kvm->arch.vsie.osca_to_sca, XA_FLAGS_ACCOUNT);
}

static void kvm_s390_vsie_destroy_page(struct kvm *kvm, struct vsie_page *vsie_page)
{
	unpin_scb(kvm, vsie_page);
	release_gmap_shadow_safe(kvm, vsie_page);
	free_vsie_page(vsie_page);
}

/* Destroy the vsie data structures. To be called when a vm is destroyed. */
void kvm_s390_vsie_destroy(struct kvm *kvm)
{
	struct vsie_page *vsie_page;
	struct vsie_sca *vsie_sca;
	int i, cpu_nr;

	guard(mutex)(&kvm->arch.vsie.mutex);

	for (i = 0; i < kvm->arch.vsie.page_count; i++) {
		vsie_page = kvm->arch.vsie.pages[i];
		kvm->arch.vsie.pages[i] = NULL;
		kvm_s390_vsie_destroy_page(kvm, vsie_page);
	}

	kvm->arch.vsie.page_count = 0;
	for (i = 0; i < kvm->arch.vsie.sca_count; i++) {
		vsie_sca = kvm->arch.vsie.scas[i];
		kvm->arch.vsie.scas[i] = NULL;
		if (!vsie_sca)
			continue;

		for (cpu_nr = 0; cpu_nr < KVM_S390_MAX_VSIE_VCPUS; cpu_nr++) {
			vsie_page = vsie_sca->pages[cpu_nr];
			vsie_sca->pages[cpu_nr] = NULL;
			if (!vsie_page)
				continue;
			unpin_scb(kvm, vsie_page);
			kvm_s390_vsie_destroy_page(kvm, vsie_page);
		}

		unpin_sca(kvm, vsie_sca);
		free_vsie_sca(kvm, vsie_sca);
	}
	kvm->arch.vsie.sca_count = 0;
	xa_destroy(&kvm->arch.vsie.addr_to_page);
	xa_destroy(&kvm->arch.vsie.osca_to_sca);
}

void kvm_s390_vsie_kick(struct kvm_vcpu *vcpu)
{
	struct kvm_s390_sie_block *scb = READ_ONCE(vcpu->arch.vsie_block);

	/*
	 * Even if the VCPU lets go of the shadow sie block reference, it is
	 * still valid in the cache. So we can safely kick it.
	 */
	if (scb) {
		atomic_or(PROG_BLOCK_SIE, &scb->prog20);
		if (scb->prog0c & PROG_IN_SIE)
			atomic_or(CPUSTAT_STOP_INT, &scb->cpuflags);
	}
}
