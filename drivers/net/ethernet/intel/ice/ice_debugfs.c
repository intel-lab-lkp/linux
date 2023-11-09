// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2022, Intel Corporation. */

#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/random.h>
#include <linux/vmalloc.h>
#include "ice.h"

static struct dentry *ice_debugfs_root;

/* create a define that has an extra module that doesn't really exist. this
 * is so we can add a module 'all' to easily enable/disable all the modules
 */
#define ICE_NR_FW_LOG_MODULES (ICE_AQC_FW_LOG_ID_MAX + 1)

/* the ordering in this array is important. it matches the ordering of the
 * values in the FW so the index is the same value as in ice_aqc_fw_logging_mod
 */
static const char * const ice_fwlog_module_string[] = {
	"general",
	"ctrl",
	"link",
	"link_topo",
	"dnl",
	"i2c",
	"sdp",
	"mdio",
	"adminq",
	"hdma",
	"lldp",
	"dcbx",
	"dcb",
	"xlr",
	"nvm",
	"auth",
	"vpd",
	"iosf",
	"parser",
	"sw",
	"scheduler",
	"txq",
	"rsvd",
	"post",
	"watchdog",
	"task_dispatch",
	"mng",
	"synce",
	"health",
	"tsdrv",
	"pfreg",
	"mdlver",
	"all",
};

/* the ordering in this array is important. it matches the ordering of the
 * values in the FW so the index is the same value as in ice_fwlog_level
 */
static const char * const ice_fwlog_level_string[] = {
	"none",
	"error",
	"warning",
	"normal",
	"verbose",
};

/**
 * ice_fwlog_print_module_cfg - print current FW logging module configuration
 * @hw: pointer to the HW structure
 * @module: module to print
 * @buff: pointer to a buffer to hold the config strings
 * @buff_size: size of the buffer in bytes
 */
static void ice_fwlog_print_module_cfg(struct ice_hw *hw, int module, char *buff,
				       int buff_size)
{
	struct ice_fwlog_cfg *cfg = &hw->fwlog_cfg;
	struct ice_fwlog_module_entry *entry;
	int len;

	if (module != ICE_AQC_FW_LOG_ID_MAX) {
		entry =	&cfg->module_entries[module];
		len = snprintf(buff, buff_size, "\tModule: %s, Log Level: %s\n",
			       ice_fwlog_module_string[entry->module_id],
			       ice_fwlog_level_string[entry->log_level]);
		buff = buff + len;
		buff_size -= len;
	} else {
		int i;

		for (i = 0; i < ICE_AQC_FW_LOG_ID_MAX; i++) {
			entry =	&cfg->module_entries[i];

			len = snprintf(buff, buff_size, "\tModule: %s, Log Level: %s\n",
				       ice_fwlog_module_string[entry->module_id],
				       ice_fwlog_level_string[entry->log_level]);
			buff = buff + len;
			buff_size -= len;
		}
	}
}

/**
 * ice_debugfs_parse_cmd_line - Parse the command line that was passed in
 * @src: pointer to a buffer holding the command line
 * @len: size of the buffer in bytes
 * @argv: pointer to store the command line items
 * @argc: pointer to store the number of command line items
 */
static ssize_t ice_debugfs_parse_cmd_line(const char __user *src, size_t len,
					  char ***argv, int *argc)
{
	char *cmd_buf, *cmd_buf_tmp;

	cmd_buf = memdup_user(src, len);
	if (IS_ERR(cmd_buf))
		return PTR_ERR(cmd_buf);
	cmd_buf[len] = '\0';

	/* the cmd_buf has a newline at the end of the command so
	 * remove it
	 */
	cmd_buf_tmp = strchr(cmd_buf, '\n');
	if (cmd_buf_tmp) {
		*cmd_buf_tmp = '\0';
		len = (size_t)cmd_buf_tmp - (size_t)cmd_buf;
	}

	*argv = argv_split(GFP_KERNEL, cmd_buf, argc);
	kfree(cmd_buf);
	if (!*argv)
		return -ENOMEM;

	return 0;
}

static int ice_find_module_by_dentry(struct ice_pf *pf, struct dentry *d)
{
	int i, module;

	module = -1;
	/* find the module based on the dentry */
	for (i = 0; i < ICE_NR_FW_LOG_MODULES; i++) {
		if (d == pf->ice_debugfs_pf_fwlog_modules[i]) {
			module = i;
			break;
		}
	}

	return module;
}

/**
 * ice_debugfs_module_read - read from 'module' file
 * @filp: the opened file
 * @buffer: where to write the data for the user to read
 * @count: the size of the user's buffer
 * @ppos: file position offset
 */
static ssize_t ice_debugfs_module_read(struct file *filp, char __user *buffer,
				       size_t count, loff_t *ppos)
{
	struct dentry *dentry = filp->f_path.dentry;
	struct ice_pf *pf = filp->private_data;
	int status, module;
	char *data = NULL;

	/* don't allow commands if the FW doesn't support it */
	if (!ice_fwlog_supported(&pf->hw))
		return -EOPNOTSUPP;

	module = ice_find_module_by_dentry(pf, dentry);
	if (module < 0) {
		dev_info(ice_pf_to_dev(pf), "unknown module\n");
		return -EINVAL;
	}

	data = vzalloc(ICE_AQ_MAX_BUF_LEN);
	if (!data) {
		dev_warn(ice_pf_to_dev(pf), "Unable to allocate memory for FW configuration!\n");
		return -ENOMEM;
	}

	ice_fwlog_print_module_cfg(&pf->hw, module, data, ICE_AQ_MAX_BUF_LEN);

	if (count < strlen(data))
		return -ENOSPC;

	status = simple_read_from_buffer(buffer, count, ppos, data,
					 strlen(data));
	vfree(data);

	return status;
}

/**
 * ice_debugfs_module_write - write into 'module' file
 * @filp: the opened file
 * @buf: where to find the user's data
 * @count: the length of the user's data
 * @ppos: file position offset
 */
static ssize_t
ice_debugfs_module_write(struct file *filp, const char __user *buf,
			 size_t count, loff_t *ppos)
{
	struct dentry *dentry = filp->f_path.dentry;
	struct ice_pf *pf = filp->private_data;
	struct device *dev = ice_pf_to_dev(pf);
	ssize_t ret;
	char **argv;
	int argc;

	/* don't allow commands if the FW doesn't support it */
	if (!ice_fwlog_supported(&pf->hw))
		return -EOPNOTSUPP;

	/* don't allow partial writes */
	if (*ppos != 0)
		return 0;

	ret = ice_debugfs_parse_cmd_line(buf, count, &argv, &argc);
	if (ret)
		goto err_copy_from_user;

	if (argc == 1) {
		int module, log_level;

		module = ice_find_module_by_dentry(pf, dentry);
		if (module < 0) {
			dev_info(dev, "unknown module\n");
			ret = -EINVAL;
			goto module_write_error;
		}

		log_level = sysfs_match_string(ice_fwlog_level_string, argv[0]);
		if (log_level < 0) {
			dev_info(dev, "unknown log level '%s'\n", argv[0]);
			ret = -EINVAL;
			goto module_write_error;
		}

		if (module != ICE_AQC_FW_LOG_ID_MAX) {
			ice_pf_fwlog_update_module(pf, log_level, module);
		} else {
			/* the module 'all' is a shortcut so that we can set
			 * all of the modules to the same level quickly
			 */
			int i;

			for (i = 0; i < ICE_AQC_FW_LOG_ID_MAX; i++)
				ice_pf_fwlog_update_module(pf, log_level, i);
		}
	} else {
		dev_info(dev, "unknown or invalid command '%s'\n", argv[0]);
		ret = -EINVAL;
		goto module_write_error;
	}

	/* if we get here, nothing went wrong; return count since we didn't
	 * really write anything
	 */
	ret = (ssize_t)count;

module_write_error:
	argv_free(argv);
err_copy_from_user:
	/* This function always consumes all of the written input, or produces
	 * an error. Check and enforce this. Otherwise, the write operation
	 * won't complete properly.
	 */
	if (WARN_ON(ret != (ssize_t)count && ret >= 0))
		ret = -EIO;

	return ret;
}

static const struct file_operations ice_debugfs_module_fops = {
	.owner = THIS_MODULE,
	.open  = simple_open,
	.read = ice_debugfs_module_read,
	.write = ice_debugfs_module_write,
};

/**
 * ice_debugfs_nr_messages_read - read from 'nr_messages' file
 * @filp: the opened file
 * @buffer: where to write the data for the user to read
 * @count: the size of the user's buffer
 * @ppos: file position offset
 */
static ssize_t ice_debugfs_nr_messages_read(struct file *filp,
					    char __user *buffer, size_t count,
					    loff_t *ppos)
{
	struct ice_pf *pf = filp->private_data;
	struct ice_hw *hw = &pf->hw;
	char buff[32] = {};
	int status;

	/* don't allow commands if the FW doesn't support it */
	if (!ice_fwlog_supported(&pf->hw))
		return -EOPNOTSUPP;

	snprintf(buff, sizeof(buff), "%d\n",
		 hw->fwlog_cfg.log_resolution);

	status = simple_read_from_buffer(buffer, count, ppos, buff,
					 strlen(buff));

	return status;
}

/**
 * ice_debugfs_nr_messages_write - write into 'nr_messages' file
 * @filp: the opened file
 * @buf: where to find the user's data
 * @count: the length of the user's data
 * @ppos: file position offset
 */
static ssize_t
ice_debugfs_nr_messages_write(struct file *filp, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct ice_pf *pf = filp->private_data;
	struct device *dev = ice_pf_to_dev(pf);
	struct ice_hw *hw = &pf->hw;
	ssize_t ret;
	char **argv;
	int argc;

	/* don't allow commands if the FW doesn't support it */
	if (!ice_fwlog_supported(hw))
		return -EOPNOTSUPP;

	/* don't allow partial writes */
	if (*ppos != 0)
		return 0;

	ret = ice_debugfs_parse_cmd_line(buf, count, &argv, &argc);
	if (ret)
		goto err_copy_from_user;

	if (argc == 1) {
		s16 nr_messages;

		ret = kstrtos16(argv[0], 0, &nr_messages);
		if (ret)
			goto nr_messages_write_error;

		if (nr_messages < ICE_AQC_FW_LOG_MIN_RESOLUTION ||
		    nr_messages > ICE_AQC_FW_LOG_MAX_RESOLUTION) {
			dev_err(dev, "Invalid FW log number of messages %d, value must be between %d - %d\n",
				nr_messages, ICE_AQC_FW_LOG_MIN_RESOLUTION,
				ICE_AQC_FW_LOG_MAX_RESOLUTION);
			ret = -EINVAL;
			goto nr_messages_write_error;
		}

		hw->fwlog_cfg.log_resolution = nr_messages;
	} else {
		dev_info(dev, "unknown or invalid command '%s'\n", argv[0]);
		ret = -EINVAL;
		goto nr_messages_write_error;
	}

	/* if we get here, nothing went wrong; return count since we didn't
	 * really write anything
	 */
	ret = (ssize_t)count;

nr_messages_write_error:
	argv_free(argv);
err_copy_from_user:
	/* This function always consumes all of the written input, or produces
	 * an error. Check and enforce this. Otherwise, the write operation
	 * won't complete properly.
	 */
	if (WARN_ON(ret != (ssize_t)count && ret >= 0))
		ret = -EIO;

	return ret;
}

static const struct file_operations ice_debugfs_nr_messages_fops = {
	.owner = THIS_MODULE,
	.open  = simple_open,
	.read = ice_debugfs_nr_messages_read,
	.write = ice_debugfs_nr_messages_write,
};

/**
 * ice_debugfs_fwlog_init - setup the debugfs directory
 * @pf: the ice that is starting up
 */
void ice_debugfs_fwlog_init(struct ice_pf *pf)
{
	const char *name = pci_name(pf->pdev);
	struct dentry *fw_modules_dir;
	struct dentry **fw_modules;
	int i;

	/* only support fw log commands on PF 0 */
	if (pf->hw.bus.func)
		return;

	/* allocate space for this first because if it fails then we don't
	 * need to unwind
	 */
	fw_modules = kcalloc(ICE_NR_FW_LOG_MODULES, sizeof(*fw_modules),
			     GFP_KERNEL);

	if (!fw_modules) {
		pr_info("Unable to allocate space for modules\n");
		return;
	}

	pf->ice_debugfs_pf = debugfs_create_dir(name, ice_debugfs_root);
	if (IS_ERR(pf->ice_debugfs_pf)) {
		pr_info("init of debugfs PCI dir failed\n");
		kfree(fw_modules);
		return;
	}

	pf->ice_debugfs_pf_fwlog = debugfs_create_dir("fwlog",
						      pf->ice_debugfs_pf);
	if (IS_ERR(pf->ice_debugfs_pf)) {
		pr_info("init of debugfs fwlog dir failed\n");
		return;
	}

	fw_modules_dir = debugfs_create_dir("modules",
					    pf->ice_debugfs_pf_fwlog);
	if (IS_ERR(fw_modules_dir)) {
		pr_info("Unable to create modules dir\n");
		kfree(fw_modules);
		return;
	}

	for (i = 0; i < ICE_NR_FW_LOG_MODULES; i++) {
		fw_modules[i] = debugfs_create_file(ice_fwlog_module_string[i],
						    0600, fw_modules_dir, pf,
						    &ice_debugfs_module_fops);

		if (IS_ERR(fw_modules[i])) {
			pr_info("Error creating module %s\n",
				ice_fwlog_module_string[i]);
			goto err_create_module_files;
		}
	}

	debugfs_create_file("nr_messages", 0600,
			    pf->ice_debugfs_pf_fwlog, pf,
			    &ice_debugfs_nr_messages_fops);

	pf->ice_debugfs_pf_fwlog_modules = fw_modules;

	return;

err_create_module_files:
	debugfs_remove_recursive(pf->ice_debugfs_pf_fwlog);
	kfree(fw_modules);
}

/**
 * ice_debugfs_init - create root directory for debugfs entries
 */
void ice_debugfs_init(void)
{
	ice_debugfs_root = debugfs_create_dir(KBUILD_MODNAME, NULL);
	if (IS_ERR(ice_debugfs_root))
		pr_info("init of debugfs failed\n");
}

/**
 * ice_debugfs_exit - remove debugfs entries
 */
void ice_debugfs_exit(void)
{
	debugfs_remove_recursive(ice_debugfs_root);
	ice_debugfs_root = NULL;
}
