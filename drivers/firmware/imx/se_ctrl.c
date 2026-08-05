// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2026 NXP
 */

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/firmware.h>
#include <linux/firmware/imx/se_api.h>
#include <linux/genalloc.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sys_soc.h>
#include <uapi/linux/se_ioctl.h>

#include "ele_base_msg.h"
#include "ele_common.h"
#include "ele_fw_api.h"
#include "se_ctrl.h"

/* Maximum response buffer size in bytes for debug-dump replies. */
#define MAX_ALLOWED_RX_MSG_SZ		ELE_DEBUG_DUMP_RSP_SZ
#define MAX_ALLOWED_TX_MSG_SZ		SZ_4K

#define MAX_SOC_INFO_DATA_SZ		256
#define MBOX_TX_NAME			"tx"
#define MBOX_RX_NAME			"rx"

struct se_soc_dev_regn {
	bool soc_dev_registered;
	struct soc_device *soc_dev;
	struct soc_device_attribute *soc_dev_attr;
};

struct se_var_info {
	u16 soc_rev;
	struct se_soc_dev_regn soc_dev_regn;
	/* To serialize populating common SoC level info. */
	struct mutex se_var_info_lock;
};

/* contains fixed information */
struct se_soc_info {
	const u16 soc_id;
	const char *soc_name;
	const struct se_fw_img_name se_fw_img_nm;
	bool imem_state_mgmt;
};

struct se_if_node {
	struct se_soc_info *se_info;
	u8 *pool_name;
	bool reserved_dma_ranges;
	struct se_if_defines if_defs;
};

/* common for all the SoC. */
static struct se_var_info var_se_info = {
	.soc_rev = 0,
	.se_var_info_lock = __MUTEX_INITIALIZER(var_se_info.se_var_info_lock)
};

static struct se_soc_info se_imx8ulp_info = {
	.soc_id = SOC_ID_OF_IMX8ULP,
	.soc_name = "i.MX8ULP",
	.se_fw_img_nm = {
		.prim_fw_nm_in_rfs = IMX_ELE_FW_DIR
			"mx8ulpa2-ahab-container.img",
		.seco_fw_nm_in_rfs = IMX_ELE_FW_DIR
			"mx8ulpa2ext-ahab-container.img",
	},
	.imem_state_mgmt = true,
};

static struct se_if_node imx8ulp_se_ele_hsm = {
	.se_info = &se_imx8ulp_info,
	.pool_name = "sram",
	.reserved_dma_ranges = true,
	.if_defs = {
		.se_if_type = SE_TYPE_ID_HSM,
		.cmd_tag = 0x17,
		.rsp_tag = 0xe1,
		.success_tag = ELE_SUCCESS_IND,
		.base_api_ver = MESSAGING_VERSION_6,
		.fw_api_ver = MESSAGING_VERSION_7,
	},
};

static struct se_soc_info se_imx93_info = {
	.soc_id = SOC_ID_OF_IMX93,
};

static struct se_if_node imx93_se_ele_hsm = {
	.se_info = &se_imx93_info,
	.reserved_dma_ranges = true,
	.if_defs = {
		.se_if_type = SE_TYPE_ID_HSM,
		.cmd_tag = 0x17,
		.rsp_tag = 0xe1,
		.success_tag = ELE_SUCCESS_IND,
		.base_api_ver = MESSAGING_VERSION_6,
		.fw_api_ver = MESSAGING_VERSION_7,
	},
};

static const struct of_device_id se_match[] = {
	{ .compatible = "fsl,imx8ulp-se-ele-hsm", .data = &imx8ulp_se_ele_hsm },
	{ .compatible = "fsl,imx93-se-ele-hsm", .data = &imx93_se_ele_hsm },
	{ }
};
MODULE_DEVICE_TABLE(of, se_match);

char *get_se_if_name(u8 se_if_id)
{
	switch (se_if_id) {
	case SE_TYPE_ID_DBG: return SE_TYPE_STR_DBG;
	case SE_TYPE_ID_HSM: return SE_TYPE_STR_HSM;
	}

	return "unknown";
}

static u32 get_se_soc_id(struct se_if_priv *priv)
{
	const struct se_if_node *if_node = device_get_match_data(priv->dev);

	return if_node->se_info->soc_id;
}

static struct se_fw_load_info *get_load_fw_instance(struct se_if_priv *priv)
{
	return &priv->load_fw;
}

static void se_soc_device_unregister(struct se_soc_dev_regn *soc_dev_regn)
{
	guard(mutex)(&var_se_info.se_var_info_lock);

	if (soc_dev_regn->soc_dev) {
		soc_device_unregister(soc_dev_regn->soc_dev);
		soc_dev_regn->soc_dev = NULL;
	}

	if (soc_dev_regn->soc_dev_attr) {
		/*
		 * revision and serial_number are the only kasprintf()-allocated
		 * strings. machine points into the DT, and soc_id/family are
		 * constants, so they must not be freed.
		 */
		kfree(soc_dev_regn->soc_dev_attr->revision);
		kfree(soc_dev_regn->soc_dev_attr->serial_number);
		kfree(soc_dev_regn->soc_dev_attr);
		soc_dev_regn->soc_dev_attr = NULL;
	}

	soc_dev_regn->soc_dev_registered = false;
}

/*
 * Build and register a soc_device entry for this SoC. Separated from
 * get_se_soc_info() so that the firmware-fetch path and the sysfs
 * registration path can be reasoned about independently.
 */
static int se_soc_dev_register(struct se_if_priv *priv, u16 soc_rev,
			       const char *soc_name, const u8 *uid)
{
	struct soc_device_attribute *attr;
	struct soc_device *sdev;
	int err;

	if (!soc_rev || !soc_name || !uid)
		return -EINVAL;

	attr = kzalloc_obj(*attr, GFP_KERNEL);
	if (!attr)
		return -ENOMEM;

	if (FIELD_GET(DEV_GETINFO_MIN_VER_MASK, soc_rev))
		attr->revision = kasprintf(GFP_KERNEL, "%x.%x",
					   FIELD_GET(DEV_GETINFO_MAJ_VER_MASK, soc_rev),
					   FIELD_GET(DEV_GETINFO_MIN_VER_MASK, soc_rev));
	else
		attr->revision = kasprintf(GFP_KERNEL, "%x",
					   FIELD_GET(DEV_GETINFO_MAJ_VER_MASK, soc_rev));

	if (!attr->revision) {
		err = -ENOMEM;
		goto err_free_attr;
	}

	attr->soc_id = soc_name;

	err = of_property_read_string(of_root, "model", &attr->machine);
	if (err) {
		err = -EINVAL;
		goto err_free_rev;
	}

	attr->family = "Freescale i.MX";

	attr->serial_number = kasprintf(GFP_KERNEL, "%016llX",
					GET_SERIAL_NUM_FROM_UID(uid, MAX_UID_SIZE >> 2));
	if (!attr->serial_number) {
		err = -ENOMEM;
		goto err_free_rev;
	}

	sdev = soc_device_register(attr);
	if (IS_ERR(sdev)) {
		err = PTR_ERR(sdev);
		goto err_free_serial;
	}

	/*
	 * Publish the singleton. Freed once, at module unload, by
	 * se_soc_device_unregister(). Caller holds se_var_info_lock.
	 */
	var_se_info.soc_dev_regn.soc_dev = sdev;
	var_se_info.soc_dev_regn.soc_dev_attr = attr;

	/* Mark registration complete so get_se_soc_info() skips this path on retry. */
	var_se_info.soc_dev_regn.soc_dev_registered = true;

	return 0;

err_free_serial:
	kfree(attr->serial_number);
err_free_rev:
	kfree(attr->revision);
err_free_attr:
	kfree(attr);

	return err;
}

static int get_se_soc_info(struct se_if_priv *priv, const struct se_soc_info *se_info)
{
	struct se_fw_load_info *load_fw = get_load_fw_instance(priv);
	u8 data[MAX_SOC_INFO_DATA_SZ];
	struct ele_dev_info *s_info;
	int err;

	guard(mutex)(&var_se_info.se_var_info_lock);

	/*
	 * Early exit: both objectives already complete, nothing to do.
	 */
	if (var_se_info.soc_rev &&
	    (!se_info->soc_name || var_se_info.soc_dev_regn.soc_dev_registered))
		return 0;

	err = ele_fetch_soc_info(priv, &data);
	if (err < 0)
		return dev_err_probe(priv->dev, err, "Failed to fetch SoC Info.");

	s_info = (struct ele_dev_info *)data;

	if (!var_se_info.soc_rev) {
		var_se_info.soc_rev = s_info->d_info.soc_rev;

		/*
		 * Only update IMEM state when the load_fw path is active;
		 * on SoCs without IMEM management (e.g. i.MX93) the field
		 * is not meaningful.
		 */
		if (load_fw->imem_mgmt)
			load_fw->imem.state = s_info->d_addn_info.imem_state;
	}

	if (se_info->soc_name && !var_se_info.soc_dev_regn.soc_dev_registered) {
		err = se_soc_dev_register(priv, var_se_info.soc_rev,
					  se_info->soc_name, s_info->d_info.uid);
		if (err < 0)
			return dev_err_probe(priv->dev, err,
					     "Failed to register SE SoC device.");
	}

	return 0;
}

static int load_firmware(struct se_if_priv *priv, const u8 *se_img_file_to_load)
{
	const struct firmware *fw = NULL;
	dma_addr_t se_fw_dma_addr;
	u32 se_fw_buf_len;
	void *se_fw_buf;
	int ret;

	if (!se_img_file_to_load) {
		dev_err(priv->dev, "FW image is not provided.");
		return -EINVAL;
	}
	ret = request_firmware(&fw, se_img_file_to_load, priv->dev);
	if (ret)
		return ret;

	if (fw->size > U32_MAX) {
		ret = -EFBIG;
		release_firmware(fw);
		return ret;
	}
	dev_info(priv->dev, "loading firmware %s.", se_img_file_to_load);

	/*
	 * Serialize access to priv_dev_ctx shared memory to prevent pos
	 * corruption if two driver-internal callers run concurrently (e.g.
	 * ele_get_info() racing with load_firmware()).
	 */
	scoped_guard(mutex, &priv->priv_dev_ctx->fops_lock) {
		se_fw_buf_len = fw->size;
		ret = get_shared_mem_slot(priv->priv_dev_ctx,
					  &se_fw_buf_len, &se_fw_dma_addr,
					  &se_fw_buf);
		if (ret) {
			dev_err(priv->dev, "Failed to allocate firmware shared buffer: %d\n",
				ret);
			release_firmware(fw);
			return ret;
		}

		memcpy(se_fw_buf, fw->data, fw->size);
		ret = ele_fw_authenticate(priv, se_fw_dma_addr, se_fw_dma_addr);
		if (ret < 0) {
			dev_err(priv->dev,
				"Error %pe: Authenticate & load SE firmware %s.",
				ERR_PTR(ret), se_img_file_to_load);
			ret = -EPERM;
		}
		if (!se_is_fw_busy_ctx(priv->priv_dev_ctx))
			se_dev_ctx_shared_mem_cleanup(priv->priv_dev_ctx);
	}

	release_firmware(fw);

	return ret;
}

static int se_load_firmware(struct se_if_priv *priv)
{
	struct se_fw_load_info *load_fw = get_load_fw_instance(priv);
	int ret = 0;

	guard(mutex)(&load_fw->load_fw_lock);
	if (!load_fw->is_fw_tobe_loaded)
		return 0;

	if (load_fw->imem.state == ELE_IMEM_STATE_BAD) {
		ret = load_firmware(priv, load_fw->se_fw_img_nm->prim_fw_nm_in_rfs);
		if (ret) {
			dev_err(priv->dev, "Failed to load boot firmware.");
			return -EPERM;
		}
	}

	ret = load_firmware(priv, load_fw->se_fw_img_nm->seco_fw_nm_in_rfs);
	if (ret) {
		dev_err(priv->dev, "Failed to load runtime firmware.");
		return -EPERM;
	}

	load_fw->is_fw_tobe_loaded = false;

	return ret;
}

static int init_se_shared_mem(struct se_if_device_ctx *dev_ctx)
{
	struct se_shared_mem_mgmt_info *se_shared_mem_mgmt = &dev_ctx->se_shared_mem_mgmt;
	struct se_if_priv *priv = dev_ctx->priv;

	INIT_LIST_HEAD(&se_shared_mem_mgmt->pending_out);
	INIT_LIST_HEAD(&se_shared_mem_mgmt->pending_in);

	if (priv->mem_pool)
		INIT_LIST_HEAD(&se_shared_mem_mgmt->mem_pool_buf_list);

	se_shared_mem_mgmt->non_secure_mem.ptr =
			dma_alloc_coherent(priv->dev, MAX_DATA_SIZE_PER_USER,
					   &se_shared_mem_mgmt->non_secure_mem.dma_addr,
					   GFP_KERNEL);
	if (!se_shared_mem_mgmt->non_secure_mem.ptr)
		return -ENOMEM;

	se_shared_mem_mgmt->non_secure_mem.size = MAX_DATA_SIZE_PER_USER;
	se_shared_mem_mgmt->non_secure_mem.pos = 0;

	return 0;
}

static void cleanup_se_shared_mem(struct se_if_device_ctx *dev_ctx, bool reclaim)
{
	struct se_shared_mem_mgmt_info *se_shared_mem_mgmt = &dev_ctx->se_shared_mem_mgmt;
	struct se_if_priv *priv = dev_ctx->priv;
	bool free_dma_buf;

	/*
	 * mem_pool_buf_list is only initialised for interfaces that own a
	 * gen_pool (priv->mem_pool != NULL). On interfaces without a pool
	 * (e.g. imx93, which has no pool_name) the list head is left
	 * zero-filled, so se_cleanup_mem_pool_buf() must not walk it here or
	 * list_for_each_entry_safe() would dereference a NULL head and panic
	 * the kernel on close/teardown. Skip the pool cleanup entirely when
	 * there is no pool; there is nothing to reclaim in that case.
	 */
	if (priv->mem_pool)
		se_cleanup_mem_pool_buf(dev_ctx, reclaim);

	/* Guard against being called before shared memory was ever allocated
	 * (e.g. probe failure before dma_alloc_coherent succeeded).
	 */
	if (!se_shared_mem_mgmt->non_secure_mem.ptr)
		return;

	/*
	 * Decide whether the DMA buffer can be released before touching the
	 * pending lists. se_dev_ctx_shared_mem_cleanup() resets
	 * non_secure_mem.pos, so the "nothing staged" test must be sampled
	 * here first. When reclaim is false the buffer is released only if no
	 * data is still staged for the firmware; otherwise the enclave may
	 * still be DMA-ing into it and the buffer is deliberately leaked to
	 * avoid a DMA-after-free.
	 */
	free_dma_buf = reclaim || !se_shared_mem_mgmt->non_secure_mem.pos;

	/*
	 * Free any se_buf_desc items that were never consumed (e.g. when the
	 * fd is closed while pending I/O buffers are still listed). This must
	 * happen before the DMA backing memory is released to avoid a leak.
	 */
	se_dev_ctx_shared_mem_cleanup(dev_ctx);

	if (free_dma_buf) {
		dma_free_coherent(priv->dev, MAX_DATA_SIZE_PER_USER,
				  se_shared_mem_mgmt->non_secure_mem.ptr,
				  se_shared_mem_mgmt->non_secure_mem.dma_addr);
	}

	/*
	 * Drop the host-side tracking unconditionally. On the reclaim path the
	 * buffer has been freed. On the deliberate-leak path the buffer is
	 * abandoned on purpose, so clearing the pointer here guarantees a later
	 * cleanup pass (e.g. se_if_priv_release()) cannot double-free it.
	 */
	se_shared_mem_mgmt->non_secure_mem.ptr = NULL;
	se_shared_mem_mgmt->non_secure_mem.dma_addr = 0;
	se_shared_mem_mgmt->non_secure_mem.size = 0;
	se_shared_mem_mgmt->non_secure_mem.pos = 0;
}

static int se_dev_ctx_cpy_out_data(struct se_if_device_ctx *dev_ctx)
{
	struct se_shared_mem_mgmt_info *se_shared_mem_mgmt = &dev_ctx->se_shared_mem_mgmt;
	struct se_if_priv *priv = dev_ctx->priv;
	struct se_buf_desc *b_desc, *temp;
	bool do_cpy = true;

	list_for_each_entry_safe(b_desc, temp, &se_shared_mem_mgmt->pending_out, link) {
		if (b_desc->usr_buf_ptr && b_desc->shared_buf_ptr && do_cpy) {
			dev_dbg(priv->dev, "Copying output data to user.");
			if (do_cpy && copy_to_user(b_desc->usr_buf_ptr,
						   b_desc->shared_buf_ptr,
						   b_desc->size)) {
				dev_err(priv->dev, "Failure copying output data to user.");
				do_cpy = false;
			}
		}

		if (b_desc->shared_buf_ptr)
			memset(b_desc->shared_buf_ptr, 0, b_desc->size);

		list_del(&b_desc->link);
		kfree(b_desc);
	}

	return do_cpy ? 0 : -EFAULT;
}

/*
 * Clean the used Shared Memory space,
 * whether its Input Data copied from user buffers, or
 * Data received from FW.
 */
void se_dev_ctx_shared_mem_cleanup(struct se_if_device_ctx *dev_ctx)
{
	struct se_shared_mem_mgmt_info *se_shared_mem_mgmt = &dev_ctx->se_shared_mem_mgmt;
	struct list_head *pending_lists[] = {&se_shared_mem_mgmt->pending_in,
						&se_shared_mem_mgmt->pending_out};
	struct se_buf_desc *b_desc, *temp;
	bool is_fw_busy_dev_ctx;
	int i;

	/*
	 * If this context is the one that caused a firmware timeout the shared
	 * DMA buffers may still be actively read/written by the firmware.
	 */
	is_fw_busy_dev_ctx = se_is_fw_busy_ctx(dev_ctx);

	for (i = 0; i < ARRAY_SIZE(pending_lists); i++) {
		list_for_each_entry_safe(b_desc, temp, pending_lists[i], link) {
			if (!is_fw_busy_dev_ctx && b_desc->shared_buf_ptr)
				memset(b_desc->shared_buf_ptr, 0, b_desc->size);

			list_del(&b_desc->link);
			kfree(b_desc);
		}
	}

	/*
	 * Keep non_secure_mem.pos non-zero while this context still owns an
	 * outstanding firmware transaction. A non-zero pos is the marker that
	 * data is still staged for the enclave, which cleanup_se_shared_mem()
	 * uses to decide the buffer must be leaked rather than freed. Resetting
	 * it here would let a later teardown pass free a buffer the enclave may
	 * still be DMA-ing into.
	 */
	if (!is_fw_busy_dev_ctx)
		se_shared_mem_mgmt->non_secure_mem.pos = 0;
}

static struct se_buf_desc *add_b_desc_to_pending_list(void *shared_ptr_with_pos,
						      struct se_ioctl_setup_iobuf *io,
						      struct se_if_device_ctx *dev_ctx)
{
	struct se_shared_mem_mgmt_info *se_shared_mem_mgmt = &dev_ctx->se_shared_mem_mgmt;
	struct se_buf_desc *b_desc = NULL;

	b_desc = kzalloc_obj(*b_desc, GFP_KERNEL);
	if (!b_desc)
		return ERR_PTR(-ENOMEM);

	b_desc->shared_buf_ptr = shared_ptr_with_pos;
	b_desc->usr_buf_ptr = u64_to_user_ptr(io->user_buf);
	b_desc->size = io->length;

	if (io->flags & SE_IO_BUF_FLAGS_IS_INPUT) {
		/*
		 * buffer is input:
		 * add an entry in the "pending input buffers" list so
		 * that copied data can be cleaned from shared memory
		 * later.
		 */
		list_add_tail(&b_desc->link, &se_shared_mem_mgmt->pending_in);
	} else {
		/*
		 * buffer is output:
		 * add an entry in the "pending out buffers" list so data
		 * can be copied to user space when receiving Secure-Enclave
		 * response.
		 */
		list_add_tail(&b_desc->link, &se_shared_mem_mgmt->pending_out);
	}

	return b_desc;
}

static void se_if_open_gate_release(struct kref *kref)
{
	struct se_if_open_gate *gate =
		container_of(kref, struct se_if_open_gate, refcount);

	kfree(gate);
}

static bool se_if_open_gate_get(struct se_if_open_gate *gate)
{
	if (!gate)
		return false;

	return kref_get_unless_zero(&gate->refcount);
}

static void se_if_open_gate_put(struct se_if_open_gate *gate)
{
	if (gate)
		kref_put(&gate->refcount, se_if_open_gate_release);
}

/*
 * Distinct lockdep class for the internal priv_dev_ctx fops_lock. Taking it
 * while an open context's fops_lock is held (for example a firmware load
 * triggered from an ioctl) is valid hierarchical locking, but shares the same
 * class as the per-open fops_lock and would otherwise be misreported as
 * recursive locking by lockdep.
 */
static struct lock_class_key se_priv_ctx_fops_key;

static int init_misc_device_context(struct se_if_priv *priv, int ch_id,
				    struct se_if_device_ctx **new_dev_ctx,
				    const struct file_operations *se_if_fops)
{
	const char *err_str = "Failed to allocate memory";
	struct se_if_device_ctx *dev_ctx;
	struct se_if_open_gate *gate = NULL;
	int ret = -ENOMEM;

	dev_ctx = kzalloc_obj(*dev_ctx, GFP_KERNEL);

	if (!dev_ctx)
		return ret;

	dev_ctx->priv = priv;
	dev_ctx->devname = kasprintf(GFP_KERNEL, "%s0_ch%d",
				     get_se_if_name(priv->if_defs->se_if_type),
				     ch_id);
	if (!dev_ctx->devname)
		goto exit;

	mutex_init(&dev_ctx->fops_lock);
	lockdep_set_class(&dev_ctx->fops_lock, &se_priv_ctx_fops_key);

	kref_init(&dev_ctx->refcount);
	dev_ctx->cleanup_done = false;
	*new_dev_ctx = dev_ctx;
	set_se_rcv_msg_timeout(dev_ctx, SE_RCV_MSG_DEFAULT_TIMEOUT_MS);

	ret = init_se_shared_mem(dev_ctx);
	if (ret < 0)
		goto exit;

	gate = kzalloc_obj(*gate, GFP_KERNEL);
	if (!gate) {
		ret = -ENOMEM;
		goto exit;
	}

	mutex_init(&gate->lock);
	kref_init(&gate->refcount);    /* device-owned reference */
	gate->priv = priv;
	gate->dying = false;
	priv->open_gate = gate;

	/*
	 * The miscdevice storage is now owned by the open gate object.
	 * priv->priv_dev_ctx still keeps a pointer to that miscdevice.
	 */
	dev_ctx->miscdev = &gate->miscdev;

	dev_ctx->miscdev->name = dev_ctx->devname;
	dev_ctx->miscdev->minor = MISC_DYNAMIC_MINOR;
	dev_ctx->miscdev->fops = se_if_fops;
	dev_ctx->miscdev->parent = priv->dev;

	return 0;
exit:
	*new_dev_ctx = NULL;

	if (gate) {
		priv->open_gate = NULL;
		se_if_open_gate_put(gate);
	}
	cleanup_se_shared_mem(dev_ctx, true);
	kfree(dev_ctx->devname);
	kfree(dev_ctx);
	return dev_err_probe(priv->dev, ret, "%s", err_str);
}

static int se_if_request_channel(struct device *dev, struct mbox_chan **chan,
				 struct mbox_client *cl, const char *name)
{
	struct mbox_chan *t_chan;

	t_chan = mbox_request_channel_byname(cl, name);
	if (IS_ERR(t_chan))
		return dev_err_probe(dev, PTR_ERR(t_chan),
				     "Failed to request %s channel.", name);

	*chan = t_chan;

	return 0;
}

/*
 * Forward declarations. se_if_probe_cleanup() and se_if_probe() are kept
 * together as the teardown/probe pair, but several helpers, the file
 * operations table and the firmware-busy work handler they reference are
 * defined further down in this file.
 */
static void dlink_dev_ctx(struct se_if_device_ctx *dev_ctx);
static void cleanup_dev_ctx(struct se_if_device_ctx *dev_ctx, bool is_fclose);
static void se_clear_fw_busy(struct se_if_priv *priv);
static void se_if_dev_ctx_release(struct kref *kref);
static void se_if_priv_release(struct kref *kref);
static int se_if_misc_register(struct se_if_priv *priv);
static void se_fw_busy_work(struct work_struct *work);
static const struct file_operations se_if_fops;

static void se_if_probe_cleanup(void *plat_dev)
{
	struct platform_device *pdev = plat_dev;
	struct se_if_device_ctx *dev_ctx;
	struct device *dev = &pdev->dev;
	struct se_if_priv *priv;

	priv = dev_get_drvdata(dev);
	if (!priv)
		return;

	/*
	 * Announce teardown, then wake any in-flight waiter. going_away makes
	 * ele_msg_send_rcv() bail out instead of arming a new transaction and
	 * lets ele_msg_rcv() tell a teardown-forced completion apart from a
	 * real response; it must be set before complete_all().
	 *
	 * Set it under clbk_rx_lock, not se_if_cmd_lock: se_if_cmd_lock is held
	 * across the whole blocking transaction, so taking it here would stall
	 * unbind for a full receive-timeout. clbk_rx_lock is the short spinlock
	 * ele_msg_send_rcv() holds while arming, so this closes the lost-wakeup
	 * window - the sender either sees going_away and bails before arming, or
	 * armed first and this store (and complete_all()) is ordered after its
	 * reinit_completion() - and supplies the ordering the relaxed atomics do
	 * not.
	 */
	scoped_guard(spinlock_irqsave, &priv->waiting_rsp_clbk_hdl.clbk_rx_lock)
		atomic_set(&priv->going_away, 1);
	/*
	 * Wake the waiter before iterating the device-context list. It sleeps on
	 * this completion holding dev_ctx->fops_lock, which cleanup_dev_ctx()
	 * below also takes, so completing first avoids an unbind hang. Runs
	 * outside clbk_rx_lock; the going_away store above already orders it
	 * against the arming path.
	 */
	complete_all(&priv->waiting_rsp_clbk_hdl.done);

	/*
	 * Mark the private device context as cleanup_done first.
	 * This prevents new device contexts from being created in open().
	 */
	if (priv->priv_dev_ctx) {
		/*
		 * Mark cleanup_done under fops_lock so that se_if_fops_open(),
		 * which checks cleanup_done while holding fops_lock, cannot
		 * race past this and add a new device context after teardown.
		 */
		scoped_guard(mutex, &priv->priv_dev_ctx->fops_lock)
			priv->priv_dev_ctx->cleanup_done = true;

		if (priv->open_gate) {
			scoped_guard(mutex, &priv->open_gate->lock) {
				priv->open_gate->dying = true;
				priv->open_gate->priv = NULL;
			}
		}

		/*
		 * misc_register() is deferred to the end of probe, so the
		 * device may have a miscdev set up but never registered if
		 * probe failed before se_if_misc_register(). Only deregister
		 * when registration actually succeeded.
		 */
		if (priv->open_gate && priv->open_gate->registered &&
		    priv->priv_dev_ctx->miscdev)
			misc_deregister(priv->priv_dev_ctx->miscdev);
	}

	while (true) {
		dev_ctx = NULL;

		scoped_guard(mutex, &priv->modify_lock) {
			if (list_empty(&priv->dev_ctx_list))
				goto out_done;

			dev_ctx = list_first_entry(&priv->dev_ctx_list,
						   struct se_if_device_ctx, link);

			/* pin this context so close() cannot free it under us */
			kref_get(&dev_ctx->refcount);
			dlink_dev_ctx(dev_ctx);
		}

		/*
		 * Local cleanup outside the global lock avoids ABBA deadlock
		 * with paths that already take dev_ctx->fops_lock first.
		 */
		cleanup_dev_ctx(dev_ctx, false);
		kref_put(&dev_ctx->refcount, se_if_dev_ctx_release);
	}
out_done:

	/*
	 * Free the rx mailbox channel before cancelling fw_busy_work.
	 * se_if_rx_callback() runs from the rx channel and can schedule
	 * fw_busy_work when a late response arrives. If the channel were still
	 * live after cancel_work_sync(), a callback could re-arm the work and
	 * later dereference priv after it has been freed. Releasing the rx
	 * channel first guarantees no further callbacks, so the subsequent
	 * cancel_work_sync() is final.
	 */
	if (priv->rx_chan)
		mbox_free_channel(priv->rx_chan);
	if (priv->tx_chan)
		mbox_free_channel(priv->tx_chan);

	/*
	 * A timed-out synchronous command may have retained a dev_ctx through
	 * priv->fw_busy_dev_ctx even after the fd was closed and the context was
	 * removed from dev_ctx_list. If no late response arrived, release that
	 * retained context during driver teardown.
	 *
	 * se_clear_fw_busy() is idempotent and internally checks
	 * priv->fw_busy_dev_ctx under fw_busy_lock.
	 */
	se_clear_fw_busy(priv);
	cancel_work_sync(&priv->fw_busy_work);

	/*
	 * Being device managed buffer, no need to free the buffer allocated
	 * in se probe to store encrypted IMEM.
	 */

	dev_set_drvdata(dev, NULL);

	/* Drop the initial reference - priv will be freed when last fd closes */
	kref_put(&priv->refcount, se_if_priv_release);
}

static int se_if_probe(struct platform_device *pdev)
{
	const struct se_soc_info *se_info;
	const struct se_if_node *if_node;
	struct se_fw_load_info *load_fw;
	struct device *dev = &pdev->dev;
	struct se_if_priv *priv;
	int ret;

	if_node = device_get_match_data(dev);
	if (!if_node)
		return -EINVAL;

	se_info = if_node->se_info;

	priv = kzalloc_obj(*priv, GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	/*
	 * Pin the parent device for the lifetime of priv. A file descriptor may
	 * stay open after the device is unbound; close() then still passes
	 * priv->dev to dma_free_coherent()/dev_warn(). Without this reference
	 * the struct device could be freed while priv->dev still points at it,
	 * so the reference is dropped in se_if_priv_release() via put_device().
	 */
	get_device(priv->dev);
	kref_init(&priv->refcount);
	priv->if_defs = &if_node->if_defs;
	dev_set_drvdata(dev, priv);

	mutex_init(&priv->se_if_cmd_lock);
	mutex_init(&priv->modify_lock);
	spin_lock_init(&priv->cmd_receiver_clbk_hdl.clbk_rx_lock);
	spin_lock_init(&priv->waiting_rsp_clbk_hdl.clbk_rx_lock);
	atomic_set(&priv->fw_busy, 0);
	spin_lock_init(&priv->fw_busy_lock);
	priv->fw_busy_dev_ctx = NULL;
	INIT_WORK(&priv->fw_busy_work, se_fw_busy_work);

	init_completion(&priv->waiting_rsp_clbk_hdl.done);
	init_completion(&priv->cmd_receiver_clbk_hdl.done);
	INIT_LIST_HEAD(&priv->dev_ctx_list);

	ret = devm_add_action_or_reset(dev, se_if_probe_cleanup, pdev);
	if (ret)
		return ret;

	/* Mailbox client configuration */
	priv->se_mb_cl.dev		= dev;
	priv->se_mb_cl.tx_block		= false;
	priv->se_mb_cl.knows_txdone	= false;
	priv->se_mb_cl.rx_callback	= se_if_rx_callback;

	ret = se_if_request_channel(dev, &priv->tx_chan, &priv->se_mb_cl, MBOX_TX_NAME);
	if (ret)
		return ret;

	ret = se_if_request_channel(dev, &priv->rx_chan, &priv->se_mb_cl, MBOX_RX_NAME);
	if (ret)
		return ret;

	if (if_node->pool_name) {
		priv->mem_pool = of_gen_pool_get(dev->of_node, if_node->pool_name, 0);
		if (!priv->mem_pool)
			return dev_err_probe(dev, -ENOMEM,
					     "Unable to get sram pool = %s.",
					     if_node->pool_name);
	}

	if (if_node->reserved_dma_ranges) {
		ret = of_reserved_mem_device_init(dev);
		if (ret)
			return dev_err_probe(dev, ret,
					    "Failed to init reserved memory region.");
	}

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "Failed to set 32-bit coherent DMA mask.");

	/*
	 * Initialize load_fw_lock before registering the misc device.
	 * A userspace process could open the device and trigger se_load_firmware()
	 * via IOCTL immediately after misc_register(), so the mutex must be ready
	 * before the device becomes visible.
	 */
	if (se_info->se_fw_img_nm.seco_fw_nm_in_rfs) {
		load_fw = get_load_fw_instance(priv);
		mutex_init(&load_fw->load_fw_lock);
		load_fw->se_fw_img_nm = &se_info->se_fw_img_nm;
		load_fw->is_fw_tobe_loaded = true;
	}

	/* By default, there is no pending FW to be loaded.*/
	if (se_info->imem_state_mgmt) {
		load_fw = get_load_fw_instance(priv);

		/* allocate buffer where SE store encrypted IMEM */
		load_fw->imem.buf = dmam_alloc_coherent(priv->dev, ELE_IMEM_SIZE,
							&load_fw->imem.daddr,
							GFP_KERNEL);
		if (!load_fw->imem.buf)
			return dev_err_probe(dev, -ENOMEM,
					     "dmam-alloc-failed: To store encr-IMEM.");
		load_fw->imem_mgmt = true;
	}

	ret = init_misc_device_context(priv, 0, &priv->priv_dev_ctx, &se_if_fops);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed[0x%x] to create device contexts.",
				     ret);

	if (if_node->if_defs.se_if_type == SE_TYPE_ID_HSM) {
		ret = get_se_soc_info(priv, se_info);
		if (ret)
			return dev_err_probe(dev, ret, "Failed to fetch SoC Info.");
	}

	/*
	 * All probe-time initialization is complete; expose the
	 * interface to userspace last so that an open()/ioctl cannot
	 * race against a not-yet-initialized device.
	 */
	ret = se_if_misc_register(priv);
	if (ret)
		return ret;

	dev_info(dev, "i.MX secure-enclave: %s0 interface to firmware, configured.",
		 get_se_if_name(priv->if_defs->se_if_type));

	return ret;
}

/*
 * Expose the interface to userspace. Deferred until the end of probe so
 * the device node only becomes openable after SoC info has been fetched
 * and, on SoCs with IMEM management, the encrypted-IMEM buffer has been
 * allocated. This prevents userspace from opening the node and issuing
 * commands against a partially initialized interface.
 */
static int se_if_misc_register(struct se_if_priv *priv)
{
	int ret;

	ret = misc_register(priv->priv_dev_ctx->miscdev);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "Failed to register misc device.");

	priv->open_gate->registered = true;

	return 0;
}

static void se_if_priv_release(struct kref *kref)
{
	struct se_if_priv *priv = container_of(kref, struct se_if_priv, refcount);

	/* Free priv_dev_ctx if it exists */
	if (priv->priv_dev_ctx) {
		/*
		 * miscdev storage belongs to open_gate, not directly to
		 * priv_dev_ctx. The gate should already have been detached
		 * from priv during teardown.
		 *
		 * Reclaim the internal context's shared memory directly here
		 * instead of through cleanup_dev_ctx(). Teardown already set
		 * cleanup_done on priv_dev_ctx, so cleanup_dev_ctx() would
		 * short-circuit and leak the host descriptors and the coherent
		 * buffer. By this point the device is fully unbound; if this
		 * context ever armed the firmware-busy breaker, se_clear_fw_busy()
		 * has already run with reclaim=false and freed the host
		 * descriptors, emptied the pool list and cleared
		 * non_secure_mem.ptr. A reclaim=true pass here is therefore both
		 * safe and idempotent: it releases the buffers for a normal
		 * context and is a no-op for the abandoned firmware-busy one.
		 */
		scoped_guard(mutex, &priv->priv_dev_ctx->fops_lock)
			cleanup_se_shared_mem(priv->priv_dev_ctx, true);

		kfree(priv->priv_dev_ctx->devname);
		kfree(priv->priv_dev_ctx);
		priv->priv_dev_ctx = NULL;
	}
	/*
	 * No need to check, if reserved memory is allocated
	 * before calling for its release. Or clearing the
	 * un-set bit.
	 */
	of_reserved_mem_device_release(priv->dev);

	/*
	 * Be defensive: if teardown did not already drop the device-owned
	 * gate reference for some reason, release it here.
	 */
	if (priv->open_gate) {
		se_if_open_gate_put(priv->open_gate);
		priv->open_gate = NULL;
	}

	/*
	 * Drop the reference on priv->dev taken in se_if_probe(). The device was
	 * pinned so that a file descriptor closed after device unbind can still
	 * safely pass priv->dev to dma_free_coherent()/dev_warn().
	 */
	put_device(priv->dev);

	/* Free any remaining resources that weren't devm-managed */
	kfree(priv);
}

static void se_if_dev_ctx_release(struct kref *kref)
{
	struct se_if_device_ctx *dev_ctx =
		container_of(kref, struct se_if_device_ctx, refcount);
	struct se_if_priv *priv = dev_ctx->priv;

	kfree(dev_ctx);

	/* drop the priv reference owned by this device context */
	kref_put(&priv->refcount, se_if_priv_release);
}

static void se_clear_fw_busy(struct se_if_priv *priv)
{
	struct se_if_device_ctx *dev_ctx = NULL;
	unsigned long flags;

	spin_lock_irqsave(&priv->fw_busy_lock, flags);
	dev_ctx = priv->fw_busy_dev_ctx;
	priv->fw_busy_dev_ctx = NULL;
	atomic_set(&priv->fw_busy, 0);
	spin_unlock_irqrestore(&priv->fw_busy_lock, flags);

	if (!dev_ctx)
		return;

	/*
	 * The circuit breaker is cleared from two places, which need opposite
	 * memory-reclaim policies:
	 *
	 *   1. se_fw_busy_work(): a late firmware response actually arrived.
	 *      going_away is not set and the enclave has finished with the
	 *      buffer, so a full reclaim (reclaim=true) is safe. Only do this
	 *      once the owning fd has been closed (cleanup_done); while the fd
	 *      is still open the buffer belongs to that context and is released
	 *      on its normal close path.
	 *
	 *   2. se_if_probe_cleanup(): teardown. going_away is set and no
	 *      response has been confirmed, so the enclave may still be
	 *      DMA-writing into the shared buffer. Freeing it here would be a
	 *      DMA-after-free. Pass reclaim=false so cleanup_se_shared_mem()
	 *      frees only the host-side descriptors and deliberately leaks the
	 *      DMA buffer that the enclave might still touch.
	 */
	scoped_guard(mutex, &dev_ctx->fops_lock) {
		if (atomic_read(&priv->going_away)) {
			/*
			 * Fatal, but deliberately non-panic: the enclave is
			 * unresponsive at unbind with a transaction still in
			 * flight. Both the coherent staging buffer and any
			 * gen_pool buffers this context owns are abandoned
			 * (host descriptors freed, DMA-visible memory leaked)
			 * to avoid a DMA-after-free while the enclave may still
			 * be writing. Emit one headline error here rather than
			 * per-buffer so the count of faulted contexts is clear.
			 * Do not use WARN/BUG: this path is recoverable and
			 * panic_on_warn kernels must not be brought down by it.
			 */
			dev_err(priv->dev,
				"%s: FATAL: enclave stuck at unbind, DMA leaked.\n",
				dev_ctx->devname);
			cleanup_se_shared_mem(dev_ctx, false);
		} else if (dev_ctx->cleanup_done) {
			cleanup_se_shared_mem(dev_ctx, true);
		}
	}

	kref_put(&dev_ctx->refcount, se_if_dev_ctx_release);
}

void unset_dev_ctx_as_command_receiver(struct se_if_device_ctx *dev_ctx)
{
	struct se_if_priv *priv = dev_ctx->priv;
	struct se_api_msg *old_rx_msg = NULL;
	struct se_clbk_handle *se_clbk_hdl;
	unsigned long flags;

	lockdep_assert_held(&priv->modify_lock);

	se_clbk_hdl = &priv->cmd_receiver_clbk_hdl;

	if (se_clbk_hdl->dev_ctx == dev_ctx) {
		spin_lock_irqsave(&se_clbk_hdl->clbk_rx_lock, flags);
		old_rx_msg = se_clbk_hdl->rx_msg;
		se_clbk_hdl->dev_ctx = NULL;
		se_clbk_hdl->rx_msg = NULL;
		se_clbk_hdl->rx_msg_sz = 0;
		spin_unlock_irqrestore(&se_clbk_hdl->clbk_rx_lock, flags);

		kfree(old_rx_msg);
		complete_all(&se_clbk_hdl->done);
	}
}

int set_dev_ctx_as_command_receiver(struct se_if_device_ctx *dev_ctx)
{
	struct se_if_priv *priv = dev_ctx->priv;
	struct se_api_msg *new_rx_msg = NULL;
	struct se_clbk_handle *se_clbk_hdl;
	unsigned long flags;

	se_clbk_hdl = &priv->cmd_receiver_clbk_hdl;
	guard(mutex)(&priv->modify_lock);
	if (se_clbk_hdl->dev_ctx == dev_ctx)
		return 0;

	if (se_clbk_hdl->dev_ctx)
		return -EBUSY;

	if (!se_clbk_hdl->rx_msg) {
		new_rx_msg = kzalloc(MAX_NVM_MSG_LEN, GFP_KERNEL);
		if (!new_rx_msg)
			return -ENOMEM;
	}
	spin_lock_irqsave(&se_clbk_hdl->clbk_rx_lock, flags);
	if (new_rx_msg)
		se_clbk_hdl->rx_msg = new_rx_msg;
	reinit_completion(&se_clbk_hdl->done);
	se_clbk_hdl->rx_msg_sz = MAX_NVM_MSG_LEN;
	se_clbk_hdl->dev_ctx = dev_ctx;
	dev_ctx->rcv_msg_timeout_jiffies = MAX_SCHEDULE_TIMEOUT;
	spin_unlock_irqrestore(&se_clbk_hdl->clbk_rx_lock, flags);

	return 0;
}

static void dlink_dev_ctx(struct se_if_device_ctx *dev_ctx)
{
	struct se_if_priv *priv = dev_ctx->priv;

	unset_dev_ctx_as_command_receiver(dev_ctx);

	if (!list_empty(&dev_ctx->link)) {
		list_del_init(&dev_ctx->link);
		priv->active_devctx_count--;
	}
}

bool se_is_fw_busy_ctx(struct se_if_device_ctx *dev_ctx)
{
	struct se_if_priv *priv = dev_ctx->priv;
	unsigned long flags;
	bool match;

	spin_lock_irqsave(&priv->fw_busy_lock, flags);
	match = priv->fw_busy_dev_ctx == dev_ctx;
	spin_unlock_irqrestore(&priv->fw_busy_lock, flags);

	return match;
}

static void cleanup_dev_ctx(struct se_if_device_ctx *dev_ctx, bool is_fclose)
{
	bool already_done;

	scoped_guard(mutex, &dev_ctx->fops_lock) {
		already_done = dev_ctx->cleanup_done;
		if (!already_done) {
			/*
			 * Ask FW to drop this context's session and storage so
			 * the kernel and FW stay in sync. Done here, under this
			 * context's fops_lock only (not the global modify_lock),
			 * because both close requests block on a firmware
			 * round-trip; issuing them while modify_lock was held
			 * would stall every other context for the FW timeout.
			 *
			 * Skip the round-trips once the FW path is marked busy.
			 * fw_busy is armed when a synchronous transaction times
			 * out; while it is set ele_msg_send_rcv() rejects further
			 * commands with -EBUSY without waiting. It is only cleared
			 * by se_clear_fw_busy(), which during unbind runs once
			 * after this loop (or earlier from fw_busy_work only if a
			 * genuine late FW response arrives). On a hung FW no late
			 * response comes, so the breaker stays set for the rest of
			 * the loop and the remaining closes would just return
			 * -EBUSY and log spurious "failed to close" errors. Skip
			 * them and emit a single warning instead.
			 */
			if (atomic_read(&dev_ctx->priv->fw_busy)) {
				if (dev_ctx->strg_hdl || dev_ctx->sess_hdl)
					dev_warn(dev_ctx->priv->dev,
						 "%s: skipping session/storage close, FW is busy\n",
						 dev_ctx->devname);
			} else {
				if (dev_ctx->strg_hdl && se_close_storage(dev_ctx->priv,
									  dev_ctx->strg_hdl))
					dev_err(dev_ctx->priv->dev, "failed to close storage.\n");
				if (dev_ctx->sess_hdl && se_close_session(dev_ctx->priv,
									  dev_ctx->sess_hdl))
					dev_err(dev_ctx->priv->dev, "failed to close session.\n");
			}
			/*
			 * fw_busy is caused by one timed-out synchronous transaction.
			 * Only that transaction's dev_ctx may still have coherent
			 * memory referenced by FW. Do not skip cleanup for unrelated
			 * contexts while fw_busy is set.
			 */
			if (se_is_fw_busy_ctx(dev_ctx))
				dev_warn(dev_ctx->priv->dev,
					 "%s: deferring shared memory cleanup while FW is busy\n",
					 dev_ctx->devname);
			else
				cleanup_se_shared_mem(dev_ctx, true);

			kfree(dev_ctx->devname);
			dev_ctx->devname = NULL;
			dev_ctx->cleanup_done = true;
		}
	}

	if (is_fclose)
		kref_put(&dev_ctx->refcount, se_if_dev_ctx_release);
}

static void dlink_n_cleanup_dev_ctx(struct se_if_device_ctx *dev_ctx, bool is_fclose)
{
	struct se_if_priv *priv = dev_ctx->priv;

	if (is_fclose) {
		scoped_guard(mutex, &priv->modify_lock)
			dlink_dev_ctx(dev_ctx);
	}

	cleanup_dev_ctx(dev_ctx, is_fclose);
}

static int init_device_context(struct se_if_priv *priv, int ch_id,
			       struct se_if_device_ctx **new_dev_ctx)
{
	struct se_if_device_ctx *dev_ctx;
	int ret = 0;

	dev_ctx = kzalloc_obj(*dev_ctx, GFP_KERNEL);

	if (!dev_ctx)
		return -ENOMEM;

	dev_ctx->devname = kasprintf(GFP_KERNEL, "%s0_ch%d",
				     get_se_if_name(priv->if_defs->se_if_type),
				     ch_id);
	if (!dev_ctx->devname) {
		kfree(dev_ctx);
		return -ENOMEM;
	}

	mutex_init(&dev_ctx->fops_lock);
	kref_init(&dev_ctx->refcount);
	dev_ctx->priv = priv;
	dev_ctx->cleanup_done = false;
	INIT_LIST_HEAD(&dev_ctx->link);
	set_se_rcv_msg_timeout(dev_ctx, SE_RCV_MSG_LONG_TIMEOUT_MS);
	*new_dev_ctx = dev_ctx;

	ret = init_se_shared_mem(dev_ctx);
	if (ret < 0) {
		kfree(dev_ctx->devname);
		kfree(dev_ctx);
		*new_dev_ctx = NULL;

		return ret;
	}

	/* Take a reference to priv for this device context */
	kref_get(&priv->refcount);

	scoped_guard(mutex, &priv->modify_lock) {
		list_add_tail(&dev_ctx->link, &priv->dev_ctx_list);
		priv->active_devctx_count++;
	}

	return ret;
}

static int se_ioctl_cmd_snd_rcv_cleanup(struct se_if_device_ctx *dev_ctx, void __user *uarg,
					struct se_ioctl_cmd_snd_rcv_rsp_info *cmd_snd_rcv_rsp_info)
{
	/* shared memory is allocated before this IOCTL */
	se_dev_ctx_shared_mem_cleanup(dev_ctx);

	if (cmd_snd_rcv_rsp_info->rx_buf_sz &&
	    copy_to_user(uarg, cmd_snd_rcv_rsp_info, sizeof(*cmd_snd_rcv_rsp_info))) {
		dev_err(dev_ctx->priv->dev, "%s: Failed to copy cmd_snd_rcv_rsp_info to user.",
			dev_ctx->devname);
		return -EFAULT;
	}

	return 0;
}

static int se_ioctl_cmd_snd_rcv_rsp_handler(struct se_if_device_ctx *dev_ctx,
					    void __user *uarg)
{
	struct se_ioctl_cmd_snd_rcv_rsp_info cmd_snd_rcv_rsp_info = {0};
	struct se_if_priv *priv = dev_ctx->priv;
	int rsp_status_err = 0;
	int cleanup_err = 0;
	int err = 0;

	if (copy_from_user(&cmd_snd_rcv_rsp_info, uarg,
			   sizeof(cmd_snd_rcv_rsp_info))) {
		dev_err(priv->dev,
			"%s: Failed to copy cmd_snd_rcv_rsp_info from user.",
			dev_ctx->devname);
		se_ioctl_cmd_snd_rcv_cleanup(dev_ctx, uarg, &cmd_snd_rcv_rsp_info);
		return -EFAULT;
	}

	if (cmd_snd_rcv_rsp_info.tx_buf_sz < SE_MU_HDR_SZ ||
	    cmd_snd_rcv_rsp_info.tx_buf_sz > MAX_ALLOWED_TX_MSG_SZ) {
		dev_err(priv->dev, "%s: User buffer too small/large(%d < %d)",
			dev_ctx->devname, cmd_snd_rcv_rsp_info.tx_buf_sz,
			cmd_snd_rcv_rsp_info.tx_buf_sz < SE_MU_HDR_SZ ? SE_MU_HDR_SZ :
								MAX_ALLOWED_TX_MSG_SZ);
		se_ioctl_cmd_snd_rcv_cleanup(dev_ctx, uarg, &cmd_snd_rcv_rsp_info);
		return -ENOSPC;
	}

	struct se_api_msg *tx_msg __free(kfree) =
		memdup_user(u64_to_user_ptr(cmd_snd_rcv_rsp_info.tx_buf),
			    cmd_snd_rcv_rsp_info.tx_buf_sz);
	if (IS_ERR(tx_msg)) {
		err = PTR_ERR(tx_msg);
		se_ioctl_cmd_snd_rcv_cleanup(dev_ctx, uarg, &cmd_snd_rcv_rsp_info);
		return err;
	}

	err = se_chk_tx_msg_hdr(dev_ctx, &tx_msg->header);
	if (err) {
		se_ioctl_cmd_snd_rcv_cleanup(dev_ctx, uarg, &cmd_snd_rcv_rsp_info);
		return err;
	}

	if (cmd_snd_rcv_rsp_info.rx_buf_sz < SE_MU_HDR_SZ ||
	    cmd_snd_rcv_rsp_info.rx_buf_sz > MAX_ALLOWED_RX_MSG_SZ) {
		se_ioctl_cmd_snd_rcv_cleanup(dev_ctx, uarg, &cmd_snd_rcv_rsp_info);
		return -EINVAL;
	}

	if (tx_msg->header.tag != priv->if_defs->cmd_tag) {
		se_ioctl_cmd_snd_rcv_cleanup(dev_ctx, uarg, &cmd_snd_rcv_rsp_info);
		return -EINVAL;
	}

	if (tx_msg->header.ver == priv->if_defs->fw_api_ver &&
	    get_load_fw_instance(priv)->is_fw_tobe_loaded) {
		err = se_load_firmware(priv);
		if (err) {
			dev_err(priv->dev, "Could not send msg as FW is not loaded.");
			se_ioctl_cmd_snd_rcv_cleanup(dev_ctx, uarg, &cmd_snd_rcv_rsp_info);
			return -EPERM;
		}
	}

	struct se_api_msg *rx_msg __free(kfree) =
		kzalloc(cmd_snd_rcv_rsp_info.rx_buf_sz, GFP_KERNEL);
	if (!rx_msg) {
		se_ioctl_cmd_snd_rcv_cleanup(dev_ctx, uarg, &cmd_snd_rcv_rsp_info);
		return -ENOMEM;
	}

	err = ele_msg_send_rcv(dev_ctx, tx_msg, cmd_snd_rcv_rsp_info.tx_buf_sz,
			       rx_msg, cmd_snd_rcv_rsp_info.rx_buf_sz);
	if (err < 0) {
		se_ioctl_cmd_snd_rcv_cleanup(dev_ctx, uarg, &cmd_snd_rcv_rsp_info);

		/*
		 * -ERESTARTSYS here means the wait was interrupted by a signal
		 * after the command had already been handed to (and possibly
		 * executed by) the firmware. Returning -ERESTARTSYS lets the VFS
		 * transparently restart the ioctl, which would re-run the command
		 * with the just cleaned-up (zeroed) shared input buffers. Report
		 * -EINTR instead so the syscall is not auto-restarted; userspace
		 * can decide whether to reissue it.
		 */
		if (err == -ERESTARTSYS)
			err = -EINTR;

		return err;
	}

	/*
	 * ele_msg_send_rcv() returns a positive received-message size on
	 * success. Returning that raw size as the ioctl result would make a
	 * successful transaction look like a positive (non-zero) return value
	 * to userspace. Record the actual received size in rx_buf_sz for the
	 * response copied back to userspace, then normalise err to 0 so the
	 * ioctl reports plain success; the firmware status is conveyed to
	 * userspace inside the response buffer itself.
	 */
	cmd_snd_rcv_rsp_info.rx_buf_sz = err;
	err = 0;

	dev_dbg(priv->dev, "%s: %s %s.", dev_ctx->devname, __func__,
		"message received, start transmit to user");

	rsp_status_err =
		se_val_rsp_hdr_n_status(priv, rx_msg, tx_msg->header.command,
					cmd_snd_rcv_rsp_info.rx_buf_sz,
					tx_msg->header.ver == priv->if_defs->base_api_ver);

	if (!rsp_status_err) {
		err = se_dev_ctx_cpy_out_data(dev_ctx);
		if (err < 0) {
			se_ioctl_cmd_snd_rcv_cleanup(dev_ctx, uarg, &cmd_snd_rcv_rsp_info);
			return err;
		}
	}

	/* Copy data from the buffer */
	print_hex_dump_debug("to user ", DUMP_PREFIX_OFFSET, 4, 4, rx_msg,
			     cmd_snd_rcv_rsp_info.rx_buf_sz, false);

	if (copy_to_user(u64_to_user_ptr(cmd_snd_rcv_rsp_info.rx_buf), rx_msg,
			 cmd_snd_rcv_rsp_info.rx_buf_sz)) {
		dev_err(priv->dev, "%s: Failed to copy to user.", dev_ctx->devname);
		err = -EFAULT;
	}

	cleanup_err = se_ioctl_cmd_snd_rcv_cleanup(dev_ctx, uarg, &cmd_snd_rcv_rsp_info);

	if (cleanup_err && !err)
		err = cleanup_err;

	if (!err && !rsp_status_err)
		fw_api_specific_ops(dev_ctx, rx_msg);

	return err;
}

static int se_ioctl_get_mu_info(struct se_if_device_ctx *dev_ctx,
				void __user *uarg)
{
	struct se_if_priv *priv = dev_ctx->priv;
	struct se_ioctl_get_if_info if_info;
	struct se_if_node *if_node;
	int err = 0;

	if_node = container_of(priv->if_defs, typeof(*if_node), if_defs);

	if_info.se_if_id = 0;
	if_info.interrupt_idx = 0;
	if_info.tz = 0;
	if_info.did = 0;
	if_info.cmd_tag = priv->if_defs->cmd_tag;
	if_info.rsp_tag = priv->if_defs->rsp_tag;
	if_info.success_tag = priv->if_defs->success_tag;
	if_info.base_api_ver = priv->if_defs->base_api_ver;
	if_info.fw_api_ver = priv->if_defs->fw_api_ver;

	dev_dbg(priv->dev, "%s: info [se_if_id: %d, irq_idx: %d, tz: 0x%x, did: 0x%x].",
		dev_ctx->devname, if_info.se_if_id, if_info.interrupt_idx, if_info.tz,
		if_info.did);

	if (copy_to_user(uarg, &if_info, sizeof(if_info))) {
		dev_err(priv->dev, "%s: Failed to copy mu info to user.",
			dev_ctx->devname);
		err = -EFAULT;
	}

	return err;
}

static void rollback_shared_mem_pos(struct se_if_device_ctx *dev_ctx, u32 length)
{
	struct se_shared_mem *shared_mem = NULL;

	shared_mem = &dev_ctx->se_shared_mem_mgmt.non_secure_mem;

	if (WARN_ON_ONCE(length > shared_mem->pos)) {
		shared_mem->pos = 0;
		return;
	}

	shared_mem->pos -= length;
}

int get_shared_mem_slot(struct se_if_device_ctx *dev_ctx,
			u32 *length, dma_addr_t *ele_dma_addr, void **ptr)
{
	struct se_shared_mem *shared_mem = NULL;
	bool is_fw_busy_dev_ctx;
	size_t aligned_len = 0;
	u32 pos;

	/*
	 * If this context is the one that caused a firmware timeout the shared
	 * DMA buffers may still be actively read/written by the firmware.
	 */
	is_fw_busy_dev_ctx = se_is_fw_busy_ctx(dev_ctx);
	if (is_fw_busy_dev_ctx)
		return -EBUSY;

	aligned_len = round_up((size_t)*length, 8);
	if (aligned_len < *length) {
		dev_err(dev_ctx->priv->dev, "%s: Invalid buffer length.",
			dev_ctx->devname);
		return -EINVAL;
	}

	/* No specific requirement for this buffer. */
	shared_mem = &dev_ctx->se_shared_mem_mgmt.non_secure_mem;

	/* Check there is enough space in the shared memory. */
	dev_dbg(dev_ctx->priv->dev, "%s: req_size = %zd, max_size= %d, curr_pos = %d",
		dev_ctx->devname, aligned_len, shared_mem->size,
		shared_mem->pos);

	if (shared_mem->size < shared_mem->pos ||
	    aligned_len > (shared_mem->size - shared_mem->pos)) {
		dev_err(dev_ctx->priv->dev, "%s: Not enough space in shared memory.",
			dev_ctx->devname);
		return -ENOMEM;
	}

	/* Allocate space in shared memory. 8 bytes aligned. */
	pos = shared_mem->pos;
	shared_mem->pos += aligned_len;
	*ele_dma_addr = (u64)shared_mem->dma_addr + pos;
	*ptr = shared_mem->ptr + pos;
	*length = aligned_len;

	memset(shared_mem->ptr + pos, 0, aligned_len);

	return 0;
}

/*
 * Copy a buffer of data to/from the user and return the address to use in
 * messages
 */
static int se_ioctl_setup_iobuf_handler(struct se_if_device_ctx *dev_ctx,
					void __user *uarg)
{
	struct se_ioctl_setup_iobuf io = {0};
	struct se_buf_desc *b_desc = NULL;
	void *dma_buf_ptr = NULL;
	dma_addr_t ele_dma_addr;
	u32 aligned_len = 0;
	int err = 0;

	if (copy_from_user(&io, uarg, sizeof(io))) {
		dev_err(dev_ctx->priv->dev, "%s: Failed copy iobuf config from user.",
			dev_ctx->devname);
		return -EFAULT;
	}

	dev_dbg(dev_ctx->priv->dev, "%s: io [buf: %p(%d) flag: %x].", dev_ctx->devname,
		u64_to_user_ptr(io.user_buf), io.length, io.flags);

	if (io.length == 0 || !io.user_buf) {
		/*
		 * Accept NULL pointers since some buffers are optional
		 * in FW commands. In this case we should return 0 as
		 * pointer to be embedded into the message.
		 * Skip all data copy part of code below.
		 */
		io.ele_addr = 0;
		goto copy;
	}

	aligned_len = io.length;
	err = get_shared_mem_slot(dev_ctx, &aligned_len, &ele_dma_addr, &dma_buf_ptr);
	if (err)
		return err;

	io.ele_addr = ele_dma_addr;
	if ((io.flags & SE_IO_BUF_FLAGS_IS_INPUT) ||
	    (io.flags & SE_IO_BUF_FLAGS_IS_IN_OUT)) {
		/*
		 * buffer is input:
		 * copy data from user space to this allocated buffer.
		 */
		if (copy_from_user(dma_buf_ptr, u64_to_user_ptr(io.user_buf),
				   io.length)) {
			dev_err(dev_ctx->priv->dev,
				"%s: Failed copy data to shared memory.",
				dev_ctx->devname);
			err = -EFAULT;
			goto rollback;
		}
	}

	b_desc = add_b_desc_to_pending_list(dma_buf_ptr, &io, dev_ctx);
	if (IS_ERR(b_desc)) {
		err = PTR_ERR(b_desc);
		dev_err(dev_ctx->priv->dev, "%s: Failed to allocate/link b_desc.",
			dev_ctx->devname);
		goto rollback;
	}

copy:
	/* Provide the EdgeLock Enclave address to user space only if success.*/
	if (copy_to_user(uarg, &io, sizeof(io))) {
		dev_err(dev_ctx->priv->dev, "%s: Failed to copy iobuff setup to user.",
			dev_ctx->devname);
		err = -EFAULT;
		goto rollback;
	}
	return err;

rollback:
	if (!IS_ERR_OR_NULL(b_desc)) {
		list_del(&b_desc->link);
		kfree(b_desc);
	}

	if (dma_buf_ptr && aligned_len) {
		memset(dma_buf_ptr, 0, aligned_len);
		rollback_shared_mem_pos(dev_ctx, aligned_len);
	}

	return err;
}

/* IOCTL to provide SoC information */
static int se_ioctl_get_se_soc_info_handler(struct se_if_device_ctx *dev_ctx,
					    void __user *uarg)
{
	struct se_ioctl_get_soc_info soc_info;
	int err = -EINVAL;

	soc_info.soc_id = get_se_soc_id(dev_ctx->priv);
	soc_info.soc_rev = var_se_info.soc_rev;

	err = copy_to_user(uarg, (u8 *)(&soc_info), sizeof(soc_info));
	if (err) {
		dev_err(dev_ctx->priv->dev, "%s: Failed to copy soc info to user.",
			dev_ctx->devname);
		err = -EFAULT;
	}

	return err;
}

/*
 * File operations for user-space
 */

/* Write a message to the MU. */
static ssize_t se_if_fops_write(struct file *fp, const char __user *buf,
				size_t size, loff_t *ppos)
{
	struct se_if_device_ctx *dev_ctx = fp->private_data;
	struct se_if_priv *priv;
	int err;

	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &dev_ctx->fops_lock) {
		if (dev_ctx->cleanup_done)
			return -ENODEV;

		priv = dev_ctx->priv;

		dev_dbg(priv->dev, "%s: write from buf (%p)%zu, ppos=%lld.", dev_ctx->devname,
			buf, size, ((ppos) ? *ppos : 0));

		if (dev_ctx != priv->cmd_receiver_clbk_hdl.dev_ctx) {
			se_dev_ctx_shared_mem_cleanup(dev_ctx);
			return -EINVAL;
		}

		if (size < SE_MU_HDR_SZ || size > MAX_ALLOWED_TX_MSG_SZ) {
			dev_err(priv->dev, "%s: User buffer too small/large(%zu < %d)",
				dev_ctx->devname, size,
				size < SE_MU_HDR_SZ ? SE_MU_HDR_SZ :
								MAX_ALLOWED_TX_MSG_SZ);
			return -ENOSPC;
		}

		struct se_api_msg *tx_msg __free(kfree) = memdup_user(buf, size);
		if (IS_ERR(tx_msg))
			return PTR_ERR(tx_msg);

		err = se_chk_tx_msg_hdr(dev_ctx, &tx_msg->header);
		if (err)
			return err;

		print_hex_dump_debug("from user ", DUMP_PREFIX_OFFSET, 4, 4,
				     tx_msg, size, false);

		err = ele_msg_send(dev_ctx, tx_msg, size);

		return err;
	}
}

/*
 * Read a message from the MU.
 * Blocking until a message is available.
 */
static ssize_t se_if_fops_read(struct file *fp, char __user *buf, size_t size,
			       loff_t *ppos)
{
	struct se_if_device_ctx *dev_ctx = fp->private_data;
	u8 rx_msg_snap[MAX_NVM_MSG_LEN];
	struct se_if_priv *priv;
	unsigned long flags;
	size_t copy_len;
	int err;

	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &dev_ctx->fops_lock) {
		priv = dev_ctx->priv;

		if (dev_ctx->cleanup_done)
			return -ENODEV;

		dev_dbg(priv->dev, "%s: read to buf %p(%zu), ppos=%lld.", dev_ctx->devname,
			buf, size, ((ppos) ? *ppos : 0));

		mutex_lock(&priv->modify_lock);
		if (dev_ctx != priv->cmd_receiver_clbk_hdl.dev_ctx) {
			mutex_unlock(&priv->modify_lock);
			se_dev_ctx_shared_mem_cleanup(dev_ctx);
			return -EINVAL;
		}
		mutex_unlock(&priv->modify_lock);
	}

	err = ele_msg_rcv(dev_ctx, &priv->cmd_receiver_clbk_hdl);
	if (err < 0) {
		if (err != -ERESTARTSYS)
			dev_err(priv->dev,
				"%s: Er[0x%x]: Signal Interrupted. Current act-dev-ctx count: %d.",
				dev_ctx->devname, err, dev_ctx->priv->active_devctx_count);
		return err;
	}

	/*
	 * Reacquire fops_lock before touching any dev_ctx state (pending lists,
	 * rx_msg) after the blocking wait. fops_lock was dropped before calling
	 * ele_msg_rcv(). If cleanup_dev_ctx() ran concurrently it could have
	 * freed the DMA buffers and the pending lists, leading to UAF and list
	 * corruption. Re-checking cleanup_done under fops_lock prevents that.
	 */
	mutex_lock(&dev_ctx->fops_lock);

	if (dev_ctx->cleanup_done) {
		mutex_unlock(&dev_ctx->fops_lock);
		return -ENODEV;
	}

	/*
	 * Snapshot rx_msg pointer under clbk_rx_lock before releasing it.
	 * unset_dev_ctx_as_command_receiver() can acquire the lock, NULL out
	 * rx_msg, and free the buffer at any time after the unlock; using a
	 * stale pointer from the shared field after the unlock is a UAF.
	 */
	scoped_guard(mutex, &priv->modify_lock) {
		spin_lock_irqsave(&priv->cmd_receiver_clbk_hdl.clbk_rx_lock, flags);
		if (priv->cmd_receiver_clbk_hdl.dev_ctx != dev_ctx ||
		    !priv->cmd_receiver_clbk_hdl.rx_msg ||
		    !priv->cmd_receiver_clbk_hdl.rx_msg_sz) {
			spin_unlock_irqrestore(&priv->cmd_receiver_clbk_hdl.clbk_rx_lock, flags);
			mutex_unlock(&dev_ctx->fops_lock);
			return -ENODEV;
		}
		/* Taking snapshot is enough for the one common pre-allocated buffer. */
		copy_len = min(size, priv->cmd_receiver_clbk_hdl.rx_msg_sz);
		memcpy(rx_msg_snap, priv->cmd_receiver_clbk_hdl.rx_msg, copy_len);
		priv->cmd_receiver_clbk_hdl.rx_msg_sz = 0;
		spin_unlock_irqrestore(&priv->cmd_receiver_clbk_hdl.clbk_rx_lock, flags);

		/* We may need to copy the output data to user before
		 * delivering the completion message.
		 */
		err = se_dev_ctx_cpy_out_data(dev_ctx);
		if (err < 0) {
			se_dev_ctx_shared_mem_cleanup(dev_ctx);
			mutex_unlock(&dev_ctx->fops_lock);
			return err;
		}
		/* Copy data from the buffer using the snapshot taken under the lock. */
		print_hex_dump_debug("to user ", DUMP_PREFIX_OFFSET, 4, 4,
				     rx_msg_snap, copy_len, false);

		err = copy_len;
		if (copy_to_user(buf, rx_msg_snap, copy_len))
			err = -EFAULT;

		se_dev_ctx_shared_mem_cleanup(dev_ctx);
		mutex_unlock(&dev_ctx->fops_lock);
	}

	return err;
}

/* Open a character device. */
static int se_if_fops_open(struct inode *nd, struct file *fp)
{
	struct miscdevice *miscdev = fp->private_data;
	struct se_if_open_gate *gate;
	struct se_if_device_ctx *misc_dev_ctx;
	struct se_if_device_ctx *dev_ctx;
	struct se_if_priv *priv;
	int err = 0;

	gate = container_of(miscdev, struct se_if_open_gate, miscdev);

	if (!se_if_open_gate_get(gate))
		return -ENODEV;

	if (mutex_lock_interruptible(&gate->lock)) {
		se_if_open_gate_put(gate);
		return -ERESTARTSYS;
	}

	if (gate->dying || !gate->priv ||
	    !kref_get_unless_zero(&gate->priv->refcount)) {
		err = -ENODEV;
		goto out_unlock_gate;
	}

	priv = gate->priv;
	mutex_unlock(&gate->lock);

	misc_dev_ctx = priv->priv_dev_ctx;

	if (mutex_lock_interruptible(&misc_dev_ctx->fops_lock)) {
		err = -ERESTARTSYS;
		goto out_put_priv;
	}

	if (misc_dev_ctx->cleanup_done) {
		err = -ENODEV;
		goto out_unlock_misc;
	}

	priv->dev_ctx_mono_count++;
	err = init_device_context(priv, priv->dev_ctx_mono_count, &dev_ctx);
	if (err) {
		dev_err(priv->dev, "Failed[0x%x] to create dev-ctx.", err);
		goto out_unlock_misc;
	}

	fp->private_data = dev_ctx;

out_unlock_misc:
	mutex_unlock(&misc_dev_ctx->fops_lock);
out_put_priv:
	kref_put(&priv->refcount, se_if_priv_release);
	se_if_open_gate_put(gate);
	return err;
out_unlock_gate:
	mutex_unlock(&gate->lock);
	se_if_open_gate_put(gate);
	return err;
}

/* Close a character device. */
static int se_if_fops_close(struct inode *nd, struct file *fp)
{
	struct se_if_device_ctx *dev_ctx = fp->private_data;

	dlink_n_cleanup_dev_ctx(dev_ctx, true);

	return 0;
}

/* IOCTL entry point of a character device */
static long se_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	struct se_if_device_ctx *dev_ctx = fp->private_data;
	struct se_if_priv *priv;
	void __user *uarg = (void __user *)arg;
	long err;

	/* Prevent race during change of device context */
	scoped_cond_guard(mutex_intr, return -ERESTARTSYS, &dev_ctx->fops_lock) {
		if (dev_ctx->cleanup_done)
			return -ENODEV;

		priv = dev_ctx->priv;

		switch (cmd) {
		case SE_IOCTL_ENABLE_CMD_RCV: {
			err = set_dev_ctx_as_command_receiver(dev_ctx);
			if (err)
				dev_err(priv->dev, "Failed to register %s as CMD-Receiver: %ld\n",
					dev_ctx->devname, err);
		break;
		}
		case SE_IOCTL_GET_MU_INFO:
			err = se_ioctl_get_mu_info(dev_ctx, uarg);
			break;
		case SE_IOCTL_SETUP_IOBUF:
			err = se_ioctl_setup_iobuf_handler(dev_ctx, uarg);
			break;
		case SE_IOCTL_GET_SOC_INFO:
			err = se_ioctl_get_se_soc_info_handler(dev_ctx, uarg);
			break;
		case SE_IOCTL_CMD_SEND_RCV_RSP:
			err = se_ioctl_cmd_snd_rcv_rsp_handler(dev_ctx, uarg);
			break;
		default:
			err = -ENOTTY;
			dev_dbg(priv->dev, "%s: IOCTL %.8x not supported.",
				dev_ctx->devname, cmd);
		}
	}

	return err;
}

/* Char driver setup */
static const struct file_operations se_if_fops = {
	.open		= se_if_fops_open,
	.owner		= THIS_MODULE,
	.release	= se_if_fops_close,
	.unlocked_ioctl = se_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
	.read		= se_if_fops_read,
	.write		= se_if_fops_write,
};

int se_get_mem_pool_buf(struct se_if_device_ctx *dev_ctx, void **buf,
			dma_addr_t *daddr, u32 len)
{
	struct se_shared_mem_mgmt_info *se_shared_mem_mgmt = &dev_ctx->se_shared_mem_mgmt;
	struct se_if_priv *priv = dev_ctx->priv;
	struct se_buf_desc *b_desc = NULL;

	lockdep_assert_held(&dev_ctx->fops_lock);

	if (se_is_fw_busy_ctx(dev_ctx))
		return -EBUSY;

	b_desc = kzalloc_obj(*b_desc, GFP_KERNEL);
	if (!b_desc)
		return -ENOMEM;

	/*
	 * gen_pool is internally thread-safe, so contexts may allocate
	 * concurrently. The buffer is tracked on this context's own
	 * mem_pool_buf_list and released on its cleanup path.
	 */
	*buf = gen_pool_dma_alloc(priv->mem_pool, len, daddr);
	if (!*buf) {
		dev_err(priv->dev, "Failed to alloc from gen_pool.\n");
		kfree(b_desc);
		return -ENOMEM;
	}

	/* gen_pool_dma_alloc() does not zero the buffer. */
	memset(*buf, 0, len);
	b_desc->shared_buf_ptr = *buf;
	b_desc->size = len;

	list_add_tail(&b_desc->link, &se_shared_mem_mgmt->mem_pool_buf_list);

	return 0;
}

void se_cleanup_mem_pool_buf(struct se_if_device_ctx *dev_ctx, bool reclaim)
{
	struct se_shared_mem_mgmt_info *se_shared_mem_mgmt = &dev_ctx->se_shared_mem_mgmt;
	struct se_if_priv *priv = dev_ctx->priv;
	struct se_buf_desc *b_desc, *temp;

	/*
	 * Free only the buffers this context allocated. A context that never
	 * used the pool has an empty list, so this is a no-op for it.
	 *
	 * Unlike the coherent staging buffer, the pool path needs no
	 * "nothing staged" (pos) gate on the reclaim=false leg. Pool buffers
	 * are ephemeral, per-transaction allocations: se_get_mem_pool_buf()
	 * refuses to allocate once the context is fw_busy, ele_msg_send_rcv()
	 * refuses to start a new command while fw_busy, and the success path
	 * frees the whole list via se_cleanup_mem_pool_buf(reclaim=true)
	 * before returning. se_if_cmd_lock serialises synchronous commands, so
	 * at most one transaction is outstanding. The only way to reach here
	 * with reclaim=false and a non-empty list is the single fw_busy
	 * context still owning the buffer(s) from the one timed-out
	 * transaction. Those buffers are exactly the in-flight ones the
	 * enclave may still be DMA-ing into, so leaving them on the list (no
	 * gen_pool_free) deliberately leaks them to avoid a DMA-after-free -
	 * there are no already-consumed pool buffers to reclaim on this leg.
	 */
	list_for_each_entry_safe(b_desc, temp, &se_shared_mem_mgmt->mem_pool_buf_list, link) {
		if (reclaim)
			gen_pool_free(priv->mem_pool,
				      (unsigned long)b_desc->shared_buf_ptr,
				      b_desc->size);
		list_del(&b_desc->link);
		kfree(b_desc);
	}
}

static void se_fw_busy_work(struct work_struct *work)
{
	struct se_if_priv *priv =
		container_of(work, struct se_if_priv, fw_busy_work);

	se_clear_fw_busy(priv);
}

static int se_suspend(struct device *dev)
{
	struct se_if_priv *priv = dev_get_drvdata(dev);
	struct se_fw_load_info *load_fw;
	int ret = 0;

	load_fw = get_load_fw_instance(priv);

	if (load_fw->imem_mgmt) {
		ret = se_save_imem_state(priv, &load_fw->imem);
		if (ret)
			dev_warn(dev, "Failure saving IMEM state[0x%x]", ret);
	}

	return 0;
}

static int se_resume(struct device *dev)
{
	struct se_if_priv *priv = dev_get_drvdata(dev);
	struct se_fw_load_info *load_fw;
	int ret = 0;

	load_fw = get_load_fw_instance(priv);

	if (load_fw->imem_mgmt) {
		ret = se_restore_imem_state(priv, &load_fw->imem);
		if (ret)
			dev_warn(dev, "Failure restoring IMEM state[0x%x]", ret);
	}

	return 0;
}

DEFINE_SIMPLE_DEV_PM_OPS(se_pm, se_suspend, se_resume);

static struct platform_driver se_driver = {
	.driver = {
		.name = "fsl-se",
		.of_match_table = se_match,
		.pm = pm_sleep_ptr(&se_pm),
	},
	.probe = se_if_probe,
};

static int __init se_init(void)
{
	return platform_driver_register(&se_driver);
}
module_init(se_init);

static void __exit se_exit(void)
{
	platform_driver_unregister(&se_driver);

	/*
	 * The soc_device is a module-scoped singleton that outlives any single
	 * MU interface bind/unbind. Release it here, once, after every interface
	 * has been unbound, so its lifetime is tied to the module rather than to
	 * the first-probed interface.
	 */
	se_soc_device_unregister(&var_se_info.soc_dev_regn);
}
module_exit(se_exit);

MODULE_AUTHOR("Pankaj Gupta <pankaj.gupta@nxp.com>");
MODULE_DESCRIPTION("iMX Secure Enclave Driver.");
MODULE_LICENSE("GPL");
