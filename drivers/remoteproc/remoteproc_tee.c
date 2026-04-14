// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) STMicroelectronics 2025
 * Author: Arnaud Pouliquen <arnaud.pouliquen@foss.st.com>
 */

#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
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

/**
 * struct rproc_tee_context - Global TEE backend context
 * @rproc_list: List of registered TEE-backed remoteprocs
 * @tee_ctx:    TEE context handle
 * @dev:        TEE client device
 */
struct rproc_tee_context {
	struct list_head	rproc_list;
	struct tee_context	*tee_ctx;
	struct device		*dev;
};

/**
 * struct rproc_tee - TEE remoteproc structure
 * @node:       Reference in global list
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

	lockdep_assert_held(&ctx_lock);

	list_for_each_entry(trproc, &rproc_tee_ctx.rproc_list, node) {
		if (trproc->rproc == rproc)
			return trproc;
	}

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

static int rproc_tee_sanity_check(struct device_node *tee_np)
{
	/* Backend not probed yet */
	if (!rproc_tee_ctx.dev || !rproc_tee_ctx.dev->of_node)
		return -EPROBE_DEFER;

	/* DT error: phandle points to different node than the backend we use */
	if (tee_np != rproc_tee_ctx.dev->of_node)
		return -EINVAL;

	return 0;
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

	/* A missing resource table is valid for some firmware images. */
	if (!table_sz) {
		rproc->table_ptr = NULL;
		rproc->table_sz = 0;
		return 0;
	}

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

/**
 * rproc_tee_register() - Register a remote processor controlled by the TEE application
 * @dev:        client rproc device
 * @rproc:      out pointer to the struct rproc representing the remote processor
 * @tee_phandle: backend phandle args (np + 1 cell: rproc_id)
 *
 * The phandle must point to the same DT node that rproc_tee_probe() attached
 * to the TEE client device.
 *
 * Returns 0 on success or a negative error code.
 */
int rproc_tee_register(struct device *dev, struct rproc *rproc,
		       const struct of_phandle_args *tee_phandle)
{
	struct tee_param param[MAX_TEE_PARAM_ARRAY_MEMBER];
	unsigned int rproc_id;
	struct device_node *tee_np;
	struct tee_ioctl_open_session_arg sess_arg;
	struct tee_client_device *tee_device;
	struct device_link *link;
	struct rproc_tee *trproc;
	int ret;

	if (!tee_phandle || !tee_phandle->np || tee_phandle->args_count < 1)
		return -EINVAL;

	rproc_id = tee_phandle->args[0];
	tee_np = tee_phandle->np;

	ret = mutex_lock_interruptible(&ctx_lock);
	if (ret)
		return ret;

	ret = rproc_tee_sanity_check(tee_np);
	if (ret)
		goto out;

	trproc = kzalloc_obj(*trproc);
	if (!trproc) {
		ret = -ENOMEM;
		goto out;
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
		dev_err(dev, "tee_client_open_session failed, err: %#x\n",
			sess_arg.ret);
		ret = -EINVAL;
		goto free_trproc;
	}

	trproc->rproc_id = rproc_id;
	trproc->session_id = sess_arg.session;

	ret = rproc_add(rproc);
	if (ret)
		goto close_tee;

	trproc->rproc = rproc;
	/* Create device link between the rproc device and the TEE device */
	link = device_link_add(dev, rproc_tee_ctx.dev,
			       DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link) {
		ret = -ENOMEM;
		goto del_rproc;
	}

	list_add_rcu(&trproc->node, &rproc_tee_ctx.rproc_list);

	mutex_unlock(&ctx_lock);

	return 0;

del_rproc:
	rproc_del(rproc);
close_tee:
	if (tee_client_close_session(rproc_tee_ctx.tee_ctx, trproc->session_id))
		dev_err(rproc_tee_ctx.dev,
			"tee_client_close_session failed\n");
free_trproc:
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

	ret = mutex_lock_interruptible(&ctx_lock);
	if (ret)
		return ret;

	if (!rproc_tee_ctx.dev) {
		ret = -ENODEV;
		goto out_unlock;
	}

	trproc = rproc_to_trproc(rproc);
	if (!trproc) {
		ret = -EINVAL;
		goto out_unlock;
	}

	/*
	 * Unlock ctx_lock before calling rproc_del(),
	 * they may call into rproc core and take other locks to stop the
	 * remote processor, if it is running.
	 */
	mutex_unlock(&ctx_lock);

	rproc_del(rproc);

	ret = mutex_lock_interruptible(&ctx_lock);
	if (ret)
		return ret;

	ret = tee_client_close_session(rproc_tee_ctx.tee_ctx,
				       trproc->session_id);
	if (ret < 0)
		dev_err(rproc_tee_ctx.dev,
			"tee_client_close_session failed, err: %#x\n", ret);

	list_del(&trproc->node);
	kfree(trproc);

out_unlock:
	mutex_unlock(&ctx_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(rproc_tee_unregister);

static int rproc_tee_ctx_match(struct tee_ioctl_version_data *ver,
			       const void *data)
{
	/* Today we support only OP-TEE; could be extended to other TEEs */
	return ver->impl_id == TEE_IMPL_ID_OPTEE;
}

static const struct tee_client_device_id rproc_tee_id_table[] = {
	{UUID_INIT(0x80a4c275, 0x0a47, 0x4905, 0x82, 0x85, 0x14, 0x86, 0xa9, 0x77, 0x1a, 0x08)},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(tee, rproc_tee_id_table);

static void uuid_to_str(const struct tee_client_device_id *id, char *uuid_str)
{
	snprintf(uuid_str, UUID_STRING_LEN + 1,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 id->uuid.b[0], id->uuid.b[1], id->uuid.b[2], id->uuid.b[3],
		 id->uuid.b[4], id->uuid.b[5], id->uuid.b[6], id->uuid.b[7],
		 id->uuid.b[8], id->uuid.b[9], id->uuid.b[10], id->uuid.b[11],
		 id->uuid.b[12], id->uuid.b[13], id->uuid.b[14], id->uuid.b[15]);
}

static int rproc_tee_probe(struct tee_client_device *tee_dev)
{
	struct device *dev = &tee_dev->dev;
	struct tee_context *tee_ctx;
	struct device_node *np;
	char uuid_str[UUID_STRING_LEN + 1];
	int ret;

	uuid_to_str(&rproc_tee_id_table[0], uuid_str);
	np = of_find_compatible_node(NULL, NULL, uuid_str);
	if (!np) {
		dev_err(dev, "Device node not found for UUID: %s\n", uuid_str);
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

	INIT_LIST_HEAD(&rproc_tee_ctx.rproc_list);

	ret = device_add_of_node(dev, np);

	if (ret) {
		mutex_unlock(&ctx_lock);
		goto close_ctx;
	}
	rproc_tee_ctx.dev = dev;
	rproc_tee_ctx.tee_ctx = tee_ctx;

	mutex_unlock(&ctx_lock);
	of_node_put(np);

	return 0;

close_ctx:
	tee_client_close_context(tee_ctx);
put_node:
	of_node_put(np);
	return ret;
}

static void rproc_tee_remove(struct tee_client_device *tee_dev)
{
	struct device *dev = &tee_dev->dev;
	int ret;

	ret = mutex_lock_interruptible(&ctx_lock);
	if (ret)
		return;

	rproc_tee_ctx.dev = NULL;
	mutex_unlock(&ctx_lock);
	device_remove_of_node(dev);

	tee_client_close_context(rproc_tee_ctx.tee_ctx);
	rproc_tee_ctx.tee_ctx = NULL;
}

static struct tee_client_driver rproc_tee_fw_driver = {
	.id_table	= rproc_tee_id_table,
	.probe		= rproc_tee_probe,
	.remove		= rproc_tee_remove,
	.driver		= {
		.name		= KBUILD_MODNAME,
	},
};

module_tee_client_driver(rproc_tee_fw_driver);

MODULE_DESCRIPTION("remote processor TEE module");
MODULE_LICENSE("GPL");
