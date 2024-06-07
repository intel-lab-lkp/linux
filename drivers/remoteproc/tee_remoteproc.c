// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) STMicroelectronics 2024 - All Rights Reserved
 * Author: Arnaud Pouliquen <arnaud.pouliquen@foss.st.com>
 */

#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/remoteproc.h>
#include <linux/slab.h>
#include <linux/tee_drv.h>
#include <linux/tee_remoteproc.h>

#include "remoteproc_internal.h"

#define MAX_TEE_PARAM_ARRY_MEMBER	4

/*
 * Authentication of the firmware and load in the remote processor memory
 *
 * [in]  params[0].value.a:	unique 32bit identifier of the remote processor
 * [in]	 params[1].memref:	buffer containing the image of the buffer
 */
#define TA_RPROC_FW_CMD_LOAD_FW		1

/*
 * Start the remote processor
 *
 * [in]  params[0].value.a:	unique 32bit identifier of the remote processor
 */
#define TA_RPROC_FW_CMD_START_FW	2

/*
 * Stop the remote processor
 *
 * [in]  params[0].value.a:	unique 32bit identifier of the remote processor
 */
#define TA_RPROC_FW_CMD_STOP_FW		3

/*
 * Return the address of the resource table, or 0 if not found
 * No check is done to verify that the address returned is accessible by
 * the non secure context. If the resource table is loaded in a protected
 * memory the access by the non secure context will lead to a data abort.
 *
 * [in]  params[0].value.a:	unique 32bit identifier of the remote processor
 * [out]  params[1].value.a:	32bit LSB resource table memory address
 * [out]  params[1].value.b:	32bit MSB resource table memory address
 * [out]  params[2].value.a:	32bit LSB resource table memory size
 * [out]  params[2].value.b:	32bit MSB resource table memory size
 */
#define TA_RPROC_FW_CMD_GET_RSC_TABLE	4

/*
 * Return the address of the core dump
 *
 * [in]  params[0].value.a:	unique 32bit identifier of the remote processor
 * [out] params[1].memref:	address of the core dump image if exist,
 *				else return Null
 */
#define TA_RPROC_FW_CMD_GET_COREDUMP	5

struct tee_rproc_context {
	struct list_head sessions;
	struct tee_context *tee_ctx;
	struct device *dev;
};

static struct tee_rproc_context *tee_rproc_ctx;

static void tee_rproc_prepare_args(struct tee_rproc *trproc, int cmd,
				   struct tee_ioctl_invoke_arg *arg,
				   struct tee_param *param,
				   unsigned int num_params)
{
	memset(arg, 0, sizeof(*arg));
	memset(param, 0, MAX_TEE_PARAM_ARRY_MEMBER * sizeof(*param));

	arg->func = cmd;
	arg->session = trproc->session_id;
	arg->num_params = num_params + 1;

	param[0] = (struct tee_param) {
		.attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT,
		.u.value.a = trproc->rproc_id,
	};
}

int tee_rproc_load_fw(struct rproc *rproc, const struct firmware *fw)
{
	struct tee_param param[MAX_TEE_PARAM_ARRY_MEMBER];
	struct tee_rproc *trproc = rproc->tee_interface;
	struct tee_ioctl_invoke_arg arg;
	struct tee_shm *fw_shm;
	int ret;

	if (!trproc)
		return -EINVAL;

	fw_shm = tee_shm_register_kernel_buf(tee_rproc_ctx->tee_ctx, (void *)fw->data, fw->size);
	if (IS_ERR(fw_shm))
		return PTR_ERR(fw_shm);

	tee_rproc_prepare_args(trproc, TA_RPROC_FW_CMD_LOAD_FW, &arg, param, 1);

	/* Provide the address of the firmware image */
	param[1] = (struct tee_param) {
		.attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT,
		.u.memref = {
			.shm = fw_shm,
			.size = fw->size,
			.shm_offs = 0,
		},
	};

	ret = tee_client_invoke_func(tee_rproc_ctx->tee_ctx, &arg, param);
	if (ret < 0 || arg.ret != 0) {
		dev_err(tee_rproc_ctx->dev,
			"TA_RPROC_FW_CMD_LOAD_FW invoke failed TEE err: %x, ret:%x\n",
			arg.ret, ret);
		if (!ret)
			ret = -EIO;
	}

	tee_shm_free(fw_shm);

	return ret;
}
EXPORT_SYMBOL_GPL(tee_rproc_load_fw);

static int tee_rproc_get_loaded_rsc_table(struct rproc *rproc, phys_addr_t *rsc_pa,
					  size_t *table_sz)
{
	struct tee_param param[MAX_TEE_PARAM_ARRY_MEMBER];
	struct tee_rproc *trproc = rproc->tee_interface;
	struct tee_ioctl_invoke_arg arg;
	int ret;

	tee_rproc_prepare_args(trproc, TA_RPROC_FW_CMD_GET_RSC_TABLE, &arg, param, 2);

	param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;
	param[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;

	ret = tee_client_invoke_func(tee_rproc_ctx->tee_ctx, &arg, param);
	if (ret < 0 || arg.ret != 0) {
		dev_err(tee_rproc_ctx->dev,
			"TA_RPROC_FW_CMD_GET_RSC_TABLE invoke failed TEE err: %x, ret:%x\n",
			arg.ret, ret);
		return -EIO;
	}

	*table_sz = param[2].u.value.a;

	if (*table_sz)
		*rsc_pa = param[1].u.value.a;
	else
		*rsc_pa  = 0;

	return 0;
}

int tee_rproc_parse_fw(struct rproc *rproc, const struct firmware *fw)
{
	phys_addr_t rsc_table;
	void __iomem *rsc_va;
	size_t table_sz;
	int ret;

	ret = tee_rproc_load_fw(rproc, fw);
	if (ret)
		return ret;

	ret = tee_rproc_get_loaded_rsc_table(rproc, &rsc_table, &table_sz);
	if (ret)
		return ret;

	/*
	 * We assume here that the memory mapping is the same between the TEE and Linux kernel
	 * contexts. Else a new TEE remoteproc service could be needed to get a copy of the
	 * resource table
	 */
	rsc_va = ioremap_wc(rsc_table, table_sz);
	if (IS_ERR_OR_NULL(rsc_va)) {
		dev_err(tee_rproc_ctx->dev, "Unable to map memory region: %pa+%zx\n",
			&rsc_table, table_sz);
		return -ENOMEM;
	}

	/*
	 * Create a copy of the resource table to have the same behavior as the elf loader.
	 * This cached table will be used after the remoteproc stops to free resources, and for
	 * crash recovery to reapply the settings.
	 */
	rproc->cached_table = kmemdup((__force void *)rsc_va, table_sz, GFP_KERNEL);
	if (!rproc->cached_table) {
		ret = -ENOMEM;
		goto out;
	}

	rproc->table_ptr = rproc->cached_table;
	rproc->table_sz = table_sz;

out:
	iounmap(rsc_va);
	return ret;
}
EXPORT_SYMBOL_GPL(tee_rproc_parse_fw);

struct resource_table *tee_rproc_find_loaded_rsc_table(struct rproc *rproc,
						       const struct firmware *fw)
{
	struct tee_rproc *trproc = rproc->tee_interface;
	phys_addr_t rsc_table;
	size_t table_sz;
	int ret;

	if (!trproc)
		return ERR_PTR(-EINVAL);

	ret = tee_rproc_get_loaded_rsc_table(rproc, &rsc_table, &table_sz);
	if (ret)
		return ERR_PTR(ret);

	rproc->table_sz = table_sz;
	if (!table_sz)
		return NULL;

	/*
	 * At this step the memory area that contains the resource table should have be declared
	 * in the remote proc platform driver and allocated by rproc_alloc_registered_carveouts().
	 */

	return (struct resource_table *)rproc_pa_to_va(rproc, rsc_table, table_sz, NULL);
}
EXPORT_SYMBOL_GPL(tee_rproc_find_loaded_rsc_table);

int tee_rproc_start(struct rproc *rproc)
{
	struct tee_param param[MAX_TEE_PARAM_ARRY_MEMBER];
	struct tee_rproc *trproc = rproc->tee_interface;
	struct tee_ioctl_invoke_arg arg;
	int ret = 0;

	if (!trproc)
		return -EINVAL;

	tee_rproc_prepare_args(trproc, TA_RPROC_FW_CMD_START_FW, &arg, param, 0);

	ret = tee_client_invoke_func(tee_rproc_ctx->tee_ctx, &arg, param);
	if (ret < 0 || arg.ret != 0) {
		dev_err(tee_rproc_ctx->dev,
			"TA_RPROC_FW_CMD_START_FW invoke failed TEE err: %x, ret:%x\n",
			arg.ret, ret);
		if (!ret)
			return  -EIO;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(tee_rproc_start);

int tee_rproc_stop(struct rproc *rproc)
{
	struct tee_param param[MAX_TEE_PARAM_ARRY_MEMBER];
	struct tee_rproc *trproc = rproc->tee_interface;
	struct tee_ioctl_invoke_arg arg;
	int ret;

	if (!trproc)
		return -EINVAL;

	tee_rproc_prepare_args(trproc, TA_RPROC_FW_CMD_STOP_FW, &arg, param, 0);

	ret = tee_client_invoke_func(tee_rproc_ctx->tee_ctx, &arg, param);
	if (ret < 0 || arg.ret != 0) {
		dev_err(tee_rproc_ctx->dev,
			"TA_RPROC_FW_CMD_STOP_FW invoke failed TEE err: %x, ret:%x\n",
			arg.ret, ret);
		if (!ret)
			ret = -EIO;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(tee_rproc_stop);

static const struct tee_client_device_id stm32_tee_rproc_id_table[] = {
	{UUID_INIT(0x80a4c275, 0x0a47, 0x4905,
		   0x82, 0x85, 0x14, 0x86, 0xa9, 0x77, 0x1a, 0x08)},
	{}
};

struct tee_rproc *tee_rproc_register(struct device *dev, struct rproc *rproc, unsigned int rproc_id)
{
	struct tee_param param[MAX_TEE_PARAM_ARRY_MEMBER];
	struct tee_ioctl_open_session_arg sess_arg;
	struct tee_client_device *tee_device;
	struct tee_rproc *trproc, *p_err;
	int ret;

	/*
	 * Test if the device has been probed by the TEE bus. In case of failure, we ignore the
	 * reason. The bus could be not yet probed or the service not available in the secure
	 * firmware.The assumption in such a case is that the TEE remoteproc is not probed.
	 */
	if (!tee_rproc_ctx)
		return ERR_PTR(-EPROBE_DEFER);

	/* Prevent tee rproc module from being removed */
	if (!try_module_get(THIS_MODULE)) {
		dev_err(tee_rproc_ctx->dev, "can't get owner\n");
		p_err = ERR_PTR(-ENODEV);
		goto module_put;
	}

	trproc =  devm_kzalloc(dev, sizeof(*trproc), GFP_KERNEL);
	if (!trproc) {
		p_err = ERR_PTR(-ENOMEM);
		goto module_put;
	}
	tee_device = to_tee_client_device(tee_rproc_ctx->dev);
	memset(&sess_arg, 0, sizeof(sess_arg));

	memcpy(sess_arg.uuid, tee_device->id.uuid.b, TEE_IOCTL_UUID_LEN);

	sess_arg.clnt_login = TEE_IOCTL_LOGIN_REE_KERNEL;
	sess_arg.num_params = 1;

	param[0] = (struct tee_param) {
		.attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT,
		.u.value.a = rproc_id,
	};

	ret = tee_client_open_session(tee_rproc_ctx->tee_ctx, &sess_arg, param);
	if (ret < 0 || sess_arg.ret != 0) {
		dev_err(dev, "tee_client_open_session failed, err: %x\n", sess_arg.ret);
		p_err = ERR_PTR(-EINVAL);
		goto module_put;
	}

	trproc->parent =  dev;
	trproc->rproc_id = rproc_id;
	trproc->session_id = sess_arg.session;

	trproc->rproc = rproc;
	rproc->tee_interface = trproc;

	list_add_tail(&trproc->node, &tee_rproc_ctx->sessions);

	return trproc;

module_put:
	module_put(THIS_MODULE);
	return p_err;
}
EXPORT_SYMBOL_GPL(tee_rproc_register);

int tee_rproc_unregister(struct tee_rproc *trproc)
{
	struct rproc *rproc = trproc->rproc;
	int ret;

	ret = tee_client_close_session(tee_rproc_ctx->tee_ctx, trproc->session_id);
	if (ret < 0)
		dev_err(trproc->parent,	"tee_client_close_session failed, err: %x\n", ret);

	list_del(&trproc->node);
	rproc->tee_interface = NULL;

	module_put(THIS_MODULE);

	return ret;
}
EXPORT_SYMBOL_GPL(tee_rproc_unregister);

static int tee_rproc_ctx_match(struct tee_ioctl_version_data *ver, const void *data)
{
	/* Today we support only the OP-TEE, could be extend to other tees */
	return (ver->impl_id == TEE_IMPL_ID_OPTEE);
}

static int tee_rproc_probe(struct device *dev)
{
	struct tee_context *tee_ctx;
	int ret;

	/* Open context with TEE driver */
	tee_ctx = tee_client_open_context(NULL, tee_rproc_ctx_match, NULL, NULL);
	if (IS_ERR(tee_ctx))
		return PTR_ERR(tee_ctx);

	tee_rproc_ctx = devm_kzalloc(dev, sizeof(*tee_ctx), GFP_KERNEL);
	if (!tee_rproc_ctx) {
		ret = -ENOMEM;
		goto err;
	}

	tee_rproc_ctx->dev = dev;
	tee_rproc_ctx->tee_ctx = tee_ctx;
	INIT_LIST_HEAD(&tee_rproc_ctx->sessions);

	return 0;
err:
	tee_client_close_context(tee_ctx);

	return ret;
}

static int tee_rproc_remove(struct device *dev)
{
	struct tee_rproc *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, &tee_rproc_ctx->sessions, node) {
		tee_client_close_session(tee_rproc_ctx->tee_ctx, entry->session_id);
		list_del(&entry->node);
		kfree(entry);
	}

	tee_client_close_context(tee_rproc_ctx->tee_ctx);

	return 0;
}

MODULE_DEVICE_TABLE(tee, stm32_tee_rproc_id_table);

static struct tee_client_driver tee_rproc_fw_driver = {
	.id_table	= stm32_tee_rproc_id_table,
	.driver		= {
		.name		= KBUILD_MODNAME,
		.bus		= &tee_bus_type,
		.probe		= tee_rproc_probe,
		.remove		= tee_rproc_remove,
	},
};

static int __init tee_rproc_fw_mod_init(void)
{
	return driver_register(&tee_rproc_fw_driver.driver);
}

static void __exit tee_rproc_fw_mod_exit(void)
{
	driver_unregister(&tee_rproc_fw_driver.driver);
}

module_init(tee_rproc_fw_mod_init);
module_exit(tee_rproc_fw_mod_exit);

MODULE_DESCRIPTION(" TEE remote processor control driver");
MODULE_LICENSE("GPL");
