// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD SMN driver, used for register access
 * Copyright 2024 Advanced Micro Devices, Inc.
 */

#define pr_fmt(fmt) "amd_smn: " fmt

#include <linux/pci.h>
#include <linux/debugfs.h>

#include <asm/amd_smn.h>

static struct pci_dev **amd_roots;

/* Protect the PCI config register pairs used for SMN. */
static DEFINE_MUTEX(smn_mutex);
static bool smn_exclusive;

#define SMN_INDEX_OFFSET	0x60
#define SMN_DATA_OFFSET		0x64

#define HSMP_INDEX_OFFSET	0xc4
#define HSMP_DATA_OFFSET	0xc8

/*
 * SMN accesses may fail in ways that are difficult to detect here in the called
 * functions amd_smn_read() and amd_smn_write(). Therefore, callers must do
 * their own checking based on what behavior they expect.
 *
 * For SMN reads, the returned value may be zero if the register is Read-as-Zero.
 * Or it may be a "PCI Error Response", e.g. all 0xFFs. The "PCI Error Response"
 * can be checked here, and a proper error code can be returned.
 *
 * But the Read-as-Zero response cannot be verified here. A value of 0 may be
 * correct in some cases, so callers must check that this correct is for the
 * register/fields they need.
 *
 * For SMN writes, success can be determined through a "write and read back"
 * However, this is not robust when done here.
 *
 * Possible issues:
 *
 * 1) Bits that are "Write-1-to-Clear". In this case, the read value should
 *    *not* match the write value.
 *
 * 2) Bits that are "Read-as-Zero"/"Writes-Ignored". This information cannot be
 *    known here.
 *
 * 3) Bits that are "Reserved / Set to 1". Ditto above.
 *
 * Callers of amd_smn_write() should do the "write and read back" check
 * themselves, if needed.
 *
 * For #1, they can see if their target bits got cleared.
 *
 * For #2 and #3, they can check if their target bits got set as intended.
 *
 * This matches what is done for RDMSR/WRMSR. As long as there's no #GP, then
 * the operation is considered a success, and the caller does their own
 * checking.
 */
static int __amd_smn_rw(u8 i_off, u8 d_off, u16 node, u32 address, u32 *value, bool write)
{
	struct pci_dev *root;
	int err = -ENODEV;

	if (node >= amd_num_nodes())
		return err;

	root = amd_roots[node];
	if (!root)
		return err;

	guard(mutex)(&smn_mutex);

	if (!smn_exclusive)
		return err;

	err = pci_write_config_dword(root, i_off, address);
	if (err) {
		pr_warn("Error programming SMN address 0x%x.\n", address);
		return pcibios_err_to_errno(err);
	}

	err = (write ? pci_write_config_dword(root, d_off, *value)
		     : pci_read_config_dword(root, d_off, value));

	return pcibios_err_to_errno(err);
}

int __must_check amd_smn_read(u16 node, u32 address, u32 *value)
{
	int err = __amd_smn_rw(SMN_INDEX_OFFSET, SMN_DATA_OFFSET, node, address, value, false);

	if (PCI_POSSIBLE_ERROR(*value)) {
		err = -ENODEV;
		*value = 0;
	}

	return err;
}
EXPORT_SYMBOL_GPL(amd_smn_read);

int __must_check amd_smn_write(u16 node, u32 address, u32 value)
{
	return __amd_smn_rw(SMN_INDEX_OFFSET, SMN_DATA_OFFSET, node, address, &value, true);
}
EXPORT_SYMBOL_GPL(amd_smn_write);

int __must_check amd_smn_hsmp_rdwr(u16 node, u32 address, u32 *value, bool write)
{
	return __amd_smn_rw(HSMP_INDEX_OFFSET, HSMP_DATA_OFFSET, node, address, value, write);
}
EXPORT_SYMBOL_GPL(amd_smn_hsmp_rdwr);

static struct dentry *debugfs_dir;
static u16 debug_node;
static u32 debug_address;

static ssize_t smn_node_write(struct file *file, const char __user *userbuf,
			      size_t count, loff_t *ppos)
{
	int ret;

	ret = kstrtou16_from_user(userbuf, count, 0, &debug_node);
	if (ret)
		return ret;

	return count;
}

static int smn_node_show(struct seq_file *m, void *v)
{
	seq_printf(m, "0x%08x\n", debug_node);
	return 0;
}

static ssize_t smn_address_write(struct file *file, const char __user *userbuf,
				 size_t count, loff_t *ppos)
{
	int ret;

	ret = kstrtouint_from_user(userbuf, count, 0, &debug_address);
	if (ret)
		return ret;

	return count;
}

static int smn_address_show(struct seq_file *m, void *v)
{
	seq_printf(m, "0x%08x\n", debug_address);
	return 0;
}

static int smn_value_show(struct seq_file *m, void *v)
{
	u32 val;
	int ret;

	ret = amd_smn_read(debug_node, debug_address, &val);
	if (ret)
		return ret;

	seq_printf(m, "0x%08x\n", val);
	return 0;
}

static ssize_t smn_value_write(struct file *file, const char __user *userbuf,
			       size_t count, loff_t *ppos)
{
	u32 val;
	int ret;

	ret = kstrtouint_from_user(userbuf, count, 0, &val);
	if (ret)
		return ret;

	add_taint(TAINT_USER, LOCKDEP_STILL_OK);

	ret = amd_smn_write(debug_node, debug_address, val);
	if (ret)
		return ret;

	return count;
}

DEFINE_SHOW_STORE_ATTRIBUTE(smn_node);
DEFINE_SHOW_STORE_ATTRIBUTE(smn_address);
DEFINE_SHOW_STORE_ATTRIBUTE(smn_value);

static int amd_cache_roots(void)
{
	u16 node, num_nodes = amd_num_nodes();

	amd_roots = kcalloc(num_nodes, sizeof(struct pci_dev *), GFP_KERNEL);
	if (!amd_roots)
		return -ENOMEM;

	for (node = 0; node < num_nodes; node++)
		amd_roots[node] = amd_node_get_root(node);

	return 0;
}

static int reserve_root_config_spaces(void)
{
	struct pci_dev *root = NULL;
	struct pci_bus *bus = NULL;

	while ((bus = pci_find_next_bus(bus))) {
		/* Root device is Device 0 Function 0 on each Primary Bus. */
		root = pci_get_slot(bus, 0);
		if (!root)
			continue;

		if (root->vendor != PCI_VENDOR_ID_AMD &&
		    root->vendor != PCI_VENDOR_ID_HYGON)
			continue;

		pci_dbg(root, "Reserving PCI config space\n");

		/*
		 * There are a few SMN index/data pairs and other registers
		 * that shouldn't be accessed by user space.
		 * So reserve the entire PCI config space for simplicity rather
		 * than covering specific registers piecemeal.
		 */
		if (!pci_request_config_region_exclusive(root, 0, PCI_CFG_SPACE_SIZE, NULL)) {
			pci_err(root, "Failed to reserve config space\n");
			return -EEXIST;
		}
	}

	smn_exclusive = true;
	return 0;
}

static int __init amd_smn_init(void)
{
	int err;

	if (!cpu_feature_enabled(X86_FEATURE_ZEN))
		return 0;

	guard(mutex)(&smn_mutex);

	if (amd_roots)
		return 0;

	err = amd_cache_roots();
	if (err)
		return err;

	err = reserve_root_config_spaces();
	if (err)
		return err;

	debugfs_dir = debugfs_create_dir("amd_smn", arch_debugfs_dir);

	debugfs_create_file("node",	0600, debugfs_dir, NULL, &smn_node_fops);
	debugfs_create_file("address",	0600, debugfs_dir, NULL, &smn_address_fops);
	debugfs_create_file("value",	0600, debugfs_dir, NULL, &smn_value_fops);

	return 0;
}

fs_initcall(amd_smn_init);
