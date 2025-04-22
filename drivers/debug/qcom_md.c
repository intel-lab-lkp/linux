// SPDX-License-Identifier: GPL-2.0-only

#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/soc/qcom/smem.h>
#include <linux/soc/qcom/socinfo.h>
#include <linux/kmemdump.h>

/*
 * In some of the Old Qualcomm devices, boot firmware statically allocates 300
 * as total number of supported region (including all co-processors) in
 * minidump table out of which linux was using 201. In future, this limitation
 * from boot firmware might get removed by allocating the region dynamically.
 * So, keep it compatible with older devices, we can keep the current limit for
 * Linux to 201.
 */
#define MAX_NUM_ENTRIES	  201

#define MAX_NUM_OF_SS           10
#define MAX_REGION_NAME_LENGTH  16
#define SBL_MINIDUMP_SMEM_ID	602
#define MINIDUMP_REGION_VALID	   ('V' << 24 | 'A' << 16 | 'L' << 8 | 'I' << 0)
#define MINIDUMP_SS_ENCR_DONE	   ('D' << 24 | 'O' << 16 | 'N' << 8 | 'E' << 0)
#define MINIDUMP_SS_ENABLED	   ('E' << 24 | 'N' << 16 | 'B' << 8 | 'L' << 0)

#define MINIDUMP_SS_ENCR_NOTREQ	   (0 << 24 | 0 << 16 | 'N' << 8 | 'R' << 0)

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

#define MINIDUMP_MAX_NAME_LENGTH	12
/**
 * struct qcom_minidump_region - Minidump region information
 *
 * @name:	Minidump region name
 * @virt_addr:  Virtual address of the entry.
 * @phys_addr:	Physical address of the entry to dump.
 * @size:	Number of bytes to dump from @address location,
 *		and it should be 4 byte aligned.
 */
struct qcom_minidump_region {
	char		name[MINIDUMP_MAX_NAME_LENGTH];
	void		*virt_addr;
	phys_addr_t	phys_addr;
	size_t		size;
	unsigned int	id;
};

static LIST_HEAD(apss_md_rlist);

/**
 * struct md_region_list - Minidump region list struct
 *
 * @md_region:	associated minidump region
 * @list:  list head entry
 */
struct md_region_list {
	struct qcom_minidump_region md_region;
	struct list_head list;
};

static struct minidump *md;

/**
 * qcom_md_add_region() - Register region in APSS Minidump table.
 * @region: minidump region.
 *
 * Return: None
 */
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

/**
 * qcom_md_get_region_index() - Lookup minidump region by name
 * @mdss_data: minidump subsystem data
 * @region: minidump region.
 *
 * Return: On success, it returns the region index, on failure, returns
 *	negative error value
 */
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

/**
 * qcom_md_region_unregister() - Unregister region from APSS Minidump table.
 * @region: minidump region.
 *
 * Return: On success, it returns 0 and negative error value on failure.
 */
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

/**
 * qcom_md_region_register() - Register region in APSS Minidump table.
 * @region: minidump region.
 *
 * Return: On success, it returns 0 and negative error value on failure.
 */
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

/**
 * qcom_minidump_valid_region() - Checks if region is valid
 * @region: minidump region.
 *
 * Return: true if region is valid, false otherwise.
 */
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
static int qcom_minidump_region_register(const struct qcom_minidump_region *region)
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
static int qcom_minidump_region_unregister(const struct qcom_minidump_region *region)
{
	int ret;

	if (!qcom_minidump_valid_region(region))
		return -EINVAL;

	mutex_lock(&md->md_lock);
	ret = qcom_md_region_unregister(region);

	mutex_unlock(&md->md_lock);
	return ret;
}

/**
 * qcom_apss_md_table_init() - Initialize the minidump table
 * @mdss_toc: minidump subsystem table of contents
 *
 * Return: On success, it returns 0 and negative error value on failure.
 */
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

/**
 * register_md_region() - Register a new minidump region
 * @id: unique id to identify the region
 * @name: name of the region
 * @vaddr: virtual memory address of the region start
 * @size: size of the region
 *
 * Return: On success, it returns 0 and negative error value on failure.
 */
static int register_md_region(unsigned int id, char *name, void *vaddr,
			      size_t size)
{
	struct qcom_minidump_region *md_region;
	int ret;

	struct md_region_list *mdr_list =
		kzalloc(sizeof(*mdr_list), GFP_KERNEL);
	if (!mdr_list)
		return -ENOMEM;
	md_region = &mdr_list->md_region;

	scnprintf(md_region->name, sizeof(md_region->name), "K%d%.8s", id, name);
	md_region->virt_addr = vaddr;
	md_region->phys_addr = virt_to_phys(vaddr);
	md_region->size = ALIGN(size, 4);
	md_region->id = id;

	ret = qcom_minidump_region_register(md_region);
	if (ret < 0) {
		pr_err("failed to register region in minidump %d %s: err: %d\n",
		       id, name, ret);
		return ret;
	}

	list_add(&mdr_list->list, &apss_md_rlist);
	return 0;
}

/**
 * unregister_md_region() - Unregister a previously registered minidump region
 * @id: unique id to identify the region
 *
 * Return: On success, it returns 0 and negative error value on failure.
 */
static int unregister_md_region(unsigned int id)
{
	int ret = -ENOENT;
	struct md_region_list *mdr_list;
	struct md_region_list *tmp;

	list_for_each_entry_safe(mdr_list, tmp, &apss_md_rlist, list) {
		struct qcom_minidump_region *region;

		region = &mdr_list->md_region;
		if (region->id == id) {
			ret = qcom_minidump_region_unregister(region);
			list_del(&mdr_list->list);
			return ret;
		}
	}

	pr_err("failed to unregister region from minidump %d\n", ret);

	return ret;
}

static struct kmemdump_backend qcom_md_backend = {
	.name = "qcom_md",
	.register_region = register_md_region,
	.unregister_region = unregister_md_region,
};

static int qcom_md_probe(struct platform_device *pdev)
{
	struct minidump_global_toc *mdgtoc;
	size_t size;
	int ret;

	md = devm_kzalloc(&pdev->dev, sizeof(*md), GFP_KERNEL);

	md->dev = &pdev->dev;

	mdgtoc = qcom_smem_get(QCOM_SMEM_HOST_ANY, SBL_MINIDUMP_SMEM_ID, &size);
	if (IS_ERR(mdgtoc)) {
		ret = PTR_ERR(mdgtoc);
		dev_err(md->dev, "Couldn't find minidump smem item %d\n", ret);
	}

	if (size < sizeof(*mdgtoc) || !mdgtoc->status) {
		dev_err(md->dev, "minidump table is not initialized %d\n", ret);
		return -ENAVAIL;
	}

	mutex_init(&md->md_lock);

	ret = qcom_apss_md_table_init(&mdgtoc->subsystems[MINIDUMP_APSS_DESC]);
	if (ret) {
		dev_err(md->dev, "apss minidump initialization failed %d\n", ret);
		return ret;
	}

	return kmemdump_register_backend(&qcom_md_backend);
}

static void qcom_md_remove(struct platform_device *pdev)
{
	kmemdump_unregister_backend(&qcom_md_backend);
}

static struct platform_driver qcom_md_driver = {
	.probe = qcom_md_probe,
	.remove = qcom_md_remove,
	.driver  = {
		.name = "qcom-md",
	},
};

module_platform_driver(qcom_md_driver);

MODULE_AUTHOR("Eugen Hristev <eugen.hristev@linaro.org>");
MODULE_AUTHOR("Mukesh Ojha <quic_mojha@quicinc.com>");
MODULE_DESCRIPTION("Qualcomm kmemdump minidump backend driver");
MODULE_LICENSE("GPL");
