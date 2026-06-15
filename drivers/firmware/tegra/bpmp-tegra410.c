// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, NVIDIA CORPORATION.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include <soc/tegra/bpmp.h>
#include <soc/tegra/bpmp-abi.h>

#include "bpmp-private.h"

bool tegra410_bpmp_mbwt_cmd_is_supported(struct tegra_bpmp *bpmp,
					 unsigned int cmd_code)
{
	struct mrq_sochub_mbwt_request request;
	struct tegra_bpmp_message msg;
	int err;

	memset(&request, 0, sizeof(request));
	request.cmd = CMD_SOCHUB_MBWT_QUERY_ABI;
	request.query_abi.cmd_code = cmd_code;

	memset(&msg, 0, sizeof(msg));
	msg.mrq = MRQ_SOCHUB_MBWT;
	msg.tx.data = &request;
	msg.tx.size = sizeof(request);

	err = tegra_bpmp_transfer(bpmp, &msg);
	if (err || msg.rx.ret)
		return false;

	return true;
}

int tegra410_bpmp_mbwt_set(struct tegra_bpmp *bpmp,
			   unsigned int instance,
			   unsigned int vc_type,
			   unsigned int bandwidth)
{
	struct mrq_sochub_mbwt_request request;
	struct tegra_bpmp_message msg;
	int err;

	memset(&request, 0, sizeof(request));
	request.cmd = CMD_SOCHUB_MBWT_SET_BW;
	request.set_bw.instance = instance;
	request.set_bw.vc_type = vc_type;
	request.set_bw.bw = bandwidth;

	memset(&msg, 0, sizeof(msg));
	msg.mrq = MRQ_SOCHUB_MBWT;
	msg.tx.data = &request;
	msg.tx.size = sizeof(request);

	err = tegra_bpmp_transfer(bpmp, &msg);

	if (err) {
		dev_err(bpmp->dev, "MBWT set bandwidth transfer failed: %d\n", err);
		return err;
	}
	if (msg.rx.ret < 0)
		return msg.rx.ret;

	return 0;
}

int tegra410_bpmp_mbwt_get(struct tegra_bpmp *bpmp,
			   unsigned int instance,
			   unsigned int vc_type,
			   unsigned int *bandwidth_out)
{
	struct mrq_sochub_mbwt_request request;
	struct mrq_sochub_mbwt_response response;
	struct tegra_bpmp_message msg;
	int err;

	if (!bandwidth_out)
		return -EINVAL;

	memset(&request, 0, sizeof(request));
	request.cmd = CMD_SOCHUB_MBWT_GET_BW;
	request.get_bw.instance = instance;
	request.get_bw.vc_type = vc_type;

	memset(&response, 0, sizeof(response));

	memset(&msg, 0, sizeof(msg));
	msg.mrq = MRQ_SOCHUB_MBWT;
	msg.tx.data = &request;
	msg.tx.size = sizeof(request);
	msg.rx.data = &response;
	msg.rx.size = sizeof(response);

	err = tegra_bpmp_transfer(bpmp, &msg);
	if (err) {
		dev_err(bpmp->dev, "MBWT get bandwidth transfer failed: %d\n", err);
		return err;
	}
	if (msg.rx.ret < 0)
		return msg.rx.ret;

	*bandwidth_out = response.get_bw.bw;

	return 0;
}
