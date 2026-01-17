// SPDX-License-Identifier: GPL-2.0
/* Marvell RVU PF/VF Netdev Devlink
 *
 * Copyright (C) 2021 Marvell.
 */

#include "otx2_common.h"

static struct devlink_trap_group otx2_trap_groups_arr[] = {
	/* No policer is associated with following groups (policerid == 0)*/
	DEVLINK_TRAP_GROUP_GENERIC(L2_DROPS, 0),
};

static struct otx2_trap otx2_trap_items_arr[] = {
	{
		.trap = OTX2_TRAP_DROP(DMAC_FILTER, L2_DROPS),
	},
};

/* Devlink Params APIs */
static int otx2_dl_mcam_count_validate(struct devlink *devlink, u32 id,
				       union devlink_param_value val,
				       struct netlink_ext_ack *extack)
{
	struct otx2_devlink *otx2_dl = devlink_priv(devlink);
	struct otx2_nic *pfvf = otx2_dl->pfvf;
	struct otx2_flow_config *flow_cfg;

	if (!pfvf->flow_cfg) {
		NL_SET_ERR_MSG_MOD(extack,
				   "pfvf->flow_cfg not initialized");
		return -EINVAL;
	}

	flow_cfg = pfvf->flow_cfg;
	if (flow_cfg && flow_cfg->nr_flows) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Cannot modify count when there are active rules");
		return -EINVAL;
	}

	return 0;
}

static int otx2_dl_mcam_count_set(struct devlink *devlink, u32 id,
				  struct devlink_param_gset_ctx *ctx,
				  struct netlink_ext_ack *extack)
{
	struct otx2_devlink *otx2_dl = devlink_priv(devlink);
	struct otx2_nic *pfvf = otx2_dl->pfvf;

	if (!pfvf->flow_cfg)
		return 0;

	pfvf->flow_cfg->ntuple_cnt = ctx->val.vu16;
	otx2_alloc_mcam_entries(pfvf, ctx->val.vu16);

	return 0;
}

static int otx2_dl_mcam_count_get(struct devlink *devlink, u32 id,
				  struct devlink_param_gset_ctx *ctx,
				  struct netlink_ext_ack *extack)
{
	struct otx2_devlink *otx2_dl = devlink_priv(devlink);
	struct otx2_nic *pfvf = otx2_dl->pfvf;
	struct otx2_flow_config *flow_cfg;

	if (!pfvf->flow_cfg) {
		ctx->val.vu16 = 0;
		return 0;
	}

	flow_cfg = pfvf->flow_cfg;
	ctx->val.vu16 = flow_cfg->max_flows;

	return 0;
}

static int otx2_dl_ucast_flt_cnt_set(struct devlink *devlink, u32 id,
				     struct devlink_param_gset_ctx *ctx,
				     struct netlink_ext_ack *extack)
{
	struct otx2_devlink *otx2_dl = devlink_priv(devlink);
	struct otx2_nic *pfvf = otx2_dl->pfvf;
	int err;

	pfvf->flow_cfg->ucast_flt_cnt = ctx->val.vu8;

	otx2_mcam_flow_del(pfvf);
	err = otx2_mcam_entry_init(pfvf);
	if (err)
		return err;

	return 0;
}

static int otx2_dl_ucast_flt_cnt_get(struct devlink *devlink, u32 id,
				     struct devlink_param_gset_ctx *ctx,
				     struct netlink_ext_ack *extack)
{
	struct otx2_devlink *otx2_dl = devlink_priv(devlink);
	struct otx2_nic *pfvf = otx2_dl->pfvf;

	ctx->val.vu8 = pfvf->flow_cfg ? pfvf->flow_cfg->ucast_flt_cnt : 0;

	return 0;
}

static int otx2_dl_ucast_flt_cnt_validate(struct devlink *devlink, u32 id,
					  union devlink_param_value val,
					  struct netlink_ext_ack *extack)
{
	struct otx2_devlink *otx2_dl = devlink_priv(devlink);
	struct otx2_nic *pfvf = otx2_dl->pfvf;

	/* Check for UNICAST filter support*/
	if (!(pfvf->flags & OTX2_FLAG_UCAST_FLTR_SUPPORT)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Unicast filter not enabled");
		return -EINVAL;
	}

	if (!pfvf->flow_cfg) {
		NL_SET_ERR_MSG_MOD(extack,
				   "pfvf->flow_cfg not initialized");
		return -EINVAL;
	}

	if (pfvf->flow_cfg->nr_flows) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Cannot modify count when there are active rules");
		return -EINVAL;
	}

	return 0;
}

enum otx2_dl_param_id {
	OTX2_DEVLINK_PARAM_ID_BASE = DEVLINK_PARAM_GENERIC_ID_MAX,
	OTX2_DEVLINK_PARAM_ID_MCAM_COUNT,
	OTX2_DEVLINK_PARAM_ID_UCAST_FLT_CNT,
};

static const struct devlink_param otx2_dl_params[] = {
	DEVLINK_PARAM_DRIVER(OTX2_DEVLINK_PARAM_ID_MCAM_COUNT,
			     "mcam_count", DEVLINK_PARAM_TYPE_U16,
			     BIT(DEVLINK_PARAM_CMODE_RUNTIME),
			     otx2_dl_mcam_count_get, otx2_dl_mcam_count_set,
			     otx2_dl_mcam_count_validate),
	DEVLINK_PARAM_DRIVER(OTX2_DEVLINK_PARAM_ID_UCAST_FLT_CNT,
			     "unicast_filter_count", DEVLINK_PARAM_TYPE_U8,
			     BIT(DEVLINK_PARAM_CMODE_RUNTIME),
			     otx2_dl_ucast_flt_cnt_get, otx2_dl_ucast_flt_cnt_set,
			     otx2_dl_ucast_flt_cnt_validate),
};

#ifdef CONFIG_RVU_ESWITCH
static int otx2_devlink_eswitch_mode_get(struct devlink *devlink, u16 *mode)
{
	struct otx2_devlink *otx2_dl = devlink_priv(devlink);
	struct otx2_nic *pfvf = otx2_dl->pfvf;

	if (!otx2_rep_dev(pfvf->pdev))
		return -EOPNOTSUPP;

	*mode = pfvf->esw_mode;

	return 0;
}

static int otx2_devlink_eswitch_mode_set(struct devlink *devlink, u16 mode,
					 struct netlink_ext_ack *extack)
{
	struct otx2_devlink *otx2_dl = devlink_priv(devlink);
	struct otx2_nic *pfvf = otx2_dl->pfvf;
	int ret = 0;

	if (!otx2_rep_dev(pfvf->pdev))
		return -EOPNOTSUPP;

	if (pfvf->esw_mode == mode)
		return 0;

	switch (mode) {
	case DEVLINK_ESWITCH_MODE_LEGACY:
		rvu_rep_destroy(pfvf);
		break;
	case DEVLINK_ESWITCH_MODE_SWITCHDEV:
		ret = rvu_rep_create(pfvf, extack);
		break;
	default:
		return -EINVAL;
	}

	if (!ret)
		pfvf->esw_mode = mode;

	return ret;
}
#endif

static struct otx2_trap_item *
otx2_devlink_trap_item_lookup(struct otx2_devlink *dl, u16 trap_id)
{
	struct otx2_trap_data *trap_data = dl->trap_data;
	int i;

	for (i = 0; i < ARRAY_SIZE(otx2_trap_items_arr); i++) {
		if (otx2_trap_items_arr[i].trap.id == trap_id)
			return &trap_data->trap_items_arr[i];
	}

	return NULL;
}

static int otx2_trap_init(struct devlink *devlink,
			  const struct devlink_trap *trap, void *trap_ctx)
{
	struct otx2_devlink *otx2_dl = devlink_priv(devlink);
	struct otx2_trap_item *trap_item;

	trap_item = otx2_devlink_trap_item_lookup(otx2_dl, trap->id);
	if (WARN_ON(!trap_item))
		return -EINVAL;

	trap_item->trap_ctx = trap_ctx;
	trap_item->action = trap->init_action;

	return 0;
}

static int otx2_trap_action_set(struct devlink *devlink,
				const struct devlink_trap *trap,
				enum devlink_trap_action action,
				struct netlink_ext_ack *extack)
{
	/* Currently, driver does not support trap action altering */
	return -EOPNOTSUPP;
}

static int
otx2_trap_drop_counter_get(struct devlink *devlink,
			   const struct devlink_trap *trap,
			   u64 *p_drops)
{
	struct otx2_devlink *otx2_dl = devlink_priv(devlink);
	struct otx2_nic *pfvf = otx2_dl->pfvf;
	struct cgx_dmac_filter_drop_cnt *rsp;
	struct msg_req *req;
	int err;

	if (trap->id != DEVLINK_TRAP_GENERIC_ID_DMAC_FILTER)
		return -EINVAL;

	/* send mailbox to AF */
	mutex_lock(&pfvf->mbox.lock);

	req = otx2_mbox_alloc_msg_cgx_get_dmacflt_dropped_pktcnt(&pfvf->mbox);
	if (!req) {
		mutex_unlock(&pfvf->mbox.lock);
		return -ENOMEM;
	}

	err = otx2_sync_mbox_msg(&pfvf->mbox);
	if (err)
		goto fail;

	rsp = (struct cgx_dmac_filter_drop_cnt *)
			otx2_mbox_get_rsp(&pfvf->mbox.mbox, 0, &req->hdr);
	if (IS_ERR(rsp)) {
		err = PTR_ERR(rsp);
		goto fail;
	}
	*p_drops = rsp->count;

fail:
	mutex_unlock(&pfvf->mbox.lock);
	return err;
}

static const struct devlink_ops otx2_devlink_ops = {
#ifdef CONFIG_RVU_ESWITCH
	.eswitch_mode_get = otx2_devlink_eswitch_mode_get,
	.eswitch_mode_set = otx2_devlink_eswitch_mode_set,
#endif
	.trap_init = otx2_trap_init,
	.trap_action_set = otx2_trap_action_set,
	.trap_drop_counter_get = otx2_trap_drop_counter_get,
};

int otx2_register_dl(struct otx2_nic *pfvf)
{
	struct otx2_devlink *otx2_dl;
	struct devlink *dl;
	int err;

	dl = devlink_alloc(&otx2_devlink_ops,
			   sizeof(struct otx2_devlink), pfvf->dev);
	if (!dl) {
		dev_warn(pfvf->dev, "devlink_alloc failed\n");
		return -ENOMEM;
	}

	otx2_dl = devlink_priv(dl);
	otx2_dl->dl = dl;
	otx2_dl->pfvf = pfvf;
	pfvf->dl = otx2_dl;

	err = devlink_params_register(dl, otx2_dl_params,
				      ARRAY_SIZE(otx2_dl_params));
	if (err) {
		dev_err(pfvf->dev,
			"devlink params register failed with error %d", err);
		goto err_dl;
	}

	devlink_register(dl);
	return 0;

err_dl:
	devlink_free(dl);
	return err;
}
EXPORT_SYMBOL(otx2_register_dl);

void otx2_unregister_dl(struct otx2_nic *pfvf)
{
	struct otx2_devlink *otx2_dl = pfvf->dl;
	struct devlink *dl = otx2_dl->dl;

	devlink_unregister(dl);
	devlink_params_unregister(dl, otx2_dl_params,
				  ARRAY_SIZE(otx2_dl_params));
	devlink_free(dl);
}
EXPORT_SYMBOL(otx2_unregister_dl);

int otx2_devlink_traps_register(struct otx2_nic *pf)
{
	const u32 groups_count = ARRAY_SIZE(otx2_trap_groups_arr);
	const u32 traps_count = ARRAY_SIZE(otx2_trap_items_arr);
	struct devlink *devlink = priv_to_devlink(pf->dl);
	struct otx2_trap_data *trap_data;
	struct otx2_trap *otx2_trap;
	int err, i;

	trap_data = kzalloc(sizeof(*trap_data), GFP_KERNEL);
	if (!trap_data)
		return -ENOMEM;

	trap_data->trap_items_arr = kcalloc(traps_count,
					    sizeof(struct otx2_trap_item),
					    GFP_KERNEL);
	if (!trap_data->trap_items_arr) {
		err = -ENOMEM;
		goto err_trap_items_alloc;
	}

	trap_data->dl = pf->dl;
	trap_data->traps_count = traps_count;
	pf->dl->trap_data = trap_data;

	err = devlink_trap_groups_register(devlink, otx2_trap_groups_arr,
					   groups_count);
	if (err)
		goto err_groups_register;

	for (i = 0; i < traps_count; i++) {
		otx2_trap = &otx2_trap_items_arr[i];
		err = devlink_traps_register(devlink, &otx2_trap->trap, 1,
					     pf);
		if (err)
			goto err_trap_register;
	}

	return 0;

err_trap_register:
	for (i--; i >= 0; i--) {
		otx2_trap = &otx2_trap_items_arr[i];
		devlink_traps_unregister(devlink, &otx2_trap->trap, 1);
	}
	devlink_trap_groups_unregister(devlink, otx2_trap_groups_arr,
				       groups_count);
err_groups_register:
	kfree(trap_data->trap_items_arr);
err_trap_items_alloc:
	kfree(trap_data);
	return err;
}

void otx2_devlink_traps_unregister(struct otx2_nic *pf)
{
	struct otx2_trap_data *trap_data = pf->dl->trap_data;
	struct devlink *devlink = priv_to_devlink(pf->dl);
	const struct devlink_trap *trap;
	int i;

	for (i = 0; i < ARRAY_SIZE(otx2_trap_items_arr); ++i) {
		trap = &otx2_trap_items_arr[i].trap;
		devlink_traps_unregister(devlink, trap, 1);
	}

	devlink_trap_groups_unregister(devlink, otx2_trap_groups_arr,
				       ARRAY_SIZE(otx2_trap_groups_arr));
	kfree(trap_data->trap_items_arr);
	kfree(trap_data);
}
