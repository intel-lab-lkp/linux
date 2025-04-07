/* SPDX-License-Identifier: GPL-2.0 */

/* Bits stolen from OOBMSM VSEC discovery code */

enum pmt_feature_id {
	FEATURE_INVALID			= 0x0,
	FEATURE_PER_CORE_PERF_TELEM	= 0x1,
	FEATURE_PER_CORE_ENV_TELEM	= 0x2,
	FEATURE_PER_RMID_PERF_TELEM	= 0x3,
	FEATURE_ACCEL_TELEM		= 0x4,
	FEATURE_UNCORE_TELEM		= 0x5,
	FEATURE_CRASH_LOG		= 0x6,
	FEATURE_PETE_LOG		= 0x7,
	FEATURE_TPMI_CTRL		= 0x8,
	FEATURE_RESERVED		= 0x9,
	FEATURE_TRACING			= 0xA,
	FEATURE_PER_RMID_ENERGY_TELEM	= 0xB,
	FEATURE_MAX			= 0xB,
};

/**
 * struct oobmsm_plat_info - Platform information for a device instance
 * @cdie_mask:       Mask of all compute dies in the partition
 * @package_id:      CPU Package id
 * @partition:       Package partition id when multiple VSEC PCI devices per package
 * @segment:         PCI segment ID
 * @bus_number:      PCI bus number
 * @device_number:   PCI device number
 * @function_number: PCI function number
 *
 * Structure to store platform data for a OOBMSM device instance.
 */
struct oobmsm_plat_info {
	u16 cdie_mask;
	u8 package_id;
	u8 partition;
	u8 segment;
	u8 bus_number;
	u8 device_number;
	u8 function_number;
};

enum oobmsm_supplier_type {
	OOBMSM_SUP_PLAT_INFO,
	OOBMSM_SUP_DISC_INFO,
	OOBMSM_SUP_S3M_SIMICS,
	OOBMSM_SUP_TYPE_MAX
};

struct oobmsm_mapping_supplier {
	struct device *supplier_dev[OOBMSM_SUP_TYPE_MAX];
	struct oobmsm_plat_info plat_info;
	unsigned long features;
};

struct telemetry_region {
	struct oobmsm_plat_info	plat_info;
	void __iomem		*addr;
	size_t			size;
	u32			guid;
	u32			num_rmids;
};

struct pmt_feature_group {
	enum pmt_feature_id	id;
	int			count;
	struct kref		kref;
	struct telemetry_region	regions[];
};

struct pmt_feature_group *intel_pmt_get_regions_by_feature(enum pmt_feature_id id);

void intel_pmt_put_feature_group(struct pmt_feature_group *feature_group);
