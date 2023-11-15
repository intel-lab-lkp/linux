// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Module version support
 *
 * Copyright (C) 2008 Rusty Russell
 */

#include <linux/module.h>
#include <linux/string.h>
#include <linux/printk.h>
#include "internal.h"

int check_version(const struct load_info *info,
		  const char *symname,
			 struct module *mod,
			 const s32 *crc)
{
	Elf_Shdr *sechdrs = info->sechdrs;
	unsigned int versindex = info->index.vers;
	unsigned int i, num_versions;
	struct modversion_info *versions;
	struct modversion_info_ext version_ext;

	/* Exporting module didn't supply crcs?  OK, we're already tainted. */
	if (!crc)
		return 1;

	/* If we have extended version info, rely on it */
	if (modversion_ext_start(info, &version_ext) >= 0) {
		do {
			if (strncmp(version_ext.name.value, symname,
				    version_ext.name.end - version_ext.name.value) != 0)
				continue;

			if (*version_ext.crc.value == *crc)
				return 1;
			pr_debug("Found checksum %X vs module %X\n",
				 *crc, *version_ext.crc.value);
			goto bad_version;
		} while (modversion_ext_advance(&version_ext) == 0);
		goto broken_toolchain;
	}

	/* No versions at all?  modprobe --force does this. */
	if (versindex == 0)
		return try_to_force_load(mod, symname) == 0;

	versions = (void *)sechdrs[versindex].sh_addr;
	num_versions = sechdrs[versindex].sh_size
		/ sizeof(struct modversion_info);

	for (i = 0; i < num_versions; i++) {
		u32 crcval;

		if (strcmp(versions[i].name, symname) != 0)
			continue;

		crcval = *crc;
		if (versions[i].crc == crcval)
			return 1;
		pr_debug("Found checksum %X vs module %lX\n",
			 crcval, versions[i].crc);
		goto bad_version;
	}

broken_toolchain:
	/* Broken toolchain. Warn once, then let it go.. */
	pr_warn_once("%s: no symbol version for %s\n", info->name, symname);
	return 1;

bad_version:
	pr_warn("%s: disagrees about version of symbol %s\n", info->name, symname);
	return 0;
}

int check_modstruct_version(const struct load_info *info,
			    struct module *mod)
{
	struct find_symbol_arg fsa = {
		.name	= "module_layout",
		.gplok	= true,
	};

	/*
	 * Since this should be found in kernel (which can't be removed), no
	 * locking is necessary -- use preempt_disable() to placate lockdep.
	 */
	preempt_disable();
	if (!find_symbol(&fsa)) {
		preempt_enable();
		BUG();
	}
	preempt_enable();
	return check_version(info, "module_layout", mod, fsa.crc);
}

/* First part is kernel version, which we ignore if module has crcs. */
int same_magic(const char *amagic, const char *bmagic,
	       bool has_crcs)
{
	if (has_crcs) {
		amagic += strcspn(amagic, " ");
		bmagic += strcspn(bmagic, " ");
	}
	return strcmp(amagic, bmagic) == 0;
}

#define MODVERSION_FIELD_START(sec, field) \
	field.value = (typeof(field.value))sec.sh_addr; \
	field.end = field.value + sec.sh_size

ssize_t modversion_ext_start(const struct load_info *info,
			     struct modversion_info_ext *start)
{
	unsigned int crc_idx = info->index.vers_ext_crc;
	unsigned int name_idx = info->index.vers_ext_name;
	Elf_Shdr *sechdrs = info->sechdrs;

	// Both of these fields are needed for this to be useful
	// Any future fields should be initialized to NULL if absent.
	if ((crc_idx == 0) || (name_idx == 0))
		return -EINVAL;

	MODVERSION_FIELD_START(sechdrs[crc_idx], start->crc);
	MODVERSION_FIELD_START(sechdrs[name_idx], start->name);

	return (start->crc.end - start->crc.value) / sizeof(*start->crc.value);
}

static int modversion_ext_s32_advance(struct modversion_info_ext_s32 *field)
{
	if (!field->value)
		return 0;
	if (field->value >= field->end)
		return -EINVAL;
	field->value++;
	return 0;
}

static int modversion_ext_string_advance(struct modversion_info_ext_string *s)
{
	if (!s->value)
		return 0;
	if (s->value >= s->end)
		return -EINVAL;
	s->value += strnlen(s->value, s->end - s->value - 1) + 1;
	if (s->value >= s->end)
		return -EINVAL;
	return 0;
}

int modversion_ext_advance(struct modversion_info_ext *start)
{
	int ret;

	ret = modversion_ext_s32_advance(&start->crc);
	if (ret < 0)
		return ret;

	ret = modversion_ext_string_advance(&start->name);
	if (ret < 0)
		return ret;

	return 0;
}

/*
 * Generate the signature for all relevant module structures here.
 * If these change, we don't want to try to parse the module.
 */
void module_layout(struct module *mod,
		   struct modversion_info *ver,
		   struct kernel_param *kp,
		   struct kernel_symbol *ks,
		   struct tracepoint * const *tp)
{
}
EXPORT_SYMBOL(module_layout);
