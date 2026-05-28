// SPDX-License-Identifier: GPL-2.0-only
/*
 * Preserve pages owned by Microsoft Hypervisor
 *
 * When handing pages to MSHV and kexec'ing, the next kernel needs to know which
 * pages not to touch. Handles this preservation here.
 *
 * Copyright (C) 2026 Microsoft Corporation, Jork Loeser <jloeser@microsoft.com>
 */

#define pr_fmt(fmt) "mshv: " fmt

#include <asm/mshyperv.h>
#include <linux/kexec.h>
#include <linux/kexec_handover.h>
#include <linux/kho_radix_tree.h>
#include <linux/libfdt.h>
#include <linux/reboot.h>
#include "mshv_page_preserve.h"

#define FDT_SUBTREE_MSHV "mshv_prsv_pt"
#define MSHV_KHO_COMPAT_STR "mshv_kho-v1"

static void *fdt_page;
static struct kho_radix_tree preserved_pages_tree;

/**
 * mshv_register_preserve_page() - Register a page to be preserved by KHO
 * @pg: pointer to the page to preserve
 *
 * Registers a single page to be preserved by KHO across kexec.
 *
 * Return: 0 on success, -errno on failure.
 */
int mshv_register_preserve_page(struct page *pg)
{
	return kho_radix_add_key(&preserved_pages_tree, page_to_pfn(pg));
}
EXPORT_SYMBOL_GPL(mshv_register_preserve_page);

/**
 * mshv_unregister_preserve_page() - Unregister a page from KHO preservation
 * @pg: pointer to the page to unpreserve
 *
 * Unregisters a page that was previously registered to be preserved by KHO.
 *
 * Return: 0 on success, -errno on failure.
 */
int mshv_unregister_preserve_page(struct page *pg)
{
	return kho_radix_del_key(&preserved_pages_tree, page_to_pfn(pg));
}
EXPORT_SYMBOL_GPL(mshv_unregister_preserve_page);

/* Preserve a single page identified by its PFN key with KHO */
static int preserve_key_cb(unsigned long key, void *data)
{
	return kho_preserve_pages(pfn_to_page(key), 1);
}

/* Preserve a radix tree metadata page with KHO */
static int preserve_table_cb(phys_addr_t phys, void *data)
{
	return kho_preserve_pages(phys_to_page(phys), 1);
}

static int create_fdt(void)
{
	int err;
	void *fdt;
	phys_addr_t root_table;

	if (!fdt_page)
		return -EINVAL;

	fdt = fdt_page;

	err = fdt_create(fdt, PAGE_SIZE);
	if (err)
		return err;
	err = fdt_finish_reservemap(fdt);
	if (err)
		return err;
	err = fdt_begin_node(fdt, "");
	if (err)
		return err;
	err = fdt_property(fdt, "compatible", MSHV_KHO_COMPAT_STR,
			   strlen(MSHV_KHO_COMPAT_STR) + 1);
	if (err)
		return err;
	root_table = virt_to_phys(preserved_pages_tree.root);
	err = fdt_property(fdt, "root_table", &root_table, sizeof(root_table));
	if (err)
		return err;
	err = fdt_end_node(fdt);
	if (err)
		return err;
	err = fdt_finish(fdt);
	if (err)
		return err;

	return 0;
}

/**
 * preserve_tree() - Preserve pages owned by Microsoft Hypervisor
 *
 * This gets called prior to kexec and is our signal to finally preserve the
 * pages with KHO, and create & register the named FDT. We also need to freeze
 * the tree, since we cannot communicate any later changes.
 *
 * Return: 0 on success, -errno on error.
 */
static int preserve_tree(void)
{
	const struct kho_radix_walk_cb preserve_cb = {
		.key = preserve_key_cb,
		.table = preserve_table_cb,
	};
	int err;

	err = kho_radix_tree_freeze(&preserved_pages_tree);
	if (err) {
		pr_warn("%s() - kho_radix_tree_freeze() failed: %d\n",
			__func__, err);
		return err;
	}

	/* Populate the pre-allocated FDT page with current tree state */
	err = create_fdt();
	if (err) {
		pr_warn("%s() - create_fdt() failed: %d\n", __func__, err);
		return err;
	}

	/* Preserve both data- and meta-pages */
	err = kho_radix_walk_tree(&preserved_pages_tree, &preserve_cb, NULL);
	if (err) {
		/* We could not preserve all pages and cannot kexec. */
		pr_warn("%s() - kho_radix_walk_tree() failed: %d\n", __func__,
			err);
		return err;
	}

	err = kho_preserve_pages(virt_to_page(fdt_page), 1);
	if (err) {
		pr_warn("%s() - kho_preserve_pages(fdt) failed: %d\n", __func__,
			err);
		return err;
	}

	err = kho_add_subtree(FDT_SUBTREE_MSHV, fdt_page, PAGE_SIZE);
	if (err) {
		/* KHO will abort and undo all preservations. We cannot kexec. */
		pr_warn("%s() - kho_add_subtree() failed: %d\n", __func__, err);
		return err;
	}

	pr_debug("%s() - success\n", __func__);
	return 0;
}

/*
 * Reboot-callback triggering page preservation prior to kexec. Other reboots
 * need no KHO preservation.
 */
static int reboot_cb(struct notifier_block *nb, unsigned long action,
		     void *data)
{
	/* codes such as SYS_RESTART, SYS_HALT do not convey kexec specifically */
	if (kexec_in_progress) {
		int err;

		/* Finalize handover: write KHO descriptors, flush metadata */
		pr_debug("%s() - KHO-preserving page tree\n", __func__);
		err = preserve_tree();
		if (err)
			panic("preserve_tree() failed - must not kexec: %d\n",
			      err);
	}
	return NOTIFY_OK;
}

/**
 * restore_tree() - Restore the page-tree state from KHO.
 *
 * Return: 0 on success, -ENOENT if no KHO subtree was found (i.e. this is
 *         not a KHO boot), -EINVAL if the preserved FDT is malformed or
 *         incompatible.
 */
static int __init restore_tree(void)
{
	void *fdt;
	phys_addr_t fdt_pa;
	int len;
	int node;
	const phys_addr_t *root_table_fdt_ptr;
	int err;

	err = kho_retrieve_subtree(FDT_SUBTREE_MSHV, &fdt_pa, NULL);
	if (err)
		return err;

	fdt = phys_to_virt(fdt_pa);
	node = fdt_path_offset(fdt, "/");
	if (node < 0) {
		pr_err("Could not find root node in KHO-preserved FDT.\n");
		return -EINVAL;
	}

	if (fdt_node_check_compatible(fdt, node, MSHV_KHO_COMPAT_STR)) {
		/*
		 * This is unfortunate. We kexec'd into a kernel that isn't
		 * compatible with prior preservations. Pages this kernel
		 * considers available might actually be held by MSHV. The only
		 * recourse is to reboot.
		 */
		const char *s = fdt_getprop(fdt, node, "compatible", &len);

		if (s && len >= 0)
			pr_err("Incompatible kernel: Current is %s, preserved is %.*s\n",
			       MSHV_KHO_COMPAT_STR, len, s);
		else
			pr_err("Incompatible kernel: preserved misses 'compatible' mark.\n");
		return -EINVAL;
	}

	root_table_fdt_ptr = fdt_getprop(fdt, node, "root_table", &len);
	if (!root_table_fdt_ptr || len != sizeof(*root_table_fdt_ptr)) {
		pr_err("Could not obtain root_table property from KHO-preserved FDT.\n");
		return -EINVAL;
	}

	/* Restore struct page so it could be freed if needed */
	if (!kho_restore_pages(fdt_pa, 1))
		return -EINVAL;

	fdt_page = phys_to_virt(fdt_pa);

	err = kho_radix_init_tree(&preserved_pages_tree,
				  phys_to_virt(*root_table_fdt_ptr));
	if (err)
		return -EINVAL;

	pr_debug("Restored tracking from KHO.\n");
	return 0;
}

/*
 * Restore individual pages using KHO's helper during boot.
 *
 * Pages must be restored one at a time because they were deposited to
 * the hypervisor individually and will be withdrawn individually later.
 * Restoring them as a higher-order group would create compound pages
 * that cannot be freed with __free_page().
 */
static int __init restore_key_cb(unsigned long key, void *data)
{
	if (!kho_restore_pages(PFN_PHYS(key), 1))
		return -EINVAL;
	return 0;
}

static int __init restore_table_cb(phys_addr_t phys, void *data)
{
	if (!kho_restore_pages(phys, 1))
		return -EINVAL;
	return 0;
}

/**
 * restore_page_structs() - Restore page-structs so they can be __free_page()'d
 *
 * This is necessary because KHO-preserved pages are in a "weird" state
 * post-kexec. While doing so here in bulk adds to boot time, there is no vetted
 * alternative that would allow doing this later, when we cannot say which pages
 * had been freshly added, and which came into the tree through KHO.
 *
 * Return: 0 on success, -errno on failure.
 */
static int __init restore_page_structs(void)
{
	const struct kho_radix_walk_cb cb = {
		.key = restore_key_cb,
		.table = restore_table_cb,
	};

	return kho_radix_walk_tree(&preserved_pages_tree, &cb, NULL);
}

/**
 * alloc_tree() - Allocate a fresh page tree and FDT page.
 *
 * Called on fresh boot (no KHO data). Allocates an empty radix tree and
 * the FDT page used to serialize state before kexec.
 *
 * Return: 0 on success, -errno on failure.
 */
static int __init alloc_tree(void)
{
	int err;

	fdt_page = (void *)get_zeroed_page(GFP_KERNEL);
	if (!fdt_page)
		return -ENOMEM;

	err = kho_radix_init_tree(&preserved_pages_tree, NULL);
	if (err) {
		free_page((unsigned long)fdt_page);
		fdt_page = NULL;
		return err;
	}

	return 0;
}

static struct notifier_block reboot_notifier = {
	.notifier_call = reboot_cb,
	.priority = 0,
};

/**
 * mshv_preserve_init() - Initialize the page preservation
 *
 * Upon return:
 * - the tracker will be ready for use (restored post-kexec, or empty
 *   post-reboot),
 * - restored pages will be in a state that can be __free_page()'d,
 * - KHO notification for preservation will be registered.
 *
 * Return: 0 on success, -errno on error.
 */
int __init mshv_preserve_init(void)
{
	int err;

	if (!kho_is_enabled()) {
		pr_err("KHO is disabled; page deposits will fail.\n");
		return 0;
	}

	err = restore_tree();
	if (!err) {
		/* Restore struct pages so they can be __free_page()'d */
		if (restore_page_structs())
			/*
			 * Unrestored struct pages would BUG when freed
			 * at withdraw time.
			 */
			panic("Failed to restore MSHV page structs\n");
	} else if (err == -ENOENT) {
		pr_debug("Nothing to restore from KHO.\n");
		if (alloc_tree()) {
			pr_err("Could not allocate page tree; page deposits will fail.\n");
			return 0;
		}
	} else {
		/*
		 * Pages from the prior kernel are held by MSHV but we
		 * lost track of them -- memory corruption is inevitable.
		 */
		panic("Could not restore page tree from KHO: %d\n", err);
	}

	err = register_reboot_notifier(&reboot_notifier);
	if (err)
		/*
		 * Deposits would succeed but pages would not be preserved
		 * across kexec, causing memory corruption post-kexec.
		 */
		panic("Could not register reboot notification: %d\n", err);

	return 0;
}
