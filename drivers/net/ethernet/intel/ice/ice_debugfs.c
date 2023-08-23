// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2022, Intel Corporation. */

#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/random.h>
#include <linux/vmalloc.h>
#include "ice.h"

static struct dentry *ice_debugfs_root;

/* the ordering in this array is important. it matches the ordering of the
 * values in the FW so the index is the same value as in ice_aqc_fw_logging_mod
 */
static const char * const ice_fwlog_module_string[] = {
	"GENERAL",
	"CTRL",
	"LINK",
	"LINK_TOPO",
	"DNL",
	"I2C",
	"SDP",
	"MDIO",
	"ADMINQ",
	"HDMA",
	"LLDP",
	"DCBX",
	"DCB",
	"XLR",
	"NVM",
	"AUTH",
	"VPD",
	"IOSF",
	"PARSER",
	"SW",
	"SCHEDULER",
	"TXQ",
	"RSVD",
	"POST",
	"WATCHDOG",
	"TASK_DISPATCH",
	"MNG",
	"SYNCE",
	"HEALTH",
	"TSDRV",
	"PFREG",
	"MDLVER",
	"ALL",
};

/* the ordering in this array is important. it matches the ordering of the
 * values in the FW so the index is the same value as in ice_fwlog_level
 */
static const char * const ice_fwlog_level_string[] = {
	"NONE",
	"ERROR",
	"WARNING",
	"NORMAL",
	"VERBOSE",
};

static void ice_print_fwlog_config(struct ice_hw *hw, struct ice_fwlog_cfg *cfg,
				   char **buff, int *size)
{
	char *tmp = *buff;
	int used = *size;
	u16 i, len;

	len = snprintf(tmp, used, "Log_resolution: %d\n", cfg->log_resolution);
	tmp = tmp + len;
	used -= len;
	len = snprintf(tmp, used, "Options: 0x%04x\n", cfg->options);
	tmp = tmp + len;
	used -= len;
	len = snprintf(tmp, used, "\tarq_ena: %s\n",
		       (cfg->options &
		       ICE_FWLOG_OPTION_ARQ_ENA) ? "true" : "false");
	tmp = tmp + len;
	used -= len;
	len = snprintf(tmp, used, "\tuart_ena: %s\n",
		       (cfg->options &
		       ICE_FWLOG_OPTION_UART_ENA) ? "true" : "false");
	tmp = tmp + len;
	used -= len;
	len = snprintf(tmp, used, "\trunning: %s\n",
		       (cfg->options &
		       ICE_FWLOG_OPTION_IS_REGISTERED) ? "true" : "false");
	tmp = tmp + len;
	used -= len;
	len = snprintf(tmp, used, "Module Entries:\n");
	tmp = tmp + len;
	used -= len;

	for (i = 0; i < ICE_AQC_FW_LOG_ID_MAX; i++) {
		struct ice_fwlog_module_entry *entry =
			&cfg->module_entries[i];

		len = snprintf(tmp, used, "\tModule: %s, Log Level: %s\n",
			       ice_fwlog_module_string[entry->module_id],
			       ice_fwlog_level_string[entry->log_level]);
		tmp = tmp + len;
		used -= len;
	}

	len = snprintf(tmp, used, "Valid log levels:\n");
	tmp = tmp + len;
	used -= len;

	for (i = 0; i < ICE_FWLOG_LEVEL_INVALID; i++) {
		len = snprintf(tmp, used, "\t%s\n", ice_fwlog_level_string[i]);
		tmp = tmp + len;
		used -= len;
	}

	*buff = tmp;
	*size = used;
}

/**
 * ice_fwlog_dump_cfg - Dump current FW logging configuration
 * @hw: pointer to the HW structure
 * @buff: pointer to a buffer to hold the config strings
 * @buff_size: size of the buffer in bytes
 */
static void ice_fwlog_dump_cfg(struct ice_hw *hw, char *buff, int buff_size)
{
	int len;

	len = snprintf(buff, buff_size, "FWLOG Configuration:\n");
	buff = buff + len;
	buff_size -= len;

	ice_print_fwlog_config(hw, &hw->fwlog_cfg, &buff, &buff_size);
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

	cmd_buf = memdup_user(src, len + 1);
	if (IS_ERR(cmd_buf))
		return PTR_ERR(cmd_buf);
	cmd_buf[len] = '\0';

	/* the cmd_buf has a newline at the end of the command so
	 * remove it
	 */
	cmd_buf_tmp = strchr(cmd_buf, '\n');
	if (cmd_buf_tmp) {
		*cmd_buf_tmp = '\0';
		len = (size_t)cmd_buf_tmp - (size_t)cmd_buf + 1;
	}

	*argv = argv_split(GFP_KERNEL, cmd_buf, argc);
	if (!*argv)
		return -ENOMEM;

	kfree(cmd_buf);
	return 0;
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
	struct ice_pf *pf = filp->private_data;
	char *data = NULL;
	int status;

	/* don't allow commands if the FW doesn't support it */
	if (!ice_fwlog_supported(&pf->hw))
		return -EOPNOTSUPP;

	data = vzalloc(ICE_AQ_MAX_BUF_LEN);
	if (!data) {
		dev_warn(ice_pf_to_dev(pf), "Unable to allocate memory for FW configuration!\n");
		return -ENOMEM;
	}

	ice_fwlog_dump_cfg(&pf->hw, data, ICE_AQ_MAX_BUF_LEN);

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

	if (argc == 2) {
		int module, log_level;

		module = sysfs_match_string(ice_fwlog_module_string, argv[0]);
		if (module < 0) {
			dev_info(dev, "unknown module '%s'\n", argv[0]);
			ret = -EINVAL;
			goto module_write_error;
		}

		log_level = sysfs_match_string(ice_fwlog_level_string, argv[1]);
		if (log_level < 0) {
			dev_info(dev, "unknown log level '%s'\n", argv[1]);
			ret = -EINVAL;
			goto module_write_error;
		}

		/* module is valid because it was range checked using
		 * sysfs_match_string()
		 */
		if (module != ICE_AQC_FW_LOG_ID_MAX) {
			ice_pf_fwlog_update_module(pf, log_level, module);
		} else {
			/* the module 'ALL' is a shortcut so that we can set
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

	/* if we get here, nothing went wrong; return bytes copied */
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
 * ice_debugfs_resolution_read - read from 'resolution' file
 * @filp: the opened file
 * @buffer: where to write the data for the user to read
 * @count: the size of the user's buffer
 * @ppos: file position offset
 */
static ssize_t ice_debugfs_resolution_read(struct file *filp,
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
 * ice_debugfs_resolution_write - write into 'resolution' file
 * @filp: the opened file
 * @buf: where to find the user's data
 * @count: the length of the user's data
 * @ppos: file position offset
 */
static ssize_t
ice_debugfs_resolution_write(struct file *filp, const char __user *buf,
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
		s16 resolution;

		ret = kstrtos16(argv[0], 0, &resolution);
		if (ret)
			goto resolution_write_error;

		if (resolution < ICE_AQC_FW_LOG_MIN_RESOLUTION ||
		    resolution > ICE_AQC_FW_LOG_MAX_RESOLUTION) {
			dev_err(dev, "Invalid FW log resolution %d, value must be between %d - %d\n",
				resolution, ICE_AQC_FW_LOG_MIN_RESOLUTION,
				ICE_AQC_FW_LOG_MAX_RESOLUTION);
			ret = -EINVAL;
			goto resolution_write_error;
		}

		hw->fwlog_cfg.log_resolution = resolution;
	} else {
		dev_info(dev, "unknown or invalid command '%s'\n", argv[0]);
		ret = -EINVAL;
		goto resolution_write_error;
	}

	/* if we get here, nothing went wrong; return bytes copied */
	ret = (ssize_t)count;

resolution_write_error:
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

static const struct file_operations ice_debugfs_resolution_fops = {
	.owner = THIS_MODULE,
	.open  = simple_open,
	.read = ice_debugfs_resolution_read,
	.write = ice_debugfs_resolution_write,
};

/**
 * ice_debugfs_enable_read - read from 'enable' file
 * @filp: the opened file
 * @buffer: where to write the data for the user to read
 * @count: the size of the user's buffer
 * @ppos: file position offset
 */
static ssize_t ice_debugfs_enable_read(struct file *filp,
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

	snprintf(buff, sizeof(buff), "%u\n",
		 (u16)(hw->fwlog_cfg.options &
		 ICE_FWLOG_OPTION_IS_REGISTERED) >> 3);

	status = simple_read_from_buffer(buffer, count, ppos, buff,
					 strlen(buff));

	return status;
}

/**
 * ice_debugfs_enable_write - write into 'enable' file
 * @filp: the opened file
 * @buf: where to find the user's data
 * @count: the length of the user's data
 * @ppos: file position offset
 */
static ssize_t
ice_debugfs_enable_write(struct file *filp, const char __user *buf,
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
		bool enable;

		ret = kstrtobool(argv[0], &enable);
		if (ret)
			goto enable_write_error;

		if (enable)
			hw->fwlog_cfg.options |= ICE_FWLOG_OPTION_ARQ_ENA;
		else
			hw->fwlog_cfg.options &= ~ICE_FWLOG_OPTION_ARQ_ENA;

		ret = ice_fwlog_set(hw, &hw->fwlog_cfg);
		if (ret)
			goto enable_write_error;

		if (enable)
			ret = ice_fwlog_register(hw);
		else
			ret = ice_fwlog_unregister(hw);

		if (ret)
			goto enable_write_error;
	} else {
		dev_info(dev, "unknown or invalid command '%s'\n", argv[0]);
		ret = -EINVAL;
		goto enable_write_error;
	}

	/* if we get here, nothing went wrong; return bytes copied */
	ret = (ssize_t)count;

enable_write_error:
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

static const struct file_operations ice_debugfs_enable_fops = {
	.owner = THIS_MODULE,
	.open  = simple_open,
	.read = ice_debugfs_enable_read,
	.write = ice_debugfs_enable_write,
};

/**
 * ice_debugfs_nr_buffs_read - read from 'nr_buffs' file
 * @filp: the opened file
 * @buffer: where to write the data for the user to read
 * @count: the size of the user's buffer
 * @ppos: file position offset
 */
static ssize_t ice_debugfs_nr_buffs_read(struct file *filp,
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

	snprintf(buff, sizeof(buff), "%d\n", hw->fwlog_ring.size);

	status = simple_read_from_buffer(buffer, count, ppos, buff,
					 strlen(buff));

	return status;
}

/**
 * ice_debugfs_nr_buffs_write - write into 'nr_buffs' file
 * @filp: the opened file
 * @buf: where to find the user's data
 * @count: the length of the user's data
 * @ppos: file position offset
 */
static ssize_t
ice_debugfs_nr_buffs_write(struct file *filp, const char __user *buf,
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
		s16 nr_buffs;

		ret = kstrtos16(argv[0], 0, &nr_buffs);
		if (ret)
			goto nr_buffs_write_error;

		if (nr_buffs <= 0 || nr_buffs > ICE_FWLOG_RING_SIZE_MAX) {
			dev_info(dev, "nr_buffs '%d' is not within bounds. Please use a value between 1 and %d\n",
				 nr_buffs, ICE_FWLOG_RING_SIZE_MAX);
			ret = -EINVAL;
			goto nr_buffs_write_error;
		} else if (hweight16(nr_buffs) > 1) {
			dev_info(dev, "nr_buffs '%d' is not a power of 2. Please use a value that is a power of 2.\n",
				 nr_buffs);
			ret = -EINVAL;
			goto nr_buffs_write_error;
		} else if (hw->fwlog_cfg.options &
			   ICE_FWLOG_OPTION_IS_REGISTERED) {
			dev_info(dev, "FW logging is currently running. Please disable FW logging to change nr_buffs\n");
			ret = -EINVAL;
			goto nr_buffs_write_error;
		}

		/* free all the buffers and the tracking info and resize */
		ice_fwlog_realloc_rings(hw, nr_buffs);
	} else {
		dev_info(dev, "unknown or invalid command '%s'\n", argv[0]);
		ret = -EINVAL;
		goto nr_buffs_write_error;
	}

	/* if we get here, nothing went wrong; return bytes copied */
	ret = (ssize_t)count;

nr_buffs_write_error:
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

static const struct file_operations ice_debugfs_nr_buffs_fops = {
	.owner = THIS_MODULE,
	.open  = simple_open,
	.read = ice_debugfs_nr_buffs_read,
	.write = ice_debugfs_nr_buffs_write,
};

/**
 * ice_debugfs_data_read - read from 'data' file
 * @filp: the opened file
 * @buffer: where to write the data for the user to read
 * @count: the size of the user's buffer
 * @ppos: file position offset
 */
static ssize_t ice_debugfs_data_read(struct file *filp, char __user *buffer,
				     size_t count, loff_t *ppos)
{
	struct ice_pf *pf = filp->private_data;
	struct ice_hw *hw = &pf->hw;
	int data_copied = 0;
	bool done = false;

	/* don't allow commands if the FW doesn't support it */
	if (!ice_fwlog_supported(&pf->hw))
		return -EOPNOTSUPP;

	if (ice_fwlog_ring_empty(&hw->fwlog_ring))
		return 0;

	while (!ice_fwlog_ring_empty(&hw->fwlog_ring) && !done) {
		struct ice_fwlog_data *log;
		u16 cur_buf_len;

		log = &hw->fwlog_ring.rings[hw->fwlog_ring.head];
		cur_buf_len = log->data_size;

		if (cur_buf_len >= count) {
			done = true;
			continue;
		}

		if (copy_to_user(buffer, log->data, cur_buf_len)) {
			/* if there is an error then bail and return whatever
			 * the driver has copied so far
			 */
			done = true;
			continue;
		}

		data_copied += cur_buf_len;
		buffer += cur_buf_len;
		count -= cur_buf_len;
		*ppos += cur_buf_len;
		ice_fwlog_ring_increment(&hw->fwlog_ring.head,
					 hw->fwlog_ring.size);
	}

	return data_copied;
}

/**
 * ice_debugfs_data_write - write into 'data' file
 * @filp: the opened file
 * @buf: where to find the user's data
 * @count: the length of the user's data
 * @ppos: file position offset
 */
static ssize_t
ice_debugfs_data_write(struct file *filp, const char __user *buf, size_t count,
		       loff_t *ppos)
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
		if (!(hw->fwlog_cfg.options & ICE_FWLOG_OPTION_IS_REGISTERED)) {
			hw->fwlog_ring.head = 0;
			hw->fwlog_ring.tail = 0;
		} else {
			dev_info(dev, "Can't clear FW log data while FW log running\n");
			ret = -EINVAL;
			goto nr_buffs_write_error;
		}
	} else {
		dev_info(dev, "unknown or invalid command '%s'\n", argv[0]);
		ret = -EINVAL;
		goto nr_buffs_write_error;
	}

	/* if we get here, nothing went wrong; return bytes copied */
	ret = (ssize_t)count;

nr_buffs_write_error:
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

static const struct file_operations ice_debugfs_data_fops = {
	.owner = THIS_MODULE,
	.open  = simple_open,
	.read = ice_debugfs_data_read,
	.write = ice_debugfs_data_write,
};

/**
 * ice_debugfs_fwlog_init - setup the debugfs directory
 * @pf: the ice that is starting up
 */
void ice_debugfs_fwlog_init(struct ice_pf *pf)
{
	const char *name = pci_name(pf->pdev);

	/* only support fw log commands on PF 0 */
	if (pf->hw.bus.func)
		return;

	pf->ice_debugfs_pf = debugfs_create_dir(name, ice_debugfs_root);
	if (IS_ERR(pf->ice_debugfs_pf)) {
		pr_info("init of debugfs PCI dir failed\n");
		return;
	}

	pf->ice_debugfs_pf_fwlog = debugfs_create_dir("fwlog",
						      pf->ice_debugfs_pf);
	if (IS_ERR(pf->ice_debugfs_pf)) {
		pr_info("init of debugfs fwlog dir failed\n");
		return;
	}

	debugfs_create_file("modules", 0600, pf->ice_debugfs_pf_fwlog,
			    pf, &ice_debugfs_module_fops);

	debugfs_create_file("resolution", 0600,
			    pf->ice_debugfs_pf_fwlog, pf,
			    &ice_debugfs_resolution_fops);

	debugfs_create_file("enable", 0600, pf->ice_debugfs_pf_fwlog,
			    pf, &ice_debugfs_enable_fops);

	debugfs_create_file("nr_buffs", 0600, pf->ice_debugfs_pf_fwlog,
			    pf, &ice_debugfs_nr_buffs_fops);

	debugfs_create_file("data", 0600, pf->ice_debugfs_pf_fwlog,
			    pf, &ice_debugfs_data_fops);

	return;
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
