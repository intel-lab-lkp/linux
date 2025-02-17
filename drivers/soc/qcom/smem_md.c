// SPDX-License-Identifier: GPL-2.0-only

#include <linux/hwspinlock.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/devcoredump.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/soc/qcom/smem.h>
#include <linux/soc/qcom/socinfo.h>
#include <linux/dma-mapping.h>
#include <linux/pstore_smem.h>
#include <linux/pstore_zone.h>
#include <linux/pstore.h>

#define MAX_NUM_ENTRIES	  201
#define MAX_STRTBL_SIZE	  (MAX_NUM_ENTRIES * MAX_REGION_NAME_LENGTH)

#define MAX_NUM_OF_SS           10
#define MAX_REGION_NAME_LENGTH  16
#define SBL_MINIDUMP_SMEM_ID	602
#define MINIDUMP_REGION_VALID	   ('V' << 24 | 'A' << 16 | 'L' << 8 | 'I' << 0)
#define MINIDUMP_SS_ENCR_DONE	   ('D' << 24 | 'O' << 16 | 'N' << 8 | 'E' << 0)
#define MINIDUMP_SS_ENABLED	   ('E' << 24 | 'N' << 16 | 'B' << 8 | 'L' << 0)
#define MINIDUMP_REGION_INVALID	   ('I' << 24 | 'N' << 16 | 'V' << 8 | 'A' << 0)
#define MINIDUMP_REGION_INIT	   ('I' << 24 | 'N' << 16 | 'I' << 8 | 'T' << 0)
#define MINIDUMP_REGION_NOINIT	   0

#define MINIDUMP_SS_ENCR_REQ	   (0 << 24 | 'Y' << 16 | 'E' << 8 | 'S' << 0)
#define MINIDUMP_SS_ENCR_NOTREQ	   (0 << 24 | 0 << 16 | 'N' << 8 | 'R' << 0)
#define MINIDUMP_SS_ENCR_START	   ('S' << 24 | 'T' << 16 | 'R' << 8 | 'T' << 0)

#define MINIDUMP_APSS_DESC	   0

/**
 * struct minidump - Minidump driver data information
 * @apss_data: APSS driver data
 * @md_lock: Lock to protect access to APSS minidump table
 */
struct minidump {
	struct device		*dev;
	struct minidump_ss_data	*apss_data;
	struct mutex		md_lock;
};

/**
 * struct minidump_region - Minidump region
 * @name		: Name of the region to be dumped
 * @seq_num:		: Use to differentiate regions with same name.
 * @valid		: This entry to be dumped (if set to 1)
 * @address		: Physical address of region to be dumped
 * @size		: Size of the region
 */
struct minidump_region {
	char	name[MAX_REGION_NAME_LENGTH];
	__le32	seq_num;
	__le32	valid;
	__le64	address;
	__le64	size;
};

/**
 * struct minidump_subsystem - Subsystem's SMEM Table of content
 * @status : Subsystem toc init status
 * @enabled : if set to 1, this region would be copied during coredump
 * @encryption_status: Encryption status for this subsystem
 * @encryption_required : Decides to encrypt the subsystem regions or not
 * @region_count : Number of regions added in this subsystem toc
 * @regions_baseptr : regions base pointer of the subsystem
 */
struct minidump_subsystem {
	__le32	status;
	__le32	enabled;
	__le32	encryption_status;
	__le32	encryption_required;
	__le32	region_count;
	__le64	regions_baseptr;
};

/**
 * struct minidump_global_toc - Global Table of Content
 * @status : Global Minidump init status
 * @md_revision : Minidump revision
 * @enabled : Minidump enable status
 * @subsystems : Array of subsystems toc
 */
struct minidump_global_toc {
	__le32				status;
	__le32				md_revision;
	__le32				enabled;
	struct minidump_subsystem	subsystems[MAX_NUM_OF_SS];
};
/**
 * struct minidump_ss_data - Minidump subsystem private data
 * @md_ss_toc: Application Subsystem TOC pointer
 * @md_regions: Application Subsystem region base pointer
 */
struct minidump_ss_data {
	struct minidump_subsystem *md_ss_toc;
	struct minidump_region	  *md_regions;
};

static struct minidump *md;

static void qcom_md_add_region(const struct qcom_minidump_region *region)
{
	struct minidump_subsystem *mdss_toc = md->apss_data->md_ss_toc;
	struct minidump_region *mdr;
	unsigned int region_cnt;

	region_cnt = le32_to_cpu(mdss_toc->region_count);
	mdr = &md->apss_data->md_regions[region_cnt];
	strscpy(mdr->name, region->name, sizeof(mdr->name));
	mdr->address = cpu_to_le64(region->phys_addr);
	mdr->size = cpu_to_le64(region->size);
	mdr->valid = cpu_to_le32(MINIDUMP_REGION_VALID);
	region_cnt++;
	mdss_toc->region_count = cpu_to_le32(region_cnt);
}

static int qcom_md_get_region_index(struct minidump_ss_data *mdss_data,
				    const struct qcom_minidump_region *region)
{
	struct minidump_subsystem *mdss_toc = mdss_data->md_ss_toc;
	struct minidump_region *mdr;
	unsigned int i;
	unsigned int count;

	count = le32_to_cpu(mdss_toc->region_count);
	for (i = 0; i < count; i++) {
		mdr = &mdss_data->md_regions[i];
		if (!strcmp(mdr->name, region->name))
			return i;
	}

	return -ENOENT;
}

static int qcom_md_region_unregister(const struct qcom_minidump_region *region)
{
	struct minidump_ss_data *mdss_data = md->apss_data;
	struct minidump_subsystem *mdss_toc = mdss_data->md_ss_toc;
	struct minidump_region *mdr;
	unsigned int region_cnt;
	unsigned int idx;
	int ret;

	ret = qcom_md_get_region_index(mdss_data, region);
	if (ret < 0) {
		dev_err(md->dev, "%s region is not present\n", region->name);
		return ret;
	}

	idx = ret;
	mdr = &mdss_data->md_regions[0];
	region_cnt = le32_to_cpu(mdss_toc->region_count);
	/*
	 * Left shift all the regions exist after this removed region
	 * index by 1 to fill the gap and zero out the last region
	 * present at the end.
	 */
	memmove(&mdr[idx], &mdr[idx + 1], (region_cnt - idx - 1) * sizeof(*mdr));
	memset(&mdr[region_cnt - 1], 0, sizeof(*mdr));
	region_cnt--;
	mdss_toc->region_count = cpu_to_le32(region_cnt);

	return 0;
}

static int qcom_md_region_register(const struct qcom_minidump_region *region)
{
	struct minidump_ss_data *mdss_data = md->apss_data;
	struct minidump_subsystem *mdss_toc = mdss_data->md_ss_toc;
	unsigned int num_region;
	int ret;

	ret = qcom_md_get_region_index(mdss_data, region);
	if (ret >= 0) {
		dev_info(md->dev, "%s region is already registered\n", region->name);
		return -EEXIST;
	}

	/* Check if there is a room for a new entry */
	num_region = le32_to_cpu(mdss_toc->region_count);
	if (num_region >= MAX_NUM_ENTRIES) {
		dev_err(md->dev, "maximum region limit %u reached\n", num_region);
		return -ENOSPC;
	}

	qcom_md_add_region(region);

	return 0;
}

static bool qcom_minidump_valid_region(const struct qcom_minidump_region *region)
{
	return region &&
		strnlen(region->name, MINIDUMP_MAX_NAME_LENGTH) < MINIDUMP_MAX_NAME_LENGTH &&
			region->virt_addr &&
			region->size &&
			IS_ALIGNED(region->size, 4);
}

/**
 * qcom_minidump_region_register() - Register region in APSS Minidump table.
 * @region: minidump region.
 *
 * Return: On success, it returns 0 and negative error value on failure.
 */
int qcom_minidump_region_register(const struct qcom_minidump_region *region)
{
	int ret;

	if (!qcom_minidump_valid_region(region))
		return -EINVAL;

	mutex_lock(&md->md_lock);
	ret = qcom_md_region_register(region);

	mutex_unlock(&md->md_lock);
	return ret;
}

/**
 * qcom_minidump_region_unregister() - Unregister region from APSS Minidump table.
 * @region: minidump region.
 *
 * Return: On success, it returns 0 and negative error value on failure.
 */
int qcom_minidump_region_unregister(const struct qcom_minidump_region *region)
{
	int ret;

	if (!qcom_minidump_valid_region(region))
		return -EINVAL;

	mutex_lock(&md->md_lock);
	ret = qcom_md_region_unregister(region);

	mutex_unlock(&md->md_lock);
	return ret;
}


static int qcom_apss_md_table_init(struct minidump_subsystem *mdss_toc)
{
	struct minidump_ss_data *mdss_data;

	mdss_data = devm_kzalloc(md->dev, sizeof(*mdss_data), GFP_KERNEL);
	if (!mdss_data)
		return -ENOMEM;

	mdss_data->md_ss_toc = mdss_toc;
	mdss_data->md_regions = devm_kcalloc(md->dev, MAX_NUM_ENTRIES,
					     sizeof(*mdss_data->md_regions),
					     GFP_KERNEL);
	if (!mdss_data->md_regions)
		return -ENOMEM;

	mdss_toc = mdss_data->md_ss_toc;
	mdss_toc->regions_baseptr = cpu_to_le64(virt_to_phys(mdss_data->md_regions));
	mdss_toc->enabled = cpu_to_le32(MINIDUMP_SS_ENABLED);
	mdss_toc->status = cpu_to_le32(1);
	mdss_toc->region_count = cpu_to_le32(0);

	/* Tell bootloader not to encrypt the regions of this subsystem */
	mdss_toc->encryption_status = cpu_to_le32(MINIDUMP_SS_ENCR_DONE);
	mdss_toc->encryption_required = cpu_to_le32(MINIDUMP_SS_ENCR_NOTREQ);

	md->apss_data = mdss_data;

	return 0;
}

int qcom_smem_md_init(struct device *dev)
{
	struct minidump_global_toc *mdgtoc;
	size_t size;
	int ret;

	md = devm_kzalloc(dev, sizeof(*md), GFP_KERNEL);

	md->dev = dev;

	mdgtoc = qcom_smem_get(QCOM_SMEM_HOST_ANY, SBL_MINIDUMP_SMEM_ID, &size);
	if (IS_ERR(mdgtoc)) {
		ret = PTR_ERR(mdgtoc);
		dev_err(md->dev, "Couldn't find minidump smem item %d\n", ret);
	}

	if (size < sizeof(*mdgtoc) || !mdgtoc->status) {
		ret = -EINVAL;
		dev_err(md->dev, "minidump table is not initialized %d\n", ret);
	}

	mutex_init(&md->md_lock);

	ret = qcom_apss_md_table_init(&mdgtoc->subsystems[MINIDUMP_APSS_DESC]);
	if (ret)
		dev_err(md->dev, "apss minidump initialization failed %d\n", ret);
	return ret;
}
