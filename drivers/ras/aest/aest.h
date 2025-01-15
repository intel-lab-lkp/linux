/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ARM Error Source Table Support
 *
 * Copyright (c) 2021-2024, Alibaba Group.
 */

#include <linux/acpi_aest.h>
#include <asm/ras.h>

#define MAX_GSI_PER_NODE 2
#define AEST_MAX_PPI 3
#define DEFAULT_CE_THRESHOLD 1

#define record_read(record, offset) \
	record->access->read(record->regs_base, offset)
#define record_write(record, offset, val) \
	record->access->write(record->regs_base, offset, val)

#define aest_dev_err(__adev, format, ...)	\
	dev_err((__adev)->dev, format, ##__VA_ARGS__)
#define aest_dev_info(__adev, format, ...)	\
	dev_info((__adev)->dev, format, ##__VA_ARGS__)
#define aest_dev_dbg(__adev, format, ...)	\
	dev_dbg((__adev)->dev, format, ##__VA_ARGS__)

#define aest_node_err(__node, format, ...)	\
	dev_err((__node)->adev->dev, "%s: " format, (__node)->name, ##__VA_ARGS__)
#define aest_node_info(__node, format, ...)	\
	dev_info((__node)->adev->dev, "%s: " format, (__node)->name, ##__VA_ARGS__)
#define aest_node_dbg(__node, format, ...)	\
	dev_dbg((__node)->adev->dev, "%s: " format, (__node)->name, ##__VA_ARGS__)

#define aest_record_err(__record, format, ...)	\
	dev_err((__record)->node->adev->dev, "%s: %s: " format, \
		(__record)->node->name, (__record)->name, ##__VA_ARGS__)
#define aest_record_info(__record, format, ...)	\
	dev_info((__record)->node->adev->dev, "%s: %s: " format, \
		(__record)->node->name, (__record)->name, ##__VA_ARGS__)
#define aest_record_dbg(__record, format, ...)	\
	dev_dbg((__record)->node->adev->dev, "%s: %s: " format, \
		(__record)->node->name, (__record)->name, ##__VA_ARGS__)

#define ERXFR			0x0
#define ERXCTLR			0x8
#define ERXSTATUS		0x10
#define ERXADDR			0x18
#define ERXMISC0		0x20
#define ERXMISC1		0x28
#define ERXMISC2		0x30
#define ERXMISC3		0x38

#define ERXGROUP		0xE00
#define GIC_ERRDEVARCH		0xFFBC

extern struct xarray *aest_array;

struct aest_event {
	struct llist_node llnode;
	char *node_name;
	u32 type;
	/*
	 * Different nodes have different meanings:
	 *   - Processor node	: processor number.
	 *   - Memory node	: SRAT proximity domain.
	 *   - SMMU node	: IORT proximity domain.
	 *   - GIC node		: interface type.
	 */
	u32 id0;
	/*
	 * Different nodes have different meanings:
	 *   - Processor node	: processor resource type.
	 *   - Memory node	: Non.
	 *   - SMMU node	: subcomponent reference.
	 *   - Vendor node	: Unique ID.
	 *   - GIC node		: instance identifier.
	 */
	u32 id1;
	char *hid;		// Vendor node	: hardware ID.
	u32 index;
	u64 ce_threshold;
	int addressing_mode;
	struct ras_ext_regs regs;

	void *vendor_data;
	size_t vendor_data_size;
};

struct aest_access {
	u64 (*read)(void *base, u32 offset);
	void (*write)(void *base, u32 offset, u64 val);
};

struct ce_threshold_info {
	const u64			max_count;
	const u64			mask;
	const u64			shift;
};

struct ce_threshold {
	const struct ce_threshold_info	*info;
	u64				count;
	u64				threshold;
	u64				reg_val;
};

struct aest_record {
	char				*name;
	int				index;
	void __iomem			*regs_base;

	/*
	 * This bit specifies the addressing mode  to populate the ERR_ADDR
	 * register:
	 *   0b: Error record reports System Physical Addresses (SPA) in
	 *       the ERR_ADDR register.
	 *   1b: Error record reports error node-specific Logical Addresses(LA)
	 *       in the ERR_ADD register. OS must use other means to translate
	 *       the reported LA into SPA
	 */
	int				addressing_mode;
	u64				fr;
	struct aest_node		*node;

	struct dentry			*debugfs;
	struct ce_threshold		ce;
	enum ras_ce_threshold		threshold_type;
	const struct aest_access	*access;

	void				*vendor_data;
	size_t				vendor_data_size;
};

struct aest_node {
	char				*name;
	u8				type;
	void				*errgsr;
	void				*inj;
	void				*irq_config;
	void				*base;

	/*
	 * This bitmap indicates which of the error records within this error
	 * node must be polled for error status.
	 * Bit[n] of this field pertains to error record corresponding to
	 * index n in this error group.
	 * Bit[n] = 0b: Error record at index n needs to be polled.
	 * Bit[n] = 1b: Error record at index n do not needs to be polled.
	 */
	unsigned long			*record_implemented;
	/*
	 * This bitmap indicates which of the error records within this error
	 * node support error status reporting using ERRGSR register.
	 * Bit[n] of this field pertains to error record corresponding to
	 * index n in this error group.
	 * Bit[n] = 0b: Error record at index n supports error status reporting
	 *              through ERRGSR.S.
	 * Bit[n] = 1b: Error record at index n does not support error reporting
	 *              through the ERRGSR.S bit If this error record is
	 *              implemented, then it must be polled explicitly for
	 *              error events.
	 */
	unsigned long			*status_reporting;
	int				version;

	struct aest_device		*adev;
	struct acpi_aest_node		*info;
	struct dentry			*debugfs;

	int				record_count;
	struct aest_record		*records;

	struct aest_node __percpu	*oncore_node;
};

struct aest_device {
	struct device			*dev;
	u32				type;
	int				node_cnt;
	struct aest_node		*nodes;

	struct work_struct		aest_work;
	struct gen_pool			*pool;
	struct llist_head		event_list;

	int				irq[MAX_GSI_PER_NODE];
	u32				uid;
	struct aest_device __percpu	*adev_oncore;

	struct dentry			*debugfs;
};

struct aest_node_context {
	struct aest_node		*node;
	unsigned long			*bitmap;
	void				(*func)(struct aest_record *record,
							void *data);
	void				*data;
	int				ret;
};

#define CASE_READ(res, x)						\
	case (x): {							\
		res = read_sysreg_s(SYS_##x##_EL1);			\
		break;							\
	}

#define CASE_WRITE(val, x)						\
	case (x): {							\
		write_sysreg_s((val), SYS_##x##_EL1);			\
		break;							\
	}

static inline u64 aest_sysreg_read(void *__unused, u32 offset)
{
	u64 res;

	switch (offset) {
	CASE_READ(res, ERXFR)
	CASE_READ(res, ERXCTLR)
	CASE_READ(res, ERXSTATUS)
	CASE_READ(res, ERXADDR)
	CASE_READ(res, ERXMISC0)
	CASE_READ(res, ERXMISC1)
	CASE_READ(res, ERXMISC2)
	CASE_READ(res, ERXMISC3)
	default :
		res = 0;
	}
	return res;
}

static inline void aest_sysreg_write(void *base, u32 offset, u64 val)
{
	switch (offset) {
	CASE_WRITE(val, ERXFR)
	CASE_WRITE(val, ERXCTLR)
	CASE_WRITE(val, ERXSTATUS)
	CASE_WRITE(val, ERXADDR)
	CASE_WRITE(val, ERXMISC0)
	CASE_WRITE(val, ERXMISC1)
	CASE_WRITE(val, ERXMISC2)
	CASE_WRITE(val, ERXMISC3)
	default :
		return;
	}
}

static inline u64 aest_iomem_read(void *base, u32 offset)
{
	return readq_relaxed(base + offset);
	return 0;
}

static inline void aest_iomem_write(void *base, u32 offset, u64 val)
{
	writeq_relaxed(val, base + offset);
}

/* access type is decided by AEST interface type. */
static const struct aest_access aest_access[] = {
	[ACPI_AEST_NODE_SYSTEM_REGISTER] = {
		.read = aest_sysreg_read,
		.write = aest_sysreg_write,
	},

	[ACPI_AEST_NODE_MEMORY_MAPPED] = {
		.read = aest_iomem_read,
		.write = aest_iomem_write,
	},
	[ACPI_AEST_NODE_SINGLE_RECORD_MEMORY_MAPPED] = {
		.read = aest_iomem_read,
		.write = aest_iomem_write,
	},
	{ }
};

static inline bool aest_dev_is_oncore(struct aest_device *adev)
{
	return adev->type == ACPI_AEST_PROCESSOR_ERROR_NODE;
}

/*
 * Each PE may has multi error record, you must selects an error
 * record to be accessed through the Error Record System
 * registers.
 */
static inline void aest_select_record(struct aest_node *node, int index)
{
	if (node->type == ACPI_AEST_PROCESSOR_ERROR_NODE) {
		write_sysreg_s(index, SYS_ERRSELR_EL1);
		isb();
	}
}

/* Ensure all writes has taken effect. */
static inline void aest_sync(struct aest_node *node)
{
	if (node->type == ACPI_AEST_PROCESSOR_ERROR_NODE)
		isb();
}

static const char * const aest_node_name[] = {
	[ACPI_AEST_PROCESSOR_ERROR_NODE] = "processor",
	[ACPI_AEST_MEMORY_ERROR_NODE] = "memory",
	[ACPI_AEST_SMMU_ERROR_NODE] = "smmu",
	[ACPI_AEST_VENDOR_ERROR_NODE] = "vendor",
	[ACPI_AEST_GIC_ERROR_NODE] = "gic",
	[ACPI_AEST_PCIE_ERROR_NODE] = "pcie",
	[ACPI_AEST_PROXY_ERROR_NODE] = "proxy",
};

static inline int
aest_set_name(struct aest_device *adev, struct aest_hnode *ahnode)
{
	adev->dev->init_name = devm_kasprintf(adev->dev, GFP_KERNEL,
					"%s%d", aest_node_name[ahnode->type],
						adev->uid);
	if (!adev->dev->init_name)
		return -ENOMEM;

	return 0;
}
