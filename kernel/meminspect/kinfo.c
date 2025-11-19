// SPDX-License-Identifier: GPL-2.0
/*
 *
 * Copyright 2002 Rusty Russell <rusty@rustcorp.com.au> IBM Corporation
 * Copyright 2021 Google LLC
 * Copyright 2025 Linaro Ltd. Eugen Hristev <eugen.hristev@linaro.org>
 */
#include <linux/container_of.h>
#include <linux/kallsyms.h>
#include <linux/meminspect.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/utsname.h>

#define BUILD_INFO_LEN		256
#define DEBUG_KINFO_MAGIC	0xcceeddff

/*
 * Header structure must be byte-packed, since the table is provided to
 * bootloader.
 */
struct kernel_info {
	/* For kallsyms */
	u8 enabled_all;
	u8 enabled_base_relative;
	u8 enabled_absolute_percpu;
	u8 enabled_cfi_clang;
	u32 num_syms;
	u16 name_len;
	u16 bit_per_long;
	u16 module_name_len;
	u16 symbol_len;
	u64 _relative_pa;
	u64 _text_pa;
	u64 _stext_pa;
	u64 _etext_pa;
	u64 _sinittext_pa;
	u64 _einittext_pa;
	u64 _end_pa;
	u64 _offsets_pa;
	u64 _names_pa;
	u64 _token_table_pa;
	u64 _token_index_pa;
	u64 _markers_pa;
	u64 _seqs_of_names_pa;

	/* For frame pointer */
	u32 thread_size;

	/* For virt_to_phys */
	u64 swapper_pg_dir_pa;

	/* For linux banner */
	u8 last_uts_release[__NEW_UTS_LEN];

	/* Info of running build */
	u8 build_info[BUILD_INFO_LEN];

	/* For module kallsyms */
	u32 enabled_modules_tree_lookup;
	u32 mod_mem_offset;
	u32 mod_kallsyms_offset;
} __packed;

struct kernel_all_info {
	u32 magic_number;
	u32 combined_checksum;
	struct kernel_info info;
} __packed;

struct debug_kinfo {
	struct device *dev;
	void *all_info_addr;
	size_t all_info_size;
	struct notifier_block nb;
};

static void update_kernel_all_info(struct kernel_all_info *all_info)
{
	struct kernel_info *info;
	u32 *checksum_info;
	int index;

	all_info->magic_number = DEBUG_KINFO_MAGIC;
	all_info->combined_checksum = 0;

	info = &all_info->info;
	checksum_info = (u32 *)info;
	for (index = 0; index < sizeof(*info) / sizeof(u32); index++)
		all_info->combined_checksum ^= checksum_info[index];
}

static u8 global_build_info[BUILD_INFO_LEN];

static int build_info_set(const char *str, const struct kernel_param *kp)
{
	size_t build_info_size = sizeof(global_build_info);

	if (strlen(str) > build_info_size)
		return -ENOMEM;
	memcpy(global_build_info, str, min(build_info_size - 1, strlen(str)));
	return 0;
}

static const struct kernel_param_ops build_info_op = {
	.set = build_info_set,
};

module_param_cb(build_info, &build_info_op, NULL, 0200);
MODULE_PARM_DESC(build_info, "Write build info to field 'build_info' of debug kinfo.");

static void __maybe_unused register_kinfo_region(void *priv,
						 const struct inspect_entry *e)
{
	struct debug_kinfo *kinfo = priv;
	struct kernel_all_info *all_info = kinfo->all_info_addr;
	struct kernel_info *info = &all_info->info;
	struct uts_namespace *uts;
	u64 paddr;

	if (e->pa)
		paddr = e->pa;
	else
		paddr = __pa(e->va);

	switch (e->id) {
	case MEMINSPECT_ID__sinittext:
		info->_sinittext_pa = paddr;
		break;
	case MEMINSPECT_ID__einittext:
		info->_einittext_pa = paddr;
		break;
	case MEMINSPECT_ID__end:
		info->_end_pa = paddr;
		break;
	case MEMINSPECT_ID__text:
		info->_text_pa = paddr;
		break;
	case MEMINSPECT_ID__stext:
		info->_stext_pa = paddr;
		break;
	case MEMINSPECT_ID__etext:
		info->_etext_pa = paddr;
		break;
	case MEMINSPECT_ID_kallsyms_num_syms:
		info->num_syms = *(__u32 *)e->va;
		break;
	case MEMINSPECT_ID_kallsyms_relative_base:
		info->_relative_pa = (u64)__pa(*(u64 *)e->va);
		break;
	case MEMINSPECT_ID_kallsyms_offsets:
		info->_offsets_pa = paddr;
		break;
	case MEMINSPECT_ID_kallsyms_names:
		info->_names_pa = paddr;
		break;
	case MEMINSPECT_ID_kallsyms_token_table:
		info->_token_table_pa = paddr;
		break;
	case MEMINSPECT_ID_kallsyms_token_index:
		info->_token_index_pa = paddr;
		break;
	case MEMINSPECT_ID_kallsyms_markers:
		info->_markers_pa = paddr;
		break;
	case MEMINSPECT_ID_kallsyms_seqs_of_names:
		info->_seqs_of_names_pa = paddr;
		break;
	case MEMINSPECT_ID_swapper_pg_dir:
		info->swapper_pg_dir_pa = paddr;
		break;
	case MEMINSPECT_ID_init_uts_ns:
		if (!e->va)
			return;
		uts = e->va;
		strscpy(info->last_uts_release, uts->name.release, __NEW_UTS_LEN);
		break;
	default:
		break;
	};

	update_kernel_all_info(all_info);
}

static int kinfo_notifier_cb(struct notifier_block *nb,
			     unsigned long code, void *entry)
{
	struct debug_kinfo *kinfo = container_of(nb, struct debug_kinfo, nb);

	if (code == MEMINSPECT_NOTIFIER_ADD)
		register_kinfo_region(kinfo, entry);

	return NOTIFY_DONE;
}

static int debug_kinfo_probe(struct platform_device *pdev)
{
	struct kernel_all_info *all_info;
	struct device *dev = &pdev->dev;
	struct device_node *mem_region;
	struct reserved_mem *rmem;
	struct debug_kinfo *kinfo;
	struct kernel_info *info;

	mem_region = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!mem_region)
		return dev_err_probe(dev, -ENODEV, "no such memory-region\n");

	rmem = of_reserved_mem_lookup(mem_region);
	if (!rmem)
		return dev_err_probe(dev, -ENODEV, "no such reserved mem of node name %s\n",
			      dev->of_node->name);

	/* Need to wait for reserved memory to be mapped */
	if (!rmem->priv)
		return -EPROBE_DEFER;

	if (!rmem->base || !rmem->size)
		dev_err_probe(dev, -EINVAL, "unexpected reserved memory\n");

	if (rmem->size < sizeof(struct kernel_all_info))
		dev_err_probe(dev, -EINVAL, "reserved memory size too small\n");

	kinfo = devm_kzalloc(dev, sizeof(*kinfo), GFP_KERNEL);
	if (!kinfo)
		return -ENOMEM;
	platform_set_drvdata(pdev, kinfo);

	kinfo->dev = dev;

	kinfo->all_info_addr = rmem->priv;
	kinfo->all_info_size = rmem->size;

	all_info = kinfo->all_info_addr;

	memset(all_info, 0, sizeof(struct kernel_all_info));
	info = &all_info->info;
	info->enabled_all = IS_ENABLED(CONFIG_KALLSYMS_ALL);
	info->enabled_absolute_percpu = IS_ENABLED(CONFIG_KALLSYMS_ABSOLUTE_PERCPU);
	info->enabled_base_relative = IS_ENABLED(CONFIG_KALLSYMS_BASE_RELATIVE);
	info->enabled_cfi_clang = IS_ENABLED(CONFIG_CFI_CLANG);
	info->name_len = KSYM_NAME_LEN;
	info->bit_per_long = BITS_PER_LONG;
	info->module_name_len = MODULE_NAME_LEN;
	info->symbol_len = KSYM_SYMBOL_LEN;
	info->thread_size = THREAD_SIZE;
	info->enabled_modules_tree_lookup = IS_ENABLED(CONFIG_MODULES_TREE_LOOKUP);
	info->mod_mem_offset = offsetof(struct module, mem);
	info->mod_kallsyms_offset = offsetof(struct module, kallsyms);

	memcpy(info->build_info, global_build_info, strlen(global_build_info));

	kinfo->nb.notifier_call = kinfo_notifier_cb;

	meminspect_notifier_register(&kinfo->nb);
	meminspect_lock_traverse(kinfo, register_kinfo_region);

	return 0;
}

static void debug_kinfo_remove(struct platform_device *pdev)
{
	struct debug_kinfo *kinfo = platform_get_drvdata(pdev);

	meminspect_notifier_unregister(&kinfo->nb);
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
MODULE_DESCRIPTION("meminspect Kinfo Driver");
MODULE_LICENSE("GPL");
