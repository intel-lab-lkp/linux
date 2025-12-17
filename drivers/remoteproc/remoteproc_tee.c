// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) STMicroelectronics 2025
 * Author: Arnaud Pouliquen <arnaud.pouliquen@foss.st.com>
 */

#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/mutex.h>
#include <linux/remoteproc.h>
#include <linux/remoteproc_tee.h>
#include <linux/slab.h>
#include <linux/tee_drv.h>

#include "remoteproc_internal.h"

#define MAX_TEE_PARAM_ARRAY_MEMBER	4

/*
 * Authentication and load of the firmware image in the remote processor
 * memories by the TEE. After this step the firmware is installed in destination
 * memories, which can then be locked to prevent access by Linux.
 *
 * [in]  params[0].value.a: remote processor identifier
 * [in]  params[1].memref:  buffer containing a temporary copy of the signed
 *			    image to load.
 */
#define TA_RPROC_CMD_LOAD_FW		1

/*
 * Start the remote processor by the TEE
 *
 * [in]  params[0].value.a: remote processor identifier
 */
#define TA_RPROC_CMD_START		2

/*
 * Stop the remote processor by the TEE
 *
 * [in]  params[0].value.a: remote processor identifier
 */
#define TA_RPROC_CMD_STOP		3

/*
 * Return the address of the resource table, or 0 if not found.
 *
 * [in]  params[0].value.a: remote processor identifier
 * [out] params[1].value.a: 32bit LSB resource table memory address
 * [out] params[1].value.b: 32bit MSB resource table memory address
 * [out] params[2].value.a: 32bit LSB resource table memory size
 * [out] params[2].value.b: 32bit MSB resource table memory size
 */
#define TA_RPROC_CMD_GET_RSC_TABLE	4

/*
 * Release remote processor firmware images and associated resources.
 * This command should be used in case an error occurs between the loading of
 * the firmware images (TA_RPROC_CMD_LOAD_FW) and the starting of the remote
 * processor (TA_RPROC_CMD_START) or after stopping the remote processor
 * to release associated resources (TA_RPROC_CMD_STOP).
 *
 * [in]  params[0].value.a: remote processor identifier
 */
#define TA_RPROC_CMD_RELEASE_FW		6

struct rproc_tee_context {
	struct list_head	rproc_list;
	struct tee_context	*tee_ctx;
	struct device		*dev;
};

/**
 * struct rproc_tee - TEE remoteproc structure
 * @node:       Reference in list
 * @rproc:      Remoteproc reference
 * @rproc_id:   remote processor identifier
 * @session_id: TEE session identifier
 */
struct rproc_tee {
	struct list_head node;
	struct rproc *rproc;
	u32 rproc_id;
	u32 session_id;
};

static struct rproc_tee_context rproc_tee_ctx;
static DEFINE_MUTEX(ctx_lock); /* Protects concurrent manipulations of the rproc_tee_ctx*/

static struct rproc_tee *rproc_to_trproc(struct rproc *rproc)
{
	struct rproc_tee *trproc;

	list_for_each_entry(trproc, &rproc_tee_ctx.rproc_list, node)
		if (trproc->rproc == rproc)
			return trproc;
	return NULL;
}

static void rproc_tee_prepare_args(struct rproc_tee *trproc, int cmd,
				   struct tee_ioctl_invoke_arg *arg,
				   struct tee_param *param,
				   unsigned int num_params)
{
	memset(arg, 0, sizeof(*arg));
	memset(param, 0, MAX_TEE_PARAM_ARRAY_MEMBER * sizeof(*param));

	arg->func = cmd;
	arg->session = trproc->session_id;
	arg->num_params = num_params + 1;

	param[0] = (struct tee_param) {
		.attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT,
		.u.value.a = trproc->rproc_id,
	};
}

/**
 * rproc_tee_release_fw() - Release the firmware for a TEE-based remote processor
 * @rproc: Pointer to the struct rproc representing the remote processor
 *
 * This function invokes the TA_RPROC_CMD_RELEASE_FW TEE client function to
 * release the firmware. It should only be called when the remoteproc state is
 * RPROC_OFFLINE or RPROC_DETACHED. The function requests the TEE remoteproc
 * application to release the firmware loaded by rproc_tee_load_fw().
 * The request is ignored if the rproc state is RPROC_DETACHED as the remote
 * processor is still running.
 */
void rproc_tee_release_fw(struct rproc *rproc)
{
	struct tee_param param[MAX_TEE_PARAM_ARRAY_MEMBER];
	struct rproc_tee *trproc;
	struct tee_ioctl_invoke_arg arg;
	int ret;

	ret = mutex_lock_interruptible(&ctx_lock);
	if (ret)
		return;

	if (!rproc_tee_ctx.dev)
		goto out;

	trproc = rproc_to_trproc(rproc);
	if (!trproc)
		goto out;

	/*
	 * If the remote processor state is RPROC_DETACHED, just ignore the
	 * request, as the remote processor is still running.
	 */
	if (rproc->state == RPROC_DETACHED)
		goto out;

	if (rproc->state != RPROC_OFFLINE) {
		dev_err(rproc_tee_ctx.dev, "unexpected rproc state: %d\n", rproc->state);
		goto out;
	}

	rproc_tee_prepare_args(trproc, TA_RPROC_CMD_RELEASE_FW, &arg, param, 0);

	ret = tee_client_invoke_func(rproc_tee_ctx.tee_ctx, &arg, param);
	if (ret < 0 || arg.ret != 0) {
		dev_err(rproc_tee_ctx.dev,
			"TA_RPROC_CMD_RELEASE_FW invoke failed TEE err: %#x, ret:%d\n",
			arg.ret, ret);
	}

out:
	mutex_unlock(&ctx_lock);
}
EXPORT_SYMBOL_GPL(rproc_tee_release_fw);

/**
 * rproc_tee_load_fw() - Load firmware from TEE application
 * @rproc: Pointer to the struct rproc representing the remote processor
 * @fw: Pointer to the firmware structure containing the firmware data and size
 *
 * This function invokes the TA_RPROC_CMD_LOAD_FW TEE client function to load
 * the firmware. It registers the fw->data as a shared memory region with the
 * TEE, and request the TEE to load the firmware. This function can be called
 * twice during the remote processor boot, a first by rproc_tee_parse_fw() to
 * parse the resource table , and a second time by rproc_tee_load_fw().
 * The TEE application should ignores the command if the firmware
 * is already loaded by rproc_tee_parse_fw().
 *
 * Return: 0 on success, or an error code on failure
 */
int rproc_tee_load_fw(struct rproc *rproc, const struct firmware *fw)
{
	struct tee_param param[MAX_TEE_PARAM_ARRAY_MEMBER];
	struct rproc_tee *trproc;
	struct tee_ioctl_invoke_arg arg;
	struct tee_shm *fw_shm;
	int ret;

	ret = mutex_lock_interruptible(&ctx_lock);
	if (ret)
		return ret;

	if (!rproc_tee_ctx.dev) {
		ret = -ENODEV;
		goto out;
	}

	trproc = rproc_to_trproc(rproc);
	if (!trproc) {
		ret = -EINVAL;
		goto out;
	}

	fw_shm = tee_shm_register_kernel_buf(rproc_tee_ctx.tee_ctx, (void *)fw->data, fw->size);
	if (IS_ERR(fw_shm)) {
		ret = PTR_ERR(fw_shm);
		goto out;
	}

	rproc_tee_prepare_args(trproc, TA_RPROC_CMD_LOAD_FW, &arg, param, 1);

	/* Provide the address of the firmware image */
	param[1] = (struct tee_param) {
		.attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT,
		.u.memref = {
			.shm = fw_shm,
			.size = fw->size,
			.shm_offs = 0,
		},
	};

	ret = tee_client_invoke_func(rproc_tee_ctx.tee_ctx, &arg, param);
	if (ret < 0 || arg.ret != 0) {
		dev_err(rproc_tee_ctx.dev,
			"TA_RPROC_CMD_LOAD_FW invoke failed TEE err: %#x, ret:%d\n",
			arg.ret, ret);
		if (!ret)
			ret = -EIO;
	}

	tee_shm_free(fw_shm);

out:
	mutex_unlock(&ctx_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(rproc_tee_load_fw);

static int rproc_tee_get_loaded_rsc_table(struct rproc *rproc, phys_addr_t *rsc_pa,
					  size_t *table_sz)
{
	struct tee_param param[MAX_TEE_PARAM_ARRAY_MEMBER];
	struct rproc_tee *trproc;
	struct tee_ioctl_invoke_arg arg;
	int ret;

	ret = mutex_lock_interruptible(&ctx_lock);
	if (ret)
		return ret;

	if (!rproc_tee_ctx.dev) {
		ret = -ENODEV;
		goto out;
	}

	trproc = rproc_to_trproc(rproc);
	if (!trproc) {
		ret = -EINVAL;
		goto out;
	}

	rproc_tee_prepare_args(trproc, TA_RPROC_CMD_GET_RSC_TABLE, &arg, param, 2);

	param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;
	param[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;

	ret = tee_client_invoke_func(rproc_tee_ctx.tee_ctx, &arg, param);
	if (ret < 0 || arg.ret != 0) {
		dev_err(rproc_tee_ctx.dev,
			"TA_RPROC_CMD_GET_RSC_TABLE invoke failed TEE err: %#x, ret:%d\n",
			arg.ret, ret);
		ret = -EIO;
		goto out;
	}

	*table_sz = param[2].u.value.a;
	if (sizeof(phys_addr_t) == sizeof(u64))
		*table_sz |= param[2].u.value.b << 32;

	if (*table_sz) {
		*rsc_pa = param[1].u.value.a;
		if (sizeof(phys_addr_t) == sizeof(u64))
			*rsc_pa |= param[1].u.value.b << 32;
	} else {
		*rsc_pa = 0;
	}

out:
	mutex_unlock(&ctx_lock);
	return ret;
}

/**
 * rproc_tee_parse_fw() - Get the resource table from TEE application
 * @rproc: Pointer to the struct rproc representing the remote processor
 * @fw: Pointer to the firmware structure containing the firmware data and size
 *
 * This function retrieves the loaded resource table and creates a cached_table
 * copy. Since the firmware image is signed and potentially encrypted, the
 * firmware must be loaded first to access the loaded resource table.
 *
 * Return: 0 on success, or an error code on failure
 */
int rproc_tee_parse_fw(struct rproc *rproc, const struct firmware *fw)
{
	phys_addr_t rsc_table;
	void *rsc_va;
	size_t table_sz;
	int ret;

	if (!rproc)
		return -EINVAL;

	/* We need first to Load the firmware, to be able to get the resource table. */
	ret = rproc_tee_load_fw(rproc, fw);
	if (ret)
		return ret;

	ret = rproc_tee_get_loaded_rsc_table(rproc, &rsc_table, &table_sz);
	if (ret)
		goto release_fw;

	/*
	 * We assume here that the memory mapping is the same between the TEE
	 * and Linux kernel contexts. Else a new TEE remoteproc service could be
	 * needed to get a copy of the resource table.
	 */
	rsc_va = memremap(rsc_table, table_sz, MEMREMAP_WC);
	if (!rsc_va) {
		dev_err(rproc_tee_ctx.dev, "Unable to map memory region: %pa+%zx\n",
			&rsc_table, table_sz);
		ret = -ENOMEM;
		goto release_fw;
	}

	/*
	 * Create a copy of the resource table to have the same behavior as the
	 * ELF loader. This cached table will be used by the remoteproc core
	 * after the remoteproc stops to free resources and for crash recovery
	 * to reapply the settings.
	 * The cached table will be freed by the remoteproc core.
	 */
	rproc->cached_table = kmemdup(rsc_va, table_sz, GFP_KERNEL);
	memunmap(rsc_va);

	if (!rproc->cached_table) {
		ret = -ENOMEM;
		goto release_fw;
	}

	rproc->table_ptr = rproc->cached_table;
	rproc->table_sz = table_sz;

	return 0;

release_fw:
	rproc_tee_release_fw(rproc);
	return ret;
}
EXPORT_SYMBOL_GPL(rproc_tee_parse_fw);

/**
 * rproc_tee_find_loaded_rsc_table() - Find the loaded resource table loaded by
 *				       the TEE application
 * @rproc: Pointer to the struct rproc representing the remote processor
 * @fw: Pointer to the firmware structure containing the firmware data and size
 *
 * This function retrieves the physical address and size of the resource table
 * loaded by the TEE application.
 *
 * Return: pointer to the resource table if found, or NULL if not found or size
 * is 0
 */
struct resource_table *rproc_tee_find_loaded_rsc_table(struct rproc *rproc,
						       const struct firmware *fw)
{
	phys_addr_t rsc_table;
	size_t table_sz;
	int ret;

	ret = rproc_tee_get_loaded_rsc_table(rproc, &rsc_table, &table_sz);
	if (ret)
		return NULL;

	rproc->table_sz = table_sz;
	if (!table_sz)
		return NULL;

	/*
	 * At this step the memory area that contains the resource table should
	 * have been registered by the remote proc platform driver and allocated
	 * by rproc_alloc_registered_carveouts().
	 */
	return rproc_pa_to_va(rproc, rsc_table, table_sz, NULL);
}
EXPORT_SYMBOL_GPL(rproc_tee_find_loaded_rsc_table);

/**
 * rproc_tee_start() - Request the TEE application to start the remote processor
 * @rproc: Pointer to the struct rproc representing the remote processor
 *
 * This function invokes the TA_RPROC_CMD_START command to start the remote
 * processor.
 *
 * Return: Returns 0 on success, -EINVAL or -EIO on failure
 */
int rproc_tee_start(struct rproc *rproc)
{
	struct tee_param param[MAX_TEE_PARAM_ARRAY_MEMBER];
	struct rproc_tee *trproc;
	struct tee_ioctl_invoke_arg arg;
	int ret;

	ret = mutex_lock_interruptible(&ctx_lock);
	if (ret)
		return ret;

	if (!rproc_tee_ctx.dev) {
		ret = -ENODEV;
		goto out;
	}

	trproc = rproc_to_trproc(rproc);
	if (!trproc) {
		ret = -EINVAL;
		goto out;
	}

	rproc_tee_prepare_args(trproc, TA_RPROC_CMD_START, &arg, param, 0);

	ret = tee_client_invoke_func(rproc_tee_ctx.tee_ctx, &arg, param);
	if (ret < 0 || arg.ret != 0) {
		dev_err(rproc_tee_ctx.dev,
			"TA_RPROC_CMD_START invoke failed TEE err: %#x, ret:%d\n", arg.ret, ret);
		if (!ret)
			ret = -EIO;
	}

out:
	mutex_unlock(&ctx_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(rproc_tee_start);

/**
 * rproc_tee_stop() - Request the TEE application to start the remote processor
 * @rproc: Pointer to the struct rproc representing the remote processor
 *
 * This function invokes the TA_RPROC_CMD_STOP command to stop the remote
 * processor.
 *
 * Return: Returns 0 on success, -EINVAL or -EIO on failure
 */
int rproc_tee_stop(struct rproc *rproc)
{
	struct tee_param param[MAX_TEE_PARAM_ARRAY_MEMBER];
	struct rproc_tee *trproc;
	struct tee_ioctl_invoke_arg arg;
	int ret;

	ret = mutex_lock_interruptible(&ctx_lock);
	if (ret)
		return ret;

	if (!rproc_tee_ctx.dev) {
		ret = -ENODEV;
		goto out;
	}

	trproc = rproc_to_trproc(rproc);
	if (!trproc) {
		ret = -EINVAL;
		goto out;
	}

	rproc_tee_prepare_args(trproc, TA_RPROC_CMD_STOP, &arg, param, 0);

	ret = tee_client_invoke_func(rproc_tee_ctx.tee_ctx, &arg, param);
	if (ret < 0 || arg.ret != 0) {
		dev_err(rproc_tee_ctx.dev,
			"TA_RPROC_CMD_STOP invoke failed TEE err: %#x, ret:%d\n", arg.ret, ret);
		if (!ret)
			ret = -EIO;
	}

out:
	mutex_unlock(&ctx_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(rproc_tee_stop);

static const struct rproc_ops rproc_tee_default_ops = {
	.start		= rproc_tee_start,
	.stop		= rproc_tee_stop,
	.load		= rproc_tee_load_fw,
	.parse_fw	= rproc_tee_parse_fw,
	.find_loaded_rsc_table = rproc_tee_find_loaded_rsc_table,
	.release_fw	= rproc_tee_release_fw,
};

static const struct tee_client_device_id rproc_tee_id_table[] = {
	{UUID_INIT(0x80a4c275, 0x0a47, 0x4905, 0x82, 0x85, 0x14, 0x86, 0xa9, 0x77, 0x1a, 0x08)},
	{ /* sentinel */ }
};

/**
 * rproc_tee_register() - Register a remote processor controlled by the TEE application.
 * @dev: Pointer to client rproc device
 * @rproc: Pointer to the struct rproc representing the remote processor
 * @rproc_id: ID of the remote processor
 * @auto_boot: flag to indicate if remote processor should be auto-started
 *
 * This function registers a remote processor that will be managed by a TEE
 * application, by opening a session with the TEE client.
 *
 * Return: Returns 0 on success, or an error code on failure
 */
int rproc_tee_register(struct device *dev, struct rproc **rproc,
		       unsigned int rproc_id, bool auto_boot)
{
	struct tee_param param[MAX_TEE_PARAM_ARRAY_MEMBER];
	struct tee_ioctl_open_session_arg sess_arg;
	struct device_node *np = dev->of_node;
	struct tee_client_device *tee_device;
	struct rproc *new_rproc;
	struct rproc_tee *trproc;
	const char *fw_name;
	int ret;

	ret = mutex_lock_interruptible(&ctx_lock);
	if (ret)
		return ret;

	if (!rproc_tee_ctx.dev) {
		ret = -ENODEV;
		goto out;
	}

	trproc = kzalloc(sizeof(*trproc), GFP_KERNEL);
	if (!trproc) {
		ret = -ENOMEM;
		goto out;
	}

	/* Look for an optional firmware name */
	ret = rproc_of_parse_firmware(dev, 0, &fw_name);
	if (ret < 0 && ret != -EINVAL)
		goto free_tproc;

	new_rproc = rproc_alloc(dev, np->name, &rproc_tee_default_ops,
				fw_name, sizeof(*trproc));
	if (!new_rproc) {
		ret = -ENOMEM;
		goto free_tproc;
	}

	tee_device = to_tee_client_device(rproc_tee_ctx.dev);
	memset(&sess_arg, 0, sizeof(sess_arg));

	memcpy(sess_arg.uuid, tee_device->id.uuid.b, TEE_IOCTL_UUID_LEN);

	sess_arg.clnt_login = TEE_IOCTL_LOGIN_REE_KERNEL;
	sess_arg.num_params = 1;

	param[0] = (struct tee_param) {
		.attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT,
		.u.value.a = rproc_id,
	};

	ret = tee_client_open_session(rproc_tee_ctx.tee_ctx, &sess_arg, param);
	if (ret < 0 || sess_arg.ret != 0) {
		dev_err(dev, "tee_client_open_session failed, err: %#x\n", sess_arg.ret);
		ret = -EINVAL;
		goto free_rproc;
	}

	trproc->rproc_id = rproc_id;
	trproc->session_id = sess_arg.session;
	new_rproc->auto_boot = auto_boot;

	ret = rproc_add(new_rproc);
	if (ret)
		goto close_tee;

	trproc->rproc = new_rproc;
	*rproc = new_rproc;

	list_add_tail(&trproc->node, &rproc_tee_ctx.rproc_list);

	mutex_unlock(&ctx_lock);

	return 0;

close_tee:
	if (tee_client_close_session(rproc_tee_ctx.tee_ctx, trproc->session_id))
		dev_err(rproc_tee_ctx.dev, "tee_client_close_session failed\n");
free_rproc:
	rproc_free(new_rproc);
free_tproc:
	kfree(trproc);
out:
	mutex_unlock(&ctx_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(rproc_tee_register);

/**
 * rproc_tee_unregister - Unregister a remote processor controlled by the TEE
 *                        application.
 * @dev: Pointer to client rproc device
 * @rproc: Pointer to the struct rproc representing the remote processor
 *
 * This function unregisters a remote processor previously registered by the
 * rproc_tee_register() function.
 *
 * Return: Returns 0 on success, or an error code on failure
 */
int rproc_tee_unregister(struct device *dev, struct rproc *rproc)
{
	struct rproc_tee *trproc;
	int ret;

	if (!rproc_tee_ctx.dev) {
		ret = -ENODEV;
		return ret;
	}

	trproc = rproc_to_trproc(rproc);
	if (!trproc) {
		ret = -EINVAL;
		return ret;
	}

	rproc_del(rproc);
	rproc_free(rproc);

	ret = mutex_lock_interruptible(&ctx_lock);
	if (ret)
		return ret;

	ret = tee_client_close_session(rproc_tee_ctx.tee_ctx, trproc->session_id);
	if (ret < 0)
		dev_err(rproc_tee_ctx.dev, "tee_client_close_session failed, err: %#x\n", ret);
	list_del(&trproc->node);
	kfree(trproc);
	mutex_unlock(&ctx_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(rproc_tee_unregister);

static int rproc_tee_ctx_match(struct tee_ioctl_version_data *ver, const void *data)
{
	/* Today we support only the OP-TEE, could be extend to other tees */
	return (ver->impl_id == TEE_IMPL_ID_OPTEE);
}

static const struct of_device_id remoteproc_tee_of_match[] = {
	{ .compatible = "rproc-service-80a4c275-0a47-4905-8285-1486a9771a08" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, remoteproc_tee_of_match);

static int rproc_tee_probe(struct device *dev)
{
	struct tee_context *tee_ctx;
	struct device_node *np;
	int ret;

	pr_err("%s %d", __func__, __LINE__);
	np = of_find_compatible_node(NULL, NULL,
				     remoteproc_tee_of_match[0].compatible);
	if (!np) {
		dev_err(dev, "Device node not found\n");
		return -ENODEV;
	}

	/* Open context with TEE driver */
	tee_ctx = tee_client_open_context(NULL, rproc_tee_ctx_match, NULL, NULL);
	if (IS_ERR(tee_ctx)) {
		ret = PTR_ERR(tee_ctx);
		goto put_node;
	}

	ret = mutex_lock_interruptible(&ctx_lock);
	if (ret)
		goto close_ctx;

	rproc_tee_ctx.dev = dev;
	rproc_tee_ctx.tee_ctx = tee_ctx;
	INIT_LIST_HEAD(&rproc_tee_ctx.rproc_list);

	mutex_unlock(&ctx_lock);

	ret = device_add_of_node(dev, np);
	if (ret)
		goto clean_ctx;

	ret = of_platform_populate(np, NULL, NULL, dev);
	if (ret) {
		dev_err(dev, "cannot populate node (%d)\n", ret);
		goto clean_ctx;
	}

	return 0;

clean_ctx:
	mutex_lock(&ctx_lock);
	rproc_tee_ctx.dev = NULL;
	rproc_tee_ctx.tee_ctx = NULL;
	mutex_unlock(&ctx_lock);

close_ctx:
	tee_client_close_context(tee_ctx);

put_node:
	of_node_put(np);
	return ret;
}

static int rproc_tee_remove(struct device *dev)
{
	of_platform_depopulate(dev);

	device_remove_of_node(dev);

	mutex_lock(&ctx_lock);
	tee_client_close_context(rproc_tee_ctx.tee_ctx);
	rproc_tee_ctx.dev = NULL;
	rproc_tee_ctx.tee_ctx = NULL;
	mutex_unlock(&ctx_lock);

	return 0;
}

MODULE_DEVICE_TABLE(tee, rproc_tee_id_table);

static struct tee_client_driver rproc_tee_fw_driver = {
	.id_table	= rproc_tee_id_table,
	.driver		= {
		.name		= KBUILD_MODNAME,
		.bus		= &tee_bus_type,
		.probe		= rproc_tee_probe,
		.remove		= rproc_tee_remove,
	},
};

static int __init rproc_tee_fw_mod_init(void)
{
	return driver_register(&rproc_tee_fw_driver.driver);
}

static void __exit rproc_tee_fw_mod_exit(void)
{
	driver_unregister(&rproc_tee_fw_driver.driver);
}

module_init(rproc_tee_fw_mod_init);
module_exit(rproc_tee_fw_mod_exit);

MODULE_DESCRIPTION("remote processor TEE module");
MODULE_LICENSE("GPL");
