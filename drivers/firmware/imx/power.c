// SPDX-License-Identifier: GPL-2.0+
/*
 * File containing client-side RPC functions for the PM (Power Management)
 * service. These function are ported to clients that communicate to the SC.
 */

#include <linux/firmware/imx/svc/pm.h>

struct imx_sc_msg_req_get_resource_power_mode {
	struct imx_sc_rpc_msg hdr;
	union {
		struct {
			u16 resource;
		} req;
		struct {
			u8 mode;
		} resp;
	} data;
} __packed __aligned(4);

/**
 * imx_sc_pm_get_resource_power_mode - Get power mode from a given resource.
 * @ipc: IPC handle.
 * @resource: Resource to check the power mode.
 *
 * Return: Returns < 0 for errors or the following for success:
 * * %IMX_SC_PM_PW_MODE_OFF	- Power off
 * * %IMX_SC_PM_PW_MODE_STBY	- Power in standby
 * * %IMX_SC_PM_PW_MODE_LP	- Power in low-power
 * * %IMX_SC_PM_PW_MODE_ON	- Power on
 *
 */
int imx_sc_pm_get_resource_power_mode(struct imx_sc_ipc *ipc, u32 resource)
{
	struct imx_sc_msg_req_get_resource_power_mode msg;
	struct imx_sc_rpc_msg *hdr = &msg.hdr;
	int ret;

	hdr->ver = IMX_SC_RPC_VERSION;
	hdr->svc = IMX_SC_RPC_SVC_PM;
	hdr->func = IMX_SC_PM_FUNC_GET_RESOURCE_POWER_MODE;
	hdr->size = 2;

	msg.data.req.resource = resource;

	ret = imx_scu_call_rpc(ipc, &msg, true);
	if (ret)
		return ret;

	return msg.data.resp.mode;
}
EXPORT_SYMBOL(imx_sc_pm_get_resource_power_mode);
