/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * IOMMU API for ARM architected SMMUv3 implementations.
 *
 * Copyright (C) 2015 ARM Limited
 */

#ifndef _ARM_SMMU_V3_H
#define _ARM_SMMU_V3_H

#include <linux/iommu.h>
#include <linux/iommufd.h>
#include <linux/kernel.h>
#include <linux/mmzone.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>

struct arm_smmu_device;

#include "arm-smmu-v3-common.h"

/* Ensure DMA allocations are naturally aligned */
#ifdef CONFIG_CMA_ALIGNMENT
#define Q_MAX_SZ_SHIFT			(PAGE_SHIFT + CONFIG_CMA_ALIGNMENT)
#else
#define Q_MAX_SZ_SHIFT			(PAGE_SHIFT + MAX_PAGE_ORDER)
#endif

/*
 * Context descriptors.
 *
 * Linear: when less than 1024 SSIDs are supported
 * 2lvl: at most 1024 L1 entries,
 *       1024 lazy entries per table.
 */
#define CTXDESC_L2_ENTRIES		1024

#define CTXDESC_L1_DESC_V		(1UL << 0)
#define CTXDESC_L1_DESC_L2PTR_MASK	GENMASK_ULL(51, 12)

#define CTXDESC_CD_DWORDS		8

struct arm_smmu_cd {
	__le64 data[CTXDESC_CD_DWORDS];
};

struct arm_smmu_cdtab_l2 {
	struct arm_smmu_cd cds[CTXDESC_L2_ENTRIES];
};

struct arm_smmu_cdtab_l1 {
	__le64 l2ptr;
};

static inline unsigned int arm_smmu_cdtab_l1_idx(unsigned int ssid)
{
	return ssid / CTXDESC_L2_ENTRIES;
}

static inline unsigned int arm_smmu_cdtab_l2_idx(unsigned int ssid)
{
	return ssid % CTXDESC_L2_ENTRIES;
}

#define CTXDESC_CD_0_TCR_T0SZ		GENMASK_ULL(5, 0)
#define CTXDESC_CD_0_TCR_TG0		GENMASK_ULL(7, 6)
#define CTXDESC_CD_0_TCR_IRGN0		GENMASK_ULL(9, 8)
#define CTXDESC_CD_0_TCR_ORGN0		GENMASK_ULL(11, 10)
#define CTXDESC_CD_0_TCR_SH0		GENMASK_ULL(13, 12)
#define CTXDESC_CD_0_TCR_EPD0		(1ULL << 14)
#define CTXDESC_CD_0_TCR_EPD1		(1ULL << 30)

#define CTXDESC_CD_0_ENDI		(1UL << 15)
#define CTXDESC_CD_0_V			(1UL << 31)

#define CTXDESC_CD_0_TCR_IPS		GENMASK_ULL(34, 32)
#define CTXDESC_CD_0_TCR_TBI0		(1ULL << 38)

#define CTXDESC_CD_0_TCR_HA            (1UL << 43)
#define CTXDESC_CD_0_TCR_HD            (1UL << 42)

#define CTXDESC_CD_0_AA64		(1UL << 41)
#define CTXDESC_CD_0_S			(1UL << 44)
#define CTXDESC_CD_0_R			(1UL << 45)
#define CTXDESC_CD_0_A			(1UL << 46)
#define CTXDESC_CD_0_ASET		(1UL << 47)
#define CTXDESC_CD_0_ASID		GENMASK_ULL(63, 48)

#define CTXDESC_CD_1_TTB0_MASK		GENMASK_ULL(51, 4)

/*
 * When the SMMU only supports linear context descriptor tables, pick a
 * reasonable size limit (64kB).
 */
#define CTXDESC_LINEAR_CDMAX		ilog2(SZ_64K / sizeof(struct arm_smmu_cd))

/* Event queue */
#define EVTQ_ENT_SZ_SHIFT		5
#define EVTQ_ENT_DWORDS			((1 << EVTQ_ENT_SZ_SHIFT) >> 3)
#define EVTQ_MAX_SZ_SHIFT		(Q_MAX_SZ_SHIFT - EVTQ_ENT_SZ_SHIFT)

#define EVTQ_0_ID			GENMASK_ULL(7, 0)

#define EVT_ID_BAD_STREAMID_CONFIG	0x02
#define EVT_ID_STE_FETCH_FAULT		0x03
#define EVT_ID_BAD_STE_CONFIG		0x04
#define EVT_ID_STREAM_DISABLED_FAULT	0x06
#define EVT_ID_BAD_SUBSTREAMID_CONFIG	0x08
#define EVT_ID_CD_FETCH_FAULT		0x09
#define EVT_ID_BAD_CD_CONFIG		0x0a
#define EVT_ID_TRANSLATION_FAULT	0x10
#define EVT_ID_ADDR_SIZE_FAULT		0x11
#define EVT_ID_ACCESS_FAULT		0x12
#define EVT_ID_PERMISSION_FAULT		0x13
#define EVT_ID_VMS_FETCH_FAULT		0x25

#define EVTQ_0_SSV			(1UL << 11)
#define EVTQ_0_SSID			GENMASK_ULL(31, 12)
#define EVTQ_0_SID			GENMASK_ULL(63, 32)
#define EVTQ_1_STAG			GENMASK_ULL(15, 0)
#define EVTQ_1_STALL			(1UL << 31)
#define EVTQ_1_PnU			(1UL << 33)
#define EVTQ_1_InD			(1UL << 34)
#define EVTQ_1_RnW			(1UL << 35)
#define EVTQ_1_S2			(1UL << 39)
#define EVTQ_1_CLASS			GENMASK_ULL(41, 40)
#define EVTQ_1_CLASS_TT			0x01
#define EVTQ_1_TT_READ			(1UL << 44)
#define EVTQ_2_ADDR			GENMASK_ULL(63, 0)
#define EVTQ_3_IPA			GENMASK_ULL(51, 12)
#define EVTQ_3_FETCH_ADDR		GENMASK_ULL(51, 3)

/* PRI queue */
#define PRIQ_ENT_SZ_SHIFT		4
#define PRIQ_ENT_DWORDS			((1 << PRIQ_ENT_SZ_SHIFT) >> 3)
#define PRIQ_MAX_SZ_SHIFT		(Q_MAX_SZ_SHIFT - PRIQ_ENT_SZ_SHIFT)

#define PRIQ_0_SID			GENMASK_ULL(31, 0)
#define PRIQ_0_SSID			GENMASK_ULL(51, 32)
#define PRIQ_0_PERM_PRIV		(1UL << 58)
#define PRIQ_0_PERM_EXEC		(1UL << 59)
#define PRIQ_0_PERM_READ		(1UL << 60)
#define PRIQ_0_PERM_WRITE		(1UL << 61)
#define PRIQ_0_PRG_LAST			(1UL << 62)
#define PRIQ_0_SSID_V			(1UL << 63)

#define PRIQ_1_PRG_IDX			GENMASK_ULL(8, 0)
#define PRIQ_1_ADDR_MASK		GENMASK_ULL(63, 12)

/* High-level queue structures */
#define ARM_SMMU_POLL_TIMEOUT_US	1000000 /* 1s! */
#define ARM_SMMU_POLL_SPIN_COUNT	10

#define MSI_IOVA_BASE			0x8000000
#define MSI_IOVA_LENGTH			0x100000

struct arm_smmu_queue_poll {
	ktime_t				timeout;
	unsigned int			delay;
	unsigned int			spin_cnt;
	bool				wfe;
};

struct arm_smmu_cmdq {
	struct arm_smmu_queue		q;
	atomic_long_t			*valid_map;
	atomic_t			owner_prod;
	atomic_t			lock;
	bool				(*supports_cmd)(struct arm_smmu_cmdq_ent *ent);
};

static inline bool arm_smmu_cmdq_supports_cmd(struct arm_smmu_cmdq *cmdq,
					      struct arm_smmu_cmdq_ent *ent)
{
	return cmdq->supports_cmd ? cmdq->supports_cmd(ent) : true;
}

struct arm_smmu_cmdq_batch {
	u64				cmds[CMDQ_BATCH_ENTRIES * CMDQ_ENT_DWORDS];
	struct arm_smmu_cmdq		*cmdq;
	int				num;
};

struct arm_smmu_evtq {
	struct arm_smmu_queue		q;
	struct iopf_queue		*iopf;
	u32				max_stalls;
};

struct arm_smmu_priq {
	struct arm_smmu_queue		q;
};

/* High-level stream table and context descriptor structures */
struct arm_smmu_ctx_desc {
	u16				asid;
};

struct arm_smmu_ctx_desc_cfg {
	union {
		struct {
			struct arm_smmu_cd *table;
			unsigned int num_ents;
		} linear;
		struct {
			struct arm_smmu_cdtab_l1 *l1tab;
			struct arm_smmu_cdtab_l2 **l2ptrs;
			unsigned int num_l1_ents;
		} l2;
	};
	dma_addr_t			cdtab_dma;
	unsigned int			used_ssids;
	u8				in_ste;
	u8				s1fmt;
	/* log2 of the maximum number of CDs supported by this table */
	u8				s1cdmax;
};

static inline bool
arm_smmu_cdtab_allocated(struct arm_smmu_ctx_desc_cfg *cfg)
{
	return cfg->linear.table || cfg->l2.l1tab;
}

/* True if the cd table has SSIDS > 0 in use. */
static inline bool arm_smmu_ssids_in_use(struct arm_smmu_ctx_desc_cfg *cd_table)
{
	return cd_table->used_ssids;
}

struct arm_smmu_s2_cfg {
	u16				vmid;
};

struct arm_smmu_impl_ops {
	int (*device_reset)(struct arm_smmu_device *smmu);
	void (*device_remove)(struct arm_smmu_device *smmu);
	int (*init_structures)(struct arm_smmu_device *smmu);
	struct arm_smmu_cmdq *(*get_secondary_cmdq)(
		struct arm_smmu_device *smmu, struct arm_smmu_cmdq_ent *ent);
};

/* An SMMUv3 instance */
struct arm_smmu_device {
	struct device			*dev;
	struct device			*impl_dev;
	const struct arm_smmu_impl_ops	*impl_ops;

	void __iomem			*base;
	void __iomem			*page1;

	/* See ARM_SMMU_FEAT_* in arm-smmu-v3-common.h*/
	u32				features;

#define ARM_SMMU_OPT_SKIP_PREFETCH	(1 << 0)
#define ARM_SMMU_OPT_PAGE0_REGS_ONLY	(1 << 1)
#define ARM_SMMU_OPT_MSIPOLL		(1 << 2)
#define ARM_SMMU_OPT_CMDQ_FORCE_SYNC	(1 << 3)
#define ARM_SMMU_OPT_TEGRA241_CMDQV	(1 << 4)
	u32				options;

	struct arm_smmu_cmdq		cmdq;
	struct arm_smmu_evtq		evtq;
	struct arm_smmu_priq		priq;

	int				gerr_irq;
	int				combined_irq;

	unsigned long			ias; /* IPA */
	unsigned long			oas; /* PA */
	unsigned long			pgsize_bitmap;

#define ARM_SMMU_MAX_ASIDS		(1 << 16)
	unsigned int			asid_bits;

#define ARM_SMMU_MAX_VMIDS		(1 << 16)
	unsigned int			vmid_bits;
	struct ida			vmid_map;

	unsigned int			ssid_bits;
	unsigned int			sid_bits;

	struct arm_smmu_strtab_cfg	strtab_cfg;

	/* IOMMU core code handle */
	struct iommu_device		iommu;

	struct rb_root			streams;
	struct mutex			streams_mutex;
};

struct arm_smmu_stream {
	u32				id;
	struct arm_smmu_master		*master;
	struct rb_node			node;
};

struct arm_smmu_vmaster {
	struct arm_vsmmu		*vsmmu;
	unsigned long			vsid;
};

struct arm_smmu_event {
	u8				stall : 1,
					ssv : 1,
					privileged : 1,
					instruction : 1,
					s2 : 1,
					read : 1,
					ttrnw : 1,
					class_tt : 1;
	u8				id;
	u8				class;
	u16				stag;
	u32				sid;
	u32				ssid;
	u64				iova;
	u64				ipa;
	u64				fetch_addr;
	struct device			*dev;
};

/* SMMU private data for each master */
struct arm_smmu_master {
	struct arm_smmu_device		*smmu;
	struct device			*dev;
	struct arm_smmu_stream		*streams;
	struct arm_smmu_vmaster		*vmaster; /* use smmu->streams_mutex */
	/* Locked by the iommu core using the group mutex */
	struct arm_smmu_ctx_desc_cfg	cd_table;
	unsigned int			num_streams;
	bool				ats_enabled : 1;
	bool				ste_ats_enabled : 1;
	bool				stall_enabled;
	unsigned int			ssid_bits;
	unsigned int			iopf_refcount;
};

/* SMMU private data for an IOMMU domain */
enum arm_smmu_domain_stage {
	ARM_SMMU_DOMAIN_S1 = 0,
	ARM_SMMU_DOMAIN_S2,
};

struct arm_smmu_domain {
	struct arm_smmu_device		*smmu;

	struct io_pgtable_ops		*pgtbl_ops;
	atomic_t			nr_ats_masters;

	enum arm_smmu_domain_stage	stage;
	union {
		struct arm_smmu_ctx_desc	cd;
		struct arm_smmu_s2_cfg		s2_cfg;
	};

	struct iommu_domain		domain;

	/* List of struct arm_smmu_master_domain */
	struct list_head		devices;
	spinlock_t			devices_lock;
	bool				enforce_cache_coherency : 1;
	bool				nest_parent : 1;

	struct mmu_notifier		mmu_notifier;
};

struct arm_smmu_nested_domain {
	struct iommu_domain domain;
	struct arm_vsmmu *vsmmu;
	bool enable_ats : 1;

	__le64 ste[2];
};

/* The following are exposed for testing purposes. */
struct arm_smmu_entry_writer_ops;
struct arm_smmu_entry_writer {
	const struct arm_smmu_entry_writer_ops *ops;
	struct arm_smmu_master *master;
};

struct arm_smmu_entry_writer_ops {
	void (*get_used)(const __le64 *entry, __le64 *used);
	void (*sync)(struct arm_smmu_entry_writer *writer);
};

void arm_smmu_make_abort_ste(struct arm_smmu_ste *target);
void arm_smmu_make_s2_domain_ste(struct arm_smmu_ste *target,
				 struct arm_smmu_master *master,
				 struct arm_smmu_domain *smmu_domain,
				 bool ats_enabled);

#if IS_ENABLED(CONFIG_KUNIT)
void arm_smmu_get_ste_used(const __le64 *ent, __le64 *used_bits);
void arm_smmu_write_entry(struct arm_smmu_entry_writer *writer, __le64 *cur,
			  const __le64 *target);
void arm_smmu_get_cd_used(const __le64 *ent, __le64 *used_bits);
void arm_smmu_make_bypass_ste(struct arm_smmu_device *smmu,
			      struct arm_smmu_ste *target);
void arm_smmu_make_cdtable_ste(struct arm_smmu_ste *target,
			       struct arm_smmu_master *master, bool ats_enabled,
			       unsigned int s1dss);
void arm_smmu_make_sva_cd(struct arm_smmu_cd *target,
			  struct arm_smmu_master *master, struct mm_struct *mm,
			  u16 asid);
#endif

struct arm_smmu_master_domain {
	struct list_head devices_elm;
	struct arm_smmu_master *master;
	/*
	 * For nested domains the master_domain is threaded onto the S2 parent,
	 * this points to the IOMMU_DOMAIN_NESTED to disambiguate the masters.
	 */
	struct iommu_domain *domain;
	ioasid_t ssid;
	bool nested_ats_flush : 1;
	bool using_iopf : 1;
};

static inline struct arm_smmu_domain *to_smmu_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct arm_smmu_domain, domain);
}

static inline struct arm_smmu_nested_domain *
to_smmu_nested_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct arm_smmu_nested_domain, domain);
}

extern struct xarray arm_smmu_asid_xa;
extern struct mutex arm_smmu_asid_lock;

struct arm_smmu_domain *arm_smmu_domain_alloc(void);

void arm_smmu_clear_cd(struct arm_smmu_master *master, ioasid_t ssid);
struct arm_smmu_cd *arm_smmu_get_cd_ptr(struct arm_smmu_master *master,
					u32 ssid);
void arm_smmu_make_s1_cd(struct arm_smmu_cd *target,
			 struct arm_smmu_master *master,
			 struct arm_smmu_domain *smmu_domain);
void arm_smmu_write_cd_entry(struct arm_smmu_master *master, int ssid,
			     struct arm_smmu_cd *cdptr,
			     const struct arm_smmu_cd *target);

int arm_smmu_set_pasid(struct arm_smmu_master *master,
		       struct arm_smmu_domain *smmu_domain, ioasid_t pasid,
		       struct arm_smmu_cd *cd, struct iommu_domain *old);

void arm_smmu_tlb_inv_asid(struct arm_smmu_device *smmu, u16 asid);
void arm_smmu_tlb_inv_range_asid(unsigned long iova, size_t size, int asid,
				 size_t granule, bool leaf,
				 struct arm_smmu_domain *smmu_domain);
int arm_smmu_atc_inv_domain(struct arm_smmu_domain *smmu_domain,
			    unsigned long iova, size_t size);

void __arm_smmu_cmdq_skip_err(struct arm_smmu_device *smmu,
			      struct arm_smmu_cmdq *cmdq);
int arm_smmu_init_one_queue(struct arm_smmu_device *smmu,
			    struct arm_smmu_queue *q, void __iomem *page,
			    unsigned long prod_off, unsigned long cons_off,
			    size_t dwords, const char *name);
int arm_smmu_cmdq_init(struct arm_smmu_device *smmu,
		       struct arm_smmu_cmdq *cmdq);

static inline bool arm_smmu_master_canwbs(struct arm_smmu_master *master)
{
	return dev_iommu_fwspec_get(master->dev)->flags &
	       IOMMU_FWSPEC_PCI_RC_CANWBS;
}

struct arm_smmu_attach_state {
	/* Inputs */
	struct iommu_domain *old_domain;
	struct arm_smmu_master *master;
	bool cd_needs_ats;
	bool disable_ats;
	ioasid_t ssid;
	/* Resulting state */
	struct arm_smmu_vmaster *vmaster;
	bool ats_enabled;
};

int arm_smmu_attach_prepare(struct arm_smmu_attach_state *state,
			    struct iommu_domain *new_domain);
void arm_smmu_attach_commit(struct arm_smmu_attach_state *state);
void arm_smmu_install_ste_for_dev(struct arm_smmu_master *master,
				  const struct arm_smmu_ste *target);

int arm_smmu_cmdq_issue_cmdlist(struct arm_smmu_device *smmu,
				struct arm_smmu_cmdq *cmdq, u64 *cmds, int n,
				bool sync);
int arm_smmu_device_hw_probe(struct arm_smmu_device *smmu);
int arm_smmu_write_reg_sync(struct arm_smmu_device *smmu, u32 val,
			    unsigned int reg_off, unsigned int ack_off);
int arm_smmu_update_gbpa(struct arm_smmu_device *smmu, u32 set, u32 clr);
int arm_smmu_device_disable(struct arm_smmu_device *smmu);
struct iommu_group *arm_smmu_device_group(struct device *dev);
int arm_smmu_of_xlate(struct device *dev, const struct of_phandle_args *args);
void arm_smmu_get_resv_regions(struct device *dev, struct list_head *head);
int arm_smmu_init_strtab(struct arm_smmu_device *smmu);
void arm_smmu_write_strtab(struct arm_smmu_device *smmu);
void arm_smmu_init_initial_stes(struct arm_smmu_ste *strtab,
				unsigned int nent);
int arm_smmu_init_one_queue(struct arm_smmu_device *smmu,
			    struct arm_smmu_queue *q, void __iomem *page,
			    unsigned long prod_off, unsigned long cons_off,
			    size_t dwords, const char *name);
int arm_smmu_fw_probe(struct platform_device *pdev,
		      struct arm_smmu_device *smmu);
int arm_smmu_register_iommu(struct arm_smmu_device *smmu,
			    struct iommu_ops *ops, phys_addr_t ioaddr);
void arm_smmu_unregister_iommu(struct arm_smmu_device *smmu);

#ifdef CONFIG_ARM_SMMU_V3_SVA
bool arm_smmu_sva_supported(struct arm_smmu_device *smmu);
void arm_smmu_sva_notifier_synchronize(void);
struct iommu_domain *arm_smmu_sva_domain_alloc(struct device *dev,
					       struct mm_struct *mm);
#else /* CONFIG_ARM_SMMU_V3_SVA */
static inline bool arm_smmu_sva_supported(struct arm_smmu_device *smmu)
{
	return false;
}

static inline void arm_smmu_sva_notifier_synchronize(void) {}

#define arm_smmu_sva_domain_alloc NULL

#endif /* CONFIG_ARM_SMMU_V3_SVA */

#ifdef CONFIG_TEGRA241_CMDQV
struct arm_smmu_device *tegra241_cmdqv_probe(struct arm_smmu_device *smmu);
#else /* CONFIG_TEGRA241_CMDQV */
static inline struct arm_smmu_device *
tegra241_cmdqv_probe(struct arm_smmu_device *smmu)
{
	return ERR_PTR(-ENODEV);
}
#endif /* CONFIG_TEGRA241_CMDQV */

struct arm_vsmmu {
	struct iommufd_viommu core;
	struct arm_smmu_device *smmu;
	struct arm_smmu_domain *s2_parent;
	u16 vmid;
};

#if IS_ENABLED(CONFIG_ARM_SMMU_V3_IOMMUFD)
void *arm_smmu_hw_info(struct device *dev, u32 *length, u32 *type);
struct iommufd_viommu *arm_vsmmu_alloc(struct device *dev,
				       struct iommu_domain *parent,
				       struct iommufd_ctx *ictx,
				       unsigned int viommu_type);
int arm_smmu_attach_prepare_vmaster(struct arm_smmu_attach_state *state,
				    struct arm_smmu_nested_domain *nested_domain);
void arm_smmu_attach_commit_vmaster(struct arm_smmu_attach_state *state);
void arm_smmu_master_clear_vmaster(struct arm_smmu_master *master);
int arm_vmaster_report_event(struct arm_smmu_vmaster *vmaster, u64 *evt);
#else
#define arm_smmu_hw_info NULL
#define arm_vsmmu_alloc NULL

static inline int
arm_smmu_attach_prepare_vmaster(struct arm_smmu_attach_state *state,
				struct arm_smmu_nested_domain *nested_domain)
{
	return 0;
}

static inline void
arm_smmu_attach_commit_vmaster(struct arm_smmu_attach_state *state)
{
}

static inline void
arm_smmu_master_clear_vmaster(struct arm_smmu_master *master)
{
}

static inline int arm_vmaster_report_event(struct arm_smmu_vmaster *vmaster,
					   u64 *evt)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_ARM_SMMU_V3_IOMMUFD */

#endif /* _ARM_SMMU_V3_H */
