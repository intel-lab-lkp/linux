// SPDX-License-Identifier: GPL-2.0

#include <linux/platform_device.h>
#include <linux/kallsyms.h>
#include <linux/vmalloc.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/kmemdump.h>
#include <linux/module.h>
#include <linux/utsname.h>

#define BUILD_INFO_LEN		256
#define DEBUG_KINFO_MAGIC	0xCCEEDDFF

/*
 * Header structure must be byte-packed, since the table is provided to
 * bootloader.
 */
struct kernel_info {
	/* For kallsyms */
	__u8 enabled_all;
	__u8 enabled_base_relative;
	__u8 enabled_absolute_percpu;
	__u8 enabled_cfi_clang;
	__u32 num_syms;
	__u16 name_len;
	__u16 bit_per_long;
	__u16 module_name_len;
	__u16 symbol_len;
	__u64 _relative_pa;
	__u64 _text_pa;
	__u64 _stext_pa;
	__u64 _etext_pa;
	__u64 _sinittext_pa;
	__u64 _einittext_pa;
	__u64 _end_pa;
	__u64 _offsets_pa;
	__u64 _names_pa;
	__u64 _token_table_pa;
	__u64 _token_index_pa;
	__u64 _markers_pa;
	__u64 _seqs_of_names_pa;

	/* For frame pointer */
	__u32 thread_size;

	/* For virt_to_phys */
	__u64 swapper_pg_dir_pa;

	/* For linux banner */
	__u8 last_uts_release[__NEW_UTS_LEN];

	/* Info of running build */
	__u8 build_info[BUILD_INFO_LEN];

	/* For module kallsyms */
	__u32 enabled_modules_tree_lookup;
	__u32 mod_mem_offset;
	__u32 mod_kallsyms_offset;
} __packed;

struct kernel_all_info {
	__u32 magic_number;
	__u32 combined_checksum;
	struct kernel_info info;
} __packed;

struct debug_kinfo {
	struct device *dev;
	void *all_info_addr;
	u32 all_info_size;
	struct kmemdump_backend kinfo_be;
};

static struct debug_kinfo *kinfo;

#define be_to_kinfo(be) container_of(be, struct debug_kinfo, kinfo_be)

static void update_kernel_all_info(struct kernel_all_info *all_info)
{
	int index;
	struct kernel_info *info;
	u32 *checksum_info;

	all_info->magic_number = DEBUG_KINFO_MAGIC;
	all_info->combined_checksum = 0;

	info = &all_info->info;
	checksum_info = (u32 *)info;
	for (index = 0; index < sizeof(*info) / sizeof(u32); index++)
		all_info->combined_checksum ^= checksum_info[index];
}

static int build_info_set(const char *str, const struct kernel_param *kp)
{
	struct kernel_all_info *all_info = kinfo->all_info_addr;
	size_t build_info_size;

	if (kinfo->all_info_addr == 0 || kinfo->all_info_size == 0)
		return -ENAVAIL;

	all_info = (struct kernel_all_info *)kinfo->all_info_addr;
	build_info_size = sizeof(all_info->info.build_info);

	memcpy(&all_info->info.build_info, str, min(build_info_size - 1,
						    strlen(str)));
	update_kernel_all_info(all_info);

	if (strlen(str) > build_info_size) {
		pr_warn("%s: Build info buffer (len: %zd) can't hold entire string '%s'\n",
			__func__, build_info_size, str);
		return -ENOMEM;
	}

	return 0;
}

static const struct kernel_param_ops build_info_op = {
	.set = build_info_set,
};

module_param_cb(build_info, &build_info_op, NULL, 0200);
MODULE_PARM_DESC(build_info, "Write build info to field 'build_info' of debug kinfo.");

/**
 * register_kinfo_region() - Register a new kinfo region
 * @be: pointer to backend
 * @id: unique id to identify the region
 * @vaddr: virtual memory address of the region start
 * @size: size of the region
 *
 * Return: On success, it returns 0 and negative error value on failure.
 */
static int register_kinfo_region(const struct kmemdump_backend *be,
				 enum kmemdump_uid id, void *vaddr, size_t size)
{
	struct debug_kinfo *kinfo = be_to_kinfo(be);
	struct kernel_all_info *all_info = kinfo->all_info_addr;
	struct kernel_info *info = &all_info->info;

	switch (id) {
	case KMEMDUMP_ID_COREIMAGE__sinittext:
		info->_sinittext_pa = (u64)__pa(vaddr);
		break;
	case KMEMDUMP_ID_COREIMAGE__einittext:
		info->_einittext_pa = (u64)__pa(vaddr);
		break;
	case KMEMDUMP_ID_COREIMAGE__end:
		info->_end_pa = (u64)__pa(vaddr);
		break;
	case KMEMDUMP_ID_COREIMAGE__text:
		info->_text_pa = (u64)__pa(vaddr);
		break;
	case KMEMDUMP_ID_COREIMAGE__stext:
		info->_stext_pa = (u64)__pa(vaddr);
		break;
	case KMEMDUMP_ID_COREIMAGE__etext:
		info->_etext_pa = (u64)__pa(vaddr);
		break;
	case KMEMDUMP_ID_COREIMAGE_kallsyms_num_syms:
		info->num_syms = *(__u32 *)vaddr;
		break;
	case KMEMDUMP_ID_COREIMAGE_kallsyms_relative_base:
		info->_relative_pa = (u64)__pa(vaddr);
		break;
	case KMEMDUMP_ID_COREIMAGE_kallsyms_offsets:
		info->_offsets_pa = (u64)__pa(vaddr);
		break;
	case KMEMDUMP_ID_COREIMAGE_kallsyms_names:
		info->_names_pa = (u64)__pa(vaddr);
		break;
	case KMEMDUMP_ID_COREIMAGE_kallsyms_token_table:
		info->_token_table_pa = (u64)__pa(vaddr);
		break;
	case KMEMDUMP_ID_COREIMAGE_kallsyms_token_index:
		info->_token_index_pa = (u64)__pa(vaddr);
		break;
	case KMEMDUMP_ID_COREIMAGE_kallsyms_markers:
		info->_markers_pa = (u64)__pa(vaddr);
		break;
	case KMEMDUMP_ID_COREIMAGE_kallsyms_seqs_of_names:
		info->_seqs_of_names_pa = (u64)__pa(vaddr);
		break;
	case KMEMDUMP_ID_COREIMAGE_swapper_pg_dir:
		info->swapper_pg_dir_pa = (u64)__pa(vaddr);
		break;
	case KMEMDUMP_ID_COREIMAGE_init_uts_ns_name:
		strscpy(info->last_uts_release, vaddr, __NEW_UTS_LEN);
		break;
	default:
	};

	update_kernel_all_info(all_info);
	return 0;
}

/**
 * unregister_md_region() - Unregister a previously registered kinfo region
 * @id: unique id to identify the region
 *
 * Return: On success, it returns 0 and negative error value on failure.
 */
static int unregister_kinfo_region(const struct kmemdump_backend *be,
				   enum kmemdump_uid id)
{
	return 0;
}

static int debug_kinfo_probe(struct platform_device *pdev)
{
	struct device_node *mem_region;
	struct reserved_mem *rmem;
	struct kernel_info *info;
	struct kernel_all_info *all_info;

	mem_region = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (!mem_region) {
		dev_warn(&pdev->dev, "no such memory-region\n");
		return -ENODEV;
	}

	rmem = of_reserved_mem_lookup(mem_region);
	if (!rmem) {
		dev_warn(&pdev->dev, "no such reserved mem of node name %s\n",
			 pdev->dev.of_node->name);
		return -ENODEV;
	}

	/* Need to wait for reserved memory to be mapped */
	if (!rmem->priv)
		return -EPROBE_DEFER;

	if (!rmem->base || !rmem->size) {
		dev_warn(&pdev->dev, "unexpected reserved memory\n");
		return -EINVAL;
	}

	if (rmem->size < sizeof(struct kernel_all_info)) {
		dev_warn(&pdev->dev, "unexpected reserved memory size\n");
		return -EINVAL;
	}

	kinfo = kzalloc(sizeof(*kinfo), GFP_KERNEL);
	if (!kinfo)
		return -ENOMEM;

	kinfo->dev = &pdev->dev;

	strscpy(kinfo->kinfo_be.name, "debug_kinfo");
	kinfo->kinfo_be.register_region = register_kinfo_region;
	kinfo->kinfo_be.unregister_region = unregister_kinfo_region;
	kinfo->all_info_addr = rmem->priv;
	kinfo->all_info_size = rmem->size;

	all_info = kinfo->all_info_addr;

	memset(all_info, 0, sizeof(struct kernel_all_info));
	info = &all_info->info;
	info->enabled_all = IS_ENABLED(CONFIG_KALLSYMS_ALL);
	info->enabled_absolute_percpu = IS_ENABLED(CONFIG_KALLSYMS_ABSOLUTE_PERCPU);
	info->enabled_cfi_clang = IS_ENABLED(CONFIG_CFI_CLANG);
	info->name_len = KSYM_NAME_LEN;
	info->bit_per_long = BITS_PER_LONG;
	info->module_name_len = MODULE_NAME_LEN;
	info->symbol_len = KSYM_SYMBOL_LEN;
	info->thread_size = THREAD_SIZE;
	info->enabled_modules_tree_lookup = IS_ENABLED(CONFIG_MODULES_TREE_LOOKUP);
	info->mod_mem_offset = offsetof(struct module, mem);
	info->mod_kallsyms_offset = offsetof(struct module, kallsyms);

	return kmemdump_register_backend(&kinfo->kinfo_be);
}

static void debug_kinfo_remove(struct platform_device *pdev)
{
	kfree(kinfo);
	kmemdump_unregister_backend(&kinfo->kinfo_be);
}

static const struct of_device_id debug_kinfo_of_match[] = {
	{ .compatible	= "google,debug-kinfo" },
	{},
};
MODULE_DEVICE_TABLE(of, debug_kinfo_of_match);

static struct platform_driver debug_kinfo_driver = {
	.probe = debug_kinfo_probe,
	.remove = debug_kinfo_remove,
	.driver = {
		.name = "debug-kinfo",
		.of_match_table = of_match_ptr(debug_kinfo_of_match),
	},
};
module_platform_driver(debug_kinfo_driver);

MODULE_AUTHOR("Eugen Hristev <eugen.hristev@linaro.org>");
MODULE_AUTHOR("Jone Chou <jonechou@google.com>");
MODULE_DESCRIPTION("Debug Kinfo Driver");
MODULE_LICENSE("GPL");
