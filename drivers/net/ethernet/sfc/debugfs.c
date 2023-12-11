// SPDX-License-Identifier: GPL-2.0-only
/****************************************************************************
 * Driver for Solarflare network controllers and boards
 * Copyright 2023, Advanced Micro Devices, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, incorporated herein by reference.
 */

#include "debugfs.h"
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/dcache.h>
#include <linux/seq_file.h>

/* Maximum length for a name component or symlink target */
#define EFX_DEBUGFS_NAME_LEN 32

/* Top-level debug directory ([/sys/kernel]/debug/sfc) */
static struct dentry *efx_debug_root;

/* "cards" directory ([/sys/kernel]/debug/sfc/cards) */
static struct dentry *efx_debug_cards;

/**
 * efx_init_debugfs_netdev - create debugfs sym-link for net device
 * @net_dev:		Net device
 *
 * Create sym-link named after @net_dev to the debugfs directories for the
 * corresponding NIC.  The sym-link must be cleaned up using
 * efx_fini_debugfs_netdev().
 *
 * Return: a negative error code or 0 on success.
 */
static int efx_init_debugfs_netdev(struct net_device *net_dev)
{
	struct efx_nic *efx = efx_netdev_priv(net_dev);
	char target[EFX_DEBUGFS_NAME_LEN];
	char name[EFX_DEBUGFS_NAME_LEN];

	if (snprintf(name, sizeof(name), "nic_%s", net_dev->name) >=
			sizeof(name))
		return -ENAMETOOLONG;
	if (snprintf(target, sizeof(target), "cards/%s", pci_name(efx->pci_dev))
	    >= sizeof(target))
		return -ENAMETOOLONG;
	efx->debug_symlink = debugfs_create_symlink(name,
						    efx_debug_root, target);
	if (IS_ERR_OR_NULL(efx->debug_symlink))
		return efx->debug_symlink ? PTR_ERR(efx->debug_symlink) :
					    -ENOMEM;

	return 0;
}

/**
 * efx_fini_debugfs_netdev - remove debugfs sym-link for net device
 * @net_dev:		Net device
 *
 * Remove sym-link created for @net_dev by efx_init_debugfs_netdev().
 */
void efx_fini_debugfs_netdev(struct net_device *net_dev)
{
	struct efx_nic *efx = efx_netdev_priv(net_dev);

	debugfs_remove(efx->debug_symlink);
	efx->debug_symlink = NULL;
}

/* replace debugfs sym-links on net device rename */
void efx_update_debugfs_netdev(struct efx_nic *efx)
{
	mutex_lock(&efx->debugfs_symlink_mutex);
	if (efx->debug_symlink)
		efx_fini_debugfs_netdev(efx->net_dev);
	efx_init_debugfs_netdev(efx->net_dev);
	mutex_unlock(&efx->debugfs_symlink_mutex);
}

static int efx_debugfs_enum_read(struct seq_file *s, void *d)
{
	struct efx_debugfs_enum_data *data = s->private;
	u64 value = 0;
	size_t len;

	len = min(data->vlen, sizeof(value));
#ifdef BIG_ENDIAN
	/* If data->value is narrower than u64, we need to copy into the
	 * far end of value, as that's where the low bits live.
	 */
	memcpy(((void *)&value) + sizeof(value) - len, data->value, len);
#else
	memcpy(&value, data->value, len);
#endif
	seq_printf(s, "%#llx => %s\n", value,
		   value < data->max ? data->names[value] : "(invalid)");
	return 0;
}

static int efx_debugfs_enum_open(struct inode *inode, struct file *f)
{
	struct efx_debugfs_enum_data *data = inode->i_private;

	return single_open(f, efx_debugfs_enum_read, data);
}

static const struct file_operations efx_debugfs_enum_ops = {
	.owner = THIS_MODULE,
	.open = efx_debugfs_enum_open,
	.release = single_release,
	.read = seq_read,
	.llseek = seq_lseek,
};

static void efx_debugfs_create_enum(const char *name, umode_t mode,
				    struct dentry *parent,
				    struct efx_debugfs_enum_data *data)
{
	debugfs_create_file(name, mode, parent, data, &efx_debugfs_enum_ops);
}

static const char *const efx_interrupt_mode_names[] = {
	[EFX_INT_MODE_MSIX]   = "MSI-X",
	[EFX_INT_MODE_MSI]    = "MSI",
	[EFX_INT_MODE_LEGACY] = "legacy",
};

#define EFX_DEBUGFS_EFX(_type, _name)	\
	debugfs_create_##_type(#_name, 0444, efx->debug_dir, &efx->_name)

/* Create basic debugfs parameter files for an Efx NIC */
static void efx_init_debugfs_nic_files(struct efx_nic *efx)
{
	EFX_DEBUGFS_EFX(x32, rx_dma_len);
	EFX_DEBUGFS_EFX(x32, rx_buffer_order);
	EFX_DEBUGFS_EFX(x32, rx_buffer_truesize);
	efx->debug_interrupt_mode.max = ARRAY_SIZE(efx_interrupt_mode_names);
	efx->debug_interrupt_mode.names = efx_interrupt_mode_names;
	efx->debug_interrupt_mode.vlen = sizeof(efx->interrupt_mode);
	efx->debug_interrupt_mode.value = &efx->interrupt_mode;
	efx_debugfs_create_enum("interrupt_mode", 0444, efx->debug_dir,
				&efx->debug_interrupt_mode);
}

/**
 * efx_init_debugfs_nic - create debugfs directory for NIC
 * @efx:		Efx NIC
 *
 * Create debugfs directory containing parameter-files for @efx.
 * The directory must be cleaned up using efx_fini_debugfs_nic().
 *
 * Return: a negative error code or 0 on success.
 */
int efx_init_debugfs_nic(struct efx_nic *efx)
{
	/* Create directory */
	efx->debug_dir = debugfs_create_dir(pci_name(efx->pci_dev),
					    efx_debug_cards);
	if (!efx->debug_dir)
		return -ENOMEM;
	efx_init_debugfs_nic_files(efx);
	return 0;
}

/**
 * efx_fini_debugfs_nic - remove debugfs directory for NIC
 * @efx:		Efx NIC
 *
 * Remove debugfs directory created for @efx by efx_init_debugfs_nic().
 */
void efx_fini_debugfs_nic(struct efx_nic *efx)
{
	debugfs_remove_recursive(efx->debug_dir);
	efx->debug_dir = NULL;
}

/**
 * efx_init_debugfs - create debugfs directories for sfc driver
 *
 * Create debugfs directories "sfc" and "sfc/cards".  This must be
 * called before any of the other functions that create debugfs
 * directories.  The directories must be cleaned up using
 * efx_fini_debugfs().
 *
 * Return: a negative error code or 0 on success.
 */
int efx_init_debugfs(void)
{
	int rc;

	/* Create top-level directory */
	efx_debug_root = debugfs_create_dir(KBUILD_MODNAME, NULL);
	if (!efx_debug_root) {
		pr_err("debugfs_create_dir %s failed.\n", KBUILD_MODNAME);
		rc = -ENOMEM;
		goto err;
	} else if (IS_ERR(efx_debug_root)) {
		rc = PTR_ERR(efx_debug_root);
		pr_err("debugfs_create_dir %s failed, rc=%d.\n",
		       KBUILD_MODNAME, rc);
		goto err;
	}

	/* Create "cards" directory */
	efx_debug_cards = debugfs_create_dir("cards", efx_debug_root);
	if (!efx_debug_cards) {
		pr_err("debugfs_create_dir cards failed.\n");
		rc = -ENOMEM;
		goto err;
	} else if (IS_ERR(efx_debug_cards)) {
		rc = PTR_ERR(efx_debug_cards);
		pr_err("debugfs_create_dir cards failed, rc=%d.\n", rc);
		goto err;
	}

	return 0;

err:
	efx_fini_debugfs();
	return rc;
}

/**
 * efx_fini_debugfs - remove debugfs directories for sfc driver
 *
 * Remove directories created by efx_init_debugfs().
 */
void efx_fini_debugfs(void)
{
	debugfs_remove_recursive(efx_debug_root);
	efx_debug_cards = NULL;
	efx_debug_root = NULL;
}
