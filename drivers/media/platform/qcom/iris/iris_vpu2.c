// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/bits.h>
#include <linux/iopoll.h>
#include <linux/reset.h>

#include "iris_instance.h"
#include "iris_vpu_common.h"

#include "iris_vpu_register_defines.h"

static u64 iris_vpu2_calc_freq(struct iris_inst *inst, size_t data_size)
{
	struct platform_inst_caps *caps = inst->core->iris_platform_data->inst_caps;
	struct v4l2_format *inp_f = inst->fmt_src;
	u32 mbs_per_second, mbpf, height, width;
	unsigned long vpp_freq, vsp_freq;
	u32 fps = DEFAULT_FPS;

	width = max(inp_f->fmt.pix_mp.width, inst->crop.width);
	height = max(inp_f->fmt.pix_mp.height, inst->crop.height);

	mbpf = NUM_MBS_PER_FRAME(height, width);
	mbs_per_second = mbpf * fps;

	vpp_freq = mbs_per_second * caps->mb_cycles_vpp;

	/* 21 / 20 is overhead factor */
	vpp_freq += vpp_freq / 20;
	vsp_freq = mbs_per_second * caps->mb_cycles_vsp;

	/* 10 / 7 is overhead factor */
	vsp_freq += ((fps * data_size * 8) * 10) / 7;

	return max(vpp_freq, vsp_freq);
}

/* iris_vpu_power_off_hw + IRIS_HW_AXI_CLK */
static void iris_vpu21_power_off_hw(struct iris_core *core)
{
	dev_pm_genpd_set_hwmode(core->pmdomain_tbl->pd_devs[IRIS_HW_POWER_DOMAIN], false);
	iris_disable_power_domains(core, core->pmdomain_tbl->pd_devs[IRIS_HW_POWER_DOMAIN]);
	iris_disable_unprepare_clock(core, IRIS_HW_AXI_CLK);
	iris_disable_unprepare_clock(core, IRIS_HW_CLK);
}

/* iris_vpu_power_on_hw + IRIS_HW_AXI_CLK */
static int iris_vpu21_power_on_hw(struct iris_core *core)
{
	int ret;

	ret = iris_enable_power_domains(core, core->pmdomain_tbl->pd_devs[IRIS_HW_POWER_DOMAIN]);
	if (ret)
		return ret;

	ret = iris_prepare_enable_clock(core, IRIS_HW_CLK);
	if (ret)
		goto err_disable_power;

	ret = iris_prepare_enable_clock(core, IRIS_HW_AXI_CLK);
	if (ret)
		goto err_disable_hw_clock;

	ret = dev_pm_genpd_set_hwmode(core->pmdomain_tbl->pd_devs[IRIS_HW_POWER_DOMAIN], true);
	if (ret)
		goto err_disable_hw_axi_clock;

	return 0;

err_disable_hw_axi_clock:
	iris_disable_unprepare_clock(core, IRIS_HW_AXI_CLK);
err_disable_hw_clock:
	iris_disable_unprepare_clock(core, IRIS_HW_CLK);
err_disable_power:
	iris_disable_power_domains(core, core->pmdomain_tbl->pd_devs[IRIS_HW_POWER_DOMAIN]);

	return ret;
}

/* iris_vpu_power_on_controller + IRIS_AHB_CLK */
static int iris_vpu21_power_on_controller(struct iris_core *core)
{
	int ret;

	ret = iris_enable_power_domains(core, core->pmdomain_tbl->pd_devs[IRIS_CTRL_POWER_DOMAIN]);
	if (ret)
		return ret;

	ret = iris_prepare_enable_clock(core, IRIS_AXI_CLK);
	if (ret)
		goto err_disable_power;

	ret = iris_prepare_enable_clock(core, IRIS_CTRL_CLK);
	if (ret)
		goto err_disable_axi_clock;

	ret = iris_prepare_enable_clock(core, IRIS_AHB_CLK);
	if (ret)
		goto err_disable_ctrl_clock;

	return 0;

err_disable_ctrl_clock:
	iris_disable_unprepare_clock(core, IRIS_CTRL_CLK);
err_disable_axi_clock:
	iris_disable_unprepare_clock(core, IRIS_AXI_CLK);
err_disable_power:
	iris_disable_power_domains(core, core->pmdomain_tbl->pd_devs[IRIS_CTRL_POWER_DOMAIN]);

	return ret;
}

/*
 * This is the same as iris_vpu_power_off_controller except
 * AON_WRAPPER_MVP_NOC_LPI_CONTROL / AON_WRAPPER_MVP_NOC_LPI_STATUS programming
 * and with added IRIS_AHB_CLK handling
 */
static int iris_vpu21_power_off_controller(struct iris_core *core)
{
	u32 val = 0;
	int ret;

	writel(MSK_SIGNAL_FROM_TENSILICA | MSK_CORE_POWER_ON, core->reg_base + CPU_CS_X2RPMH);

	writel(REQ_POWER_DOWN_PREP, core->reg_base + WRAPPER_IRIS_CPU_NOC_LPI_CONTROL);

	ret = readl_poll_timeout(core->reg_base + WRAPPER_IRIS_CPU_NOC_LPI_STATUS,
				 val, val & BIT(0), 200, 2000);
	if (ret)
		goto disable_power;

	writel(0x0, core->reg_base + WRAPPER_DEBUG_BRIDGE_LPI_CONTROL);

	ret = readl_poll_timeout(core->reg_base + WRAPPER_DEBUG_BRIDGE_LPI_STATUS,
				 val, val == 0, 200, 2000);
	if (ret)
		goto disable_power;

	writel(CTL_AXI_CLK_HALT | CTL_CLK_HALT,
	       core->reg_base + WRAPPER_TZ_CTL_AXI_CLOCK_CONFIG);
	writel(RESET_HIGH, core->reg_base + WRAPPER_TZ_QNS4PDXFIFO_RESET);
	writel(0x0, core->reg_base + WRAPPER_TZ_QNS4PDXFIFO_RESET);
	writel(0x0, core->reg_base + WRAPPER_TZ_CTL_AXI_CLOCK_CONFIG);

disable_power:
	iris_disable_unprepare_clock(core, IRIS_AHB_CLK);
	iris_disable_unprepare_clock(core, IRIS_CTRL_CLK);
	iris_disable_unprepare_clock(core, IRIS_AXI_CLK);
	iris_disable_power_domains(core, core->pmdomain_tbl->pd_devs[IRIS_CTRL_POWER_DOMAIN]);

	return 0;
}

const struct vpu_ops iris_vpu2_ops = {
	.power_off_hw = iris_vpu_power_off_hw,
	.power_on_hw = iris_vpu_power_on_hw,
	.power_off_controller = iris_vpu_power_off_controller,
	.power_on_controller = iris_vpu_power_on_controller,
	.calc_freq = iris_vpu2_calc_freq,
};

const struct vpu_ops iris_vpu21_ops = {
	.power_off_hw = iris_vpu21_power_off_hw,
	.power_on_hw = iris_vpu21_power_on_hw,
	.power_off_controller = iris_vpu21_power_off_controller,
	.power_on_controller = iris_vpu21_power_on_controller,
	.calc_freq = iris_vpu2_calc_freq,
};
