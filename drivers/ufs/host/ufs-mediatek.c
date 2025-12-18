// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 MediaTek Inc.
 * Authors:
 *	Stanley Chu <stanley.chu@mediatek.com>
 *	Peter Wang <peter.wang@mediatek.com>
 */

#include <linux/arm-smccc.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>

#include <ufs/ufshcd.h>
#include "ufshcd-pltfrm.h"
#include <ufs/ufs_quirks.h>
#include <ufs/unipro.h>

#include "ufs-mediatek.h"
#include "ufs-mediatek-sip.h"

static int  ufs_mtk_config_mcq(struct ufs_hba *hba, bool irq);
static void _ufs_mtk_clk_scale(struct ufs_hba *hba, bool scale_up);

#define CREATE_TRACE_POINTS
#include "ufs-mediatek-trace.h"
#undef CREATE_TRACE_POINTS

#define MAX_SUPP_MAC 64
#define MCQ_QUEUE_OFFSET(c) ((((c) >> 16) & 0xFF) * 0x200)

struct ufs_mtk_soc_data {
	bool has_avdd09;
	u8 num_reg_names;
	const char *const *reg_names;
};

static const struct ufs_dev_quirk ufs_mtk_dev_fixups[] = {
	{ .wmanufacturerid = UFS_ANY_VENDOR,
	  .model = UFS_ANY_MODEL,
	  .quirk = UFS_DEVICE_QUIRK_DELAY_BEFORE_LPM },
	{ .wmanufacturerid = UFS_VENDOR_SKHYNIX,
	  .model = "H9HQ21AFAMZDAR",
	  .quirk = UFS_DEVICE_QUIRK_SUPPORT_EXTENDED_FEATURES },
	{}
};

/*
 * Details of UIC Errors
 */
static const char *const ufs_uic_err_str[] = {
	"PHY Adapter Layer",
	"Data Link Layer",
	"Network Link Layer",
	"Transport Link Layer",
	"DME"
};

static const char *const ufs_uic_pa_err_str[] = {
	"PHY error on Lane 0",
	"PHY error on Lane 1",
	"PHY error on Lane 2",
	"PHY error on Lane 3",
	"Generic PHY Adapter Error. This should be the LINERESET indication"
};

static const char *const ufs_uic_dl_err_str[] = {
	"NAC_RECEIVED",
	"TCx_REPLAY_TIMER_EXPIRED",
	"AFCx_REQUEST_TIMER_EXPIRED",
	"FCx_PROTECTION_TIMER_EXPIRED",
	"CRC_ERROR",
	"RX_BUFFER_OVERFLOW",
	"MAX_FRAME_LENGTH_EXCEEDED",
	"WRONG_SEQUENCE_NUMBER",
	"AFC_FRAME_SYNTAX_ERROR",
	"NAC_FRAME_SYNTAX_ERROR",
	"EOF_SYNTAX_ERROR",
	"FRAME_SYNTAX_ERROR",
	"BAD_CTRL_SYMBOL_TYPE",
	"PA_INIT_ERROR",
	"PA_ERROR_IND_RECEIVED",
	"PA_INIT"
};

static const char *const ufs_reset_names[] = {
	"unipro",
	"crypto",
	"hci",
};

static bool ufs_mtk_is_boost_crypt_enabled(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	return host->caps & UFS_MTK_CAP_BOOST_CRYPT_ENGINE;
}

static bool ufs_mtk_is_broken_vcc(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	return host->caps & UFS_MTK_CAP_BROKEN_VCC;
}

static bool ufs_mtk_is_pmc_via_fastauto(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	return host->caps & UFS_MTK_CAP_PMC_VIA_FASTAUTO;
}

static bool ufs_mtk_is_tx_skew_fix(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	return host->caps & UFS_MTK_CAP_TX_SKEW_FIX;
}

static bool ufs_mtk_is_rtff_mtcmos(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	return host->caps & UFS_MTK_CAP_RTFF_MTCMOS;
}

static bool ufs_mtk_is_allow_vccqx_lpm(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	return host->caps & UFS_MTK_CAP_ALLOW_VCCQX_LPM;
}

static bool ufs_mtk_is_clk_scale_ready(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct ufs_mtk_clk *mclk = &host->mclk;

	return mclk->ufs_sel_clki &&
		mclk->ufs_sel_max_clki &&
		mclk->ufs_sel_min_clki;
}

static void ufs_mtk_cfg_unipro_cg(struct ufs_hba *hba, bool enable)
{
	u32 tmp;

	if (enable) {
		ufshcd_dme_get(hba,
			       UIC_ARG_MIB(VS_SAVEPOWERCONTROL), &tmp);
		tmp = tmp |
		      (1 << RX_SYMBOL_CLK_GATE_EN) |
		      (1 << SYS_CLK_GATE_EN) |
		      (1 << TX_CLK_GATE_EN);
		ufshcd_dme_set(hba,
			       UIC_ARG_MIB(VS_SAVEPOWERCONTROL), tmp);

		ufshcd_dme_get(hba,
			       UIC_ARG_MIB(VS_DEBUGCLOCKENABLE), &tmp);
		tmp = tmp & ~(1 << TX_SYMBOL_CLK_REQ_FORCE);
		ufshcd_dme_set(hba,
			       UIC_ARG_MIB(VS_DEBUGCLOCKENABLE), tmp);
	} else {
		ufshcd_dme_get(hba,
			       UIC_ARG_MIB(VS_SAVEPOWERCONTROL), &tmp);
		tmp = tmp & ~((1 << RX_SYMBOL_CLK_GATE_EN) |
			      (1 << SYS_CLK_GATE_EN) |
			      (1 << TX_CLK_GATE_EN));
		ufshcd_dme_set(hba,
			       UIC_ARG_MIB(VS_SAVEPOWERCONTROL), tmp);

		ufshcd_dme_get(hba,
			       UIC_ARG_MIB(VS_DEBUGCLOCKENABLE), &tmp);
		tmp = tmp | (1 << TX_SYMBOL_CLK_REQ_FORCE);
		ufshcd_dme_set(hba,
			       UIC_ARG_MIB(VS_DEBUGCLOCKENABLE), tmp);
	}
}

static void ufs_mtk_crypto_enable(struct ufs_hba *hba)
{
	struct arm_smccc_res res;

	ufs_mtk_crypto_ctrl(res, 1);
	if (res.a0) {
		dev_err(hba->dev, "%s: crypto enable failed with error %lu, disabling\n",
			__func__, res.a0);
		hba->caps &= ~UFSHCD_CAP_CRYPTO;
	}
}

static void ufs_mtk_host_reset(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	int ret;

	ret = reset_control_bulk_assert(MTK_UFS_NUM_RESETS, host->resets);
	if (ret)
		dev_warn(hba->dev, "Host reset assert failed: %pe\n", ERR_PTR(ret));

	ret = phy_reset(host->mphy);

	/*
	 * Only sleep if MPHY doesn't have a reset implemented (which already
	 * sleeps) or the PHY reset function failed somehow, just to be safe
	 */
	if (ret) {
		usleep_range(100, 110);
		if (ret != -EOPNOTSUPP)
			dev_warn(hba->dev, "PHY reset failed: %pe\n", ERR_PTR(ret));
	}

	ret = reset_control_bulk_deassert(MTK_UFS_NUM_RESETS, host->resets);
	if (ret)
		dev_warn(hba->dev, "Host reset deassert failed: %pe\n", ERR_PTR(ret));
}

static int ufs_mtk_init_reset(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	int ret, i;

	for (i = 0; i < MTK_UFS_NUM_RESETS; i++)
		host->resets[i].id = ufs_reset_names[i];

	ret = devm_reset_control_bulk_get_optional_exclusive(hba->dev, MTK_UFS_NUM_RESETS,
							     host->resets);
	if (ret) {
		dev_err(hba->dev, "Failed to get resets: %pe\n", ERR_PTR(ret));
		return ret;
	}

	return 0;
}

static int ufs_mtk_hce_enable_notify(struct ufs_hba *hba,
				     enum ufs_notify_change_status status)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	if (status == PRE_CHANGE) {
		if (host->unipro_lpm) {
			hba->vps->hba_enable_delay_us = 0;
		} else {
			hba->vps->hba_enable_delay_us = 600;
			ufs_mtk_host_reset(hba);
		}

		if (hba->caps & UFSHCD_CAP_CRYPTO)
			ufs_mtk_crypto_enable(hba);

		if (host->caps & UFS_MTK_CAP_DISABLE_AH8) {
			ufshcd_writel(hba, 0,
				      REG_AUTO_HIBERNATE_IDLE_TIMER);
			hba->capabilities &= ~MASK_AUTO_HIBERN8_SUPPORT;
			hba->ahit = 0;
		}

		/*
		 * Turn on CLK_CG early to bypass abnormal ERR_CHK signal
		 * to prevent host hang issue
		 */
		ufshcd_writel(hba,
			      ufshcd_readl(hba, REG_UFS_XOUFS_CTRL) | 0x80,
			      REG_UFS_XOUFS_CTRL);

		if (host->legacy_ip_ver)
			return 0;

		/* DDR_EN setting */
		if (host->ip_ver >= IP_VER_MT6989) {
			ufshcd_rmwl(hba, UFS_MASK(0x7FFF, 8),
				0x453000, REG_UFS_MMIO_OPT_CTRL_0);
		}

		if (host->ip_ver >= IP_VER_MT6991_A0) {
			/* Enable multi-rtt */
			ufshcd_rmwl(hba, MRTT_EN, MRTT_EN, REG_UFS_MMIO_OPT_CTRL_0);
			/* Enable random performance improvement */
			ufshcd_rmwl(hba, RDN_PFM_IMPV_DIS, 0, REG_UFS_MMIO_OPT_CTRL_0);
		}
	}

	return 0;
}

static int ufs_mtk_setup_ref_clk(struct ufs_hba *hba, bool on)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct arm_smccc_res res;
	ktime_t timeout, time_checked;
	u32 value;

	if (host->ref_clk_enabled == on)
		return 0;

	ufs_mtk_ref_clk_notify(on, PRE_CHANGE, res);

	if (on) {
		ufshcd_writel(hba, REFCLK_REQUEST, REG_UFS_REFCLK_CTRL);
	} else {
		ufshcd_delay_us(host->ref_clk_gating_wait_us, 10);
		ufshcd_writel(hba, REFCLK_RELEASE, REG_UFS_REFCLK_CTRL);
	}

	/* Wait for ack */
	timeout = ktime_add_us(ktime_get(), REFCLK_REQ_TIMEOUT_US);
	do {
		time_checked = ktime_get();
		value = ufshcd_readl(hba, REG_UFS_REFCLK_CTRL);

		/* Wait until ack bit equals to req bit */
		if (((value & REFCLK_ACK) >> 1) == (value & REFCLK_REQUEST))
			goto out;

		usleep_range(100, 200);
	} while (ktime_before(time_checked, timeout));

	dev_err(hba->dev, "missing ack of refclk req, reg: 0x%x\n", value);

	/*
	 * If clock on timeout, assume clock is off, notify tfa do clock
	 * off setting.(keep DIFN disable, release resource)
	 * If clock off timeout, assume clock will off finally,
	 * set ref_clk_enabled directly.(keep DIFN disable, keep resource)
	 */
	if (on)
		ufs_mtk_ref_clk_notify(false, POST_CHANGE, res);
	else
		host->ref_clk_enabled = false;

	return -ETIMEDOUT;

out:
	host->ref_clk_enabled = on;
	if (on)
		ufshcd_delay_us(host->ref_clk_ungating_wait_us, 10);

	ufs_mtk_ref_clk_notify(on, POST_CHANGE, res);

	return 0;
}

static void ufs_mtk_setup_ref_clk_wait_us(struct ufs_hba *hba,
					  u16 gating_us)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	if (hba->dev_info.clk_gating_wait_us) {
		host->ref_clk_gating_wait_us =
			hba->dev_info.clk_gating_wait_us;
	} else {
		host->ref_clk_gating_wait_us = gating_us;
	}

	host->ref_clk_ungating_wait_us = REFCLK_DEFAULT_WAIT_US;
}

static void ufs_mtk_dbg_sel(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	if (!host->legacy_ip_ver && host->ip_ver >= IP_VER_MT6983) {
		ufshcd_writel(hba, 0x820820, REG_UFS_DEBUG_SEL);
		ufshcd_writel(hba, 0x0, REG_UFS_DEBUG_SEL_B0);
		ufshcd_writel(hba, 0x55555555, REG_UFS_DEBUG_SEL_B1);
		ufshcd_writel(hba, 0xaaaaaaaa, REG_UFS_DEBUG_SEL_B2);
		ufshcd_writel(hba, 0xffffffff, REG_UFS_DEBUG_SEL_B3);
	} else {
		ufshcd_writel(hba, 0x20, REG_UFS_DEBUG_SEL);
	}
}

static u32 ufs_mtk_read_state(struct ufs_hba *hba, bool old_style)
{
	u32 val;

	if (old_style) {
		ufs_mtk_dbg_sel(hba);
		val = ufshcd_readl(hba, REG_UFS_PROBE);
	} else {
		val = ufshcd_readl(hba, REG_UFS_UFS_MMIO_OTSD_CTRL) >> 16;
	}

	return FIELD_GET(0x1f, val);
}

static int ufs_mtk_wait_idle_state(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	bool old_style = (host->legacy_ip_ver || host->ip_ver < IP_VER_MT6899);
	u32 val;
	int ret;

	/* If the device is already in the base state after 10us, don't wait. */
	udelay(10);
	if (ufs_mtk_read_state(hba, old_style) == VS_HCE_BASE)
		return 0;

	/* Poll to wait for idle */
	ret = read_poll_timeout(ufs_mtk_read_state, val,
				(val < VS_HIB_ENTER || val > VS_HIB_EXIT), 50,
				5 * USEC_PER_MSEC, false, hba, old_style);
	if (ret) {
		dev_err(hba->dev, "Timed out waiting for idle state, val = 0x%x\n", val);
		return ret;
	}

	return 0;
}

static int ufs_mtk_wait_link_state(struct ufs_hba *hba, u32 state,
				   unsigned long max_wait_ms)
{
	ktime_t timeout, time_checked;
	u32 val;
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	timeout = ktime_add_ms(ktime_get(), max_wait_ms);
	do {
		time_checked = ktime_get();

		if (host->legacy_ip_ver || host->ip_ver < IP_VER_MT6899) {
			ufs_mtk_dbg_sel(hba);
			val = ufshcd_readl(hba, REG_UFS_PROBE);
			val = val >> 28;
		} else {
			val = ufshcd_readl(hba, REG_UFS_UFS_MMIO_OTSD_CTRL);
			val = val >> 24;
		}

		if (val == state)
			return 0;

		/* Sleep for max. 200us */
		usleep_range(100, 200);
	} while (ktime_before(time_checked, timeout));

	return -ETIMEDOUT;
}

static int ufs_mtk_09v_off(struct ufs_mtk_host *host)
{
	struct arm_smccc_res res;
	int ret;

	if (!host->reg_avdd09)
		return 0;

	ufs_mtk_va09_pwr_ctrl(res, 0);
	ret = regulator_disable(host->reg_avdd09);
	if (ret) {
		dev_err(host->hba->dev, "Failed to disable avdd09-supply: %pe\n",
			ERR_PTR(ret));
		ufs_mtk_va09_pwr_ctrl(res, 1);
		return ret;
	}

	return 0;
}

static int ufs_mtk_09v_on(struct ufs_mtk_host *host)
{
	struct arm_smccc_res res;
	int ret;

	if (!host->reg_avdd09)
		return 0;

	ret = regulator_enable(host->reg_avdd09);
	if (ret) {
		dev_err(host->hba->dev, "Failed to enable avdd09-supply: %pe\n",
			ERR_PTR(ret));
		return ret;
	}

	ufs_mtk_va09_pwr_ctrl(res, 1);

	return 0;
}

static int ufs_mtk_mphy_power_on(struct ufs_hba *hba, bool on)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct phy *mphy = host->mphy;
	int ret;

	if (!mphy || on == host->mphy_powered_on)
		return 0;

	if (on) {
		ret = ufs_mtk_09v_on(host);
		if (ret)
			return ret;
		phy_power_on(mphy);
	} else {
		phy_power_off(mphy);
		ret = ufs_mtk_09v_off(host);
		if (ret)
			return ret;
	}

	host->mphy_powered_on = on;

	return 0;
}

static void ufs_mtk_boost_crypt(struct ufs_hba *hba, bool boost)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct ufs_mtk_crypt_cfg *cfg;
	int volt, ret;

	if (!ufs_mtk_is_boost_crypt_enabled(hba) || !host->reg_vcore)
		return;

	cfg = host->crypt;
	volt = cfg->vcore_volt;

	ret = clk_prepare_enable(cfg->clk_crypt_mux);
	if (ret) {
		dev_err(hba->dev, "%s: Failed to enable clk_crypt_mux: %pe\n",
			__func__, ERR_PTR(ret));
		return;
	}

	if (boost) {
		ret = regulator_set_voltage(host->reg_vcore, volt, INT_MAX);
		if (ret) {
			dev_err(hba->dev, "%s: Failed to set vcore to %d: %pe\n",
				__func__, volt, ERR_PTR(ret));
			goto out;
		}

		ret = clk_set_parent(cfg->clk_crypt_mux, cfg->clk_crypt_perf);
		if (ret) {
			dev_err(hba->dev, "%s: Failed to reparent clk_crypt_perf: %pe\n",
				__func__, ERR_PTR(ret));
			regulator_set_voltage(host->reg_vcore, 0, INT_MAX);
			goto out;
		}
	} else {
		ret = clk_set_parent(cfg->clk_crypt_mux, cfg->clk_crypt_lp);
		if (ret) {
			dev_err(hba->dev, "%s: Failed to reparent clk_crypt_lp: %pe\n",
				__func__, ERR_PTR(ret));
			goto out;
		}

		ret = regulator_set_voltage(host->reg_vcore, 0, INT_MAX);
		if (ret) {
			dev_err(hba->dev, "%s: Failed to set vcore to minimum: %pe\n",
				__func__, ERR_PTR(ret));
		}
	}
out:
	clk_disable_unprepare(cfg->clk_crypt_mux);
}

static int ufs_mtk_init_boost_crypt(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct ufs_mtk_crypt_cfg *cfg;
	struct device *dev = hba->dev;
	int ret;

	if (!host->reg_vcore)
		return 0;

	cfg = devm_kzalloc(dev, sizeof(*cfg), GFP_KERNEL);
	if (!cfg)
		return -ENOMEM;

	ret = of_property_read_u32(dev->of_node, "mediatek,boost-crypt-vcore-min",
				   &cfg->vcore_volt);
	if (ret) {
		dev_err(dev, "Failed to get mediatek,boost-crypt-vcore-min: %pe\n",
			ERR_PTR(ret));
		return ret;
	}

	cfg->clk_crypt_mux = devm_clk_get(dev, "crypt_mux");
	if (IS_ERR(cfg->clk_crypt_mux)) {
		dev_err(dev, "Failed to get clock crypt_mux: %pe\n", cfg->clk_crypt_mux);
		return PTR_ERR(cfg->clk_crypt_mux);
	}

	cfg->clk_crypt_lp = devm_clk_get(dev, "crypt_lp");
	if (IS_ERR(cfg->clk_crypt_lp)) {
		dev_err(dev, "Failed to get clock crypt_lp: %pe\n", cfg->clk_crypt_lp);
		return PTR_ERR(cfg->clk_crypt_lp);
	}

	cfg->clk_crypt_perf = devm_clk_get(dev, "crypt_perf");
	if (IS_ERR(cfg->clk_crypt_perf)) {
		dev_err(dev, "Failed to get clock crypt_perf: %pe\n", cfg->clk_crypt_perf);
		return PTR_ERR(cfg->clk_crypt_perf);
	}

	host->crypt = cfg;
	host->caps |= UFS_MTK_CAP_BOOST_CRYPT_ENGINE;

	return 0;
}

static void ufs_mtk_init_host_caps(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct device_node *np = hba->dev->of_node;

	if (of_property_read_bool(np, "mediatek,ufs-boost-crypt"))
		ufs_mtk_init_boost_crypt(hba);

	if (of_property_read_bool(np, "mediatek,ufs-disable-ah8"))
		host->caps |= UFS_MTK_CAP_DISABLE_AH8;

	if (of_property_read_bool(np, "mediatek,ufs-broken-vcc"))
		host->caps |= UFS_MTK_CAP_BROKEN_VCC;

	if (of_property_read_bool(np, "mediatek,ufs-pmc-via-fastauto"))
		host->caps |= UFS_MTK_CAP_PMC_VIA_FASTAUTO;

	if (of_property_read_bool(np, "mediatek,ufs-tx-skew-fix"))
		host->caps |= UFS_MTK_CAP_TX_SKEW_FIX;

	if (of_property_read_bool(np, "mediatek,ufs-disable-mcq"))
		host->caps |= UFS_MTK_CAP_DISABLE_MCQ;

	if (of_property_read_bool(np, "mediatek,ufs-rtff-mtcmos"))
		host->caps |= UFS_MTK_CAP_RTFF_MTCMOS;

	dev_info(hba->dev, "caps: 0x%x", host->caps);
}

static void ufs_mtk_scale_perf(struct ufs_hba *hba, bool scale_up)
{
	ufs_mtk_boost_crypt(hba, scale_up);
}

static void ufs_mtk_pwr_ctrl(struct ufs_hba *hba, bool on)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	if (on) {
		phy_power_on(host->mphy);
		ufs_mtk_setup_ref_clk(hba, on);
		if (!ufshcd_is_clkscaling_supported(hba))
			ufs_mtk_scale_perf(hba, on);
	} else {
		if (!ufshcd_is_clkscaling_supported(hba))
			ufs_mtk_scale_perf(hba, on);
		ufs_mtk_setup_ref_clk(hba, on);
		phy_power_off(host->mphy);
	}
}

static void ufs_mtk_mcq_disable_irq(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	u32 irq, i;

	if (!hba->mcq_enabled)
		return;

	if (host->mcq_nr_intr == 0)
		return;

	for (i = 0; i < host->mcq_nr_intr; i++) {
		irq = host->mcq_intr_info[i].irq;
		disable_irq(irq);
	}
	host->is_mcq_intr_enabled = false;
}

static void ufs_mtk_mcq_enable_irq(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	u32 irq, i;

	if (!hba->mcq_enabled)
		return;

	if (host->mcq_nr_intr == 0)
		return;

	if (host->is_mcq_intr_enabled == true)
		return;

	for (i = 0; i < host->mcq_nr_intr; i++) {
		irq = host->mcq_intr_info[i].irq;
		enable_irq(irq);
	}
	host->is_mcq_intr_enabled = true;
}

/**
 * ufs_mtk_setup_clocks - enables/disable clocks
 * @hba: host controller instance
 * @on: If true, enable clocks else disable them.
 * @status: PRE_CHANGE or POST_CHANGE notify
 *
 * Return: 0 on success, non-zero on failure.
 */
static int ufs_mtk_setup_clocks(struct ufs_hba *hba, bool on,
				enum ufs_notify_change_status status)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	bool clk_pwr_off = false;
	int ret = 0;

	/*
	 * In case ufs_mtk_init() is not yet done, simply ignore.
	 * This ufs_mtk_setup_clocks() shall be called from
	 * ufs_mtk_init() after init is done.
	 */
	if (!host)
		return 0;

	if (!on && status == PRE_CHANGE) {
		if (ufshcd_is_link_off(hba)) {
			clk_pwr_off = true;
		} else if (ufshcd_is_link_hibern8(hba) ||
			 (!ufshcd_can_hibern8_during_gating(hba) &&
			 ufshcd_is_auto_hibern8_enabled(hba))) {
			/*
			 * Gate ref-clk and poweroff mphy if link state is in
			 * OFF or Hibern8 by either Auto-Hibern8 or
			 * ufshcd_link_state_transition().
			 */
			ret = ufs_mtk_wait_link_state(hba,
						      VS_LINK_HIBERN8,
						      15);
			if (!ret)
				clk_pwr_off = true;
		}

		if (clk_pwr_off) {
			ufs_mtk_pwr_ctrl(hba, false);
		} else {
			dev_warn(hba->dev, "Clock isn't off, hba->ahit = 0x%x, AHIT = 0x%x\n",
				 hba->ahit, ufshcd_readl(hba, REG_AUTO_HIBERNATE_IDLE_TIMER));
		}
		ufs_mtk_mcq_disable_irq(hba);
	} else if (on && status == POST_CHANGE) {
		ufs_mtk_pwr_ctrl(hba, true);
		ufs_mtk_mcq_enable_irq(hba);
	}

	return ret;
}

static u32 ufs_mtk_mcq_get_irq(struct ufs_hba *hba, unsigned int cpu)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct blk_mq_tag_set *tag_set = &hba->host->tag_set;
	struct blk_mq_queue_map	*map = &tag_set->map[HCTX_TYPE_DEFAULT];
	unsigned int nr = map->nr_queues;
	unsigned int q_index;

	q_index = map->mq_map[cpu];
	if (q_index >= nr) {
		dev_err(hba->dev, "hwq index %d exceed %d\n",
			q_index, nr);
		return MTK_MCQ_INVALID_IRQ;
	}

	return host->mcq_intr_info[q_index].irq;
}

static void ufs_mtk_mcq_set_irq_affinity(struct ufs_hba *hba, unsigned int cpu)
{
	unsigned int irq, _cpu;
	int ret;

	irq = ufs_mtk_mcq_get_irq(hba, cpu);
	if (irq == MTK_MCQ_INVALID_IRQ) {
		dev_err(hba->dev, "invalid irq. unable to bind irq to cpu%d", cpu);
		return;
	}

	/* force migrate irq of cpu0 to cpu3 */
	_cpu = (cpu == 0) ? 3 : cpu;
	ret = irq_set_affinity(irq, cpumask_of(_cpu));
	if (ret) {
		dev_err(hba->dev, "setting irq %d affinity to CPU %d failed\n",
			irq, _cpu);
		return;
	}
	dev_dbg(hba->dev, "set irq %d affinity to CPU %d\n", irq, _cpu);
}

static bool __pure ufs_mtk_is_legacy_chipset(u32 hw_ip_ver)
{
	switch (hw_ip_ver) {
	case IP_LEGACY_VER_MT6893:
	case IP_LEGACY_VER_MT6781:
		/* can add other legacy chipset ID here accordingly */
		return true;
	}

	return false;
}

#define MTK_UFS_VER_PREFIX_M (0xFF << 24)

/**
 * ufs_mtk_get_hw_ip_version - read and return adjusted hardware version
 * @hba: pointer to this device's &struct ufs_hba
 *
 * Reads, transforms and returns the hardware version.
 *
 * Since MT6878, the versioning scheme was changed from 01MMmmmm to 1MMMmmmm.
 * In order to support version comparisons across these different versioning
 * schemes, this function transforms the older style to the newer one.
 *
 * For example:
 *  MT6983 is transformed from 0x00360000 to 0x10360000
 *  MT6897 is transformed from 0x01440000 to 0x10440000
 *  MT6989 is transformed from 0x01450000 to 0x10450000
 *  MT6991 is transformed from 0x01460000 to 0x10460000
 *
 * Returns a u32 representing the hardware version.
 */
static u32 ufs_mtk_get_hw_ip_version(struct ufs_hba *hba)
{
	u32 version = ufshcd_readl(hba, REG_UFS_MTK_IP_VER);
	u32 prefix = FIELD_GET(MTK_UFS_VER_PREFIX_M, version);

	if (prefix <= 1)
		FIELD_MODIFY(MTK_UFS_VER_PREFIX_M, &version, BIT(28));

	return version;
}

static void ufs_mtk_get_controller_version(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	int ret, ver = 0;

	if (host->hw_ver.major)
		return;

	/* Set default (minimum) version anyway */
	host->hw_ver.major = 2;

	ret = ufshcd_dme_get(hba, UIC_ARG_MIB(PA_LOCALVERINFO), &ver);
	if (!ret) {
		if (ver >= UFS_UNIPRO_VER_1_8) {
			host->hw_ver.major = 3;
			/*
			 * Fix HCI version for some platforms with
			 * incorrect version
			 */
			if (hba->ufs_version < ufshci_version(3, 0))
				hba->ufs_version = ufshci_version(3, 0);
		}
	}
}

static u32 ufs_mtk_get_ufs_hci_version(struct ufs_hba *hba)
{
	return hba->ufs_version;
}

/**
 * ufs_mtk_init_clocks - Init mtk driver private clocks
 *
 * @hba: per adapter instance
 */
static void ufs_mtk_init_clocks(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct list_head *head = &hba->clk_list_head;
	struct ufs_clk_info *clki, *clki_tmp;
	struct device *dev = hba->dev;
	u32 volt;

	/*
	 * Find private clocks and store them in struct ufs_mtk_clk.
	 * Remove "ufs_sel_min_src" and "ufs_sel_min_src" from list to avoid
	 * being switched on/off in clock gating.
	 */
	list_for_each_entry_safe(clki, clki_tmp, head, list) {
		if (!strcmp(clki->name, "ufs_sel")) {
			host->mclk.ufs_sel_clki = clki;
		} else if (!strcmp(clki->name, "ufs_sel_max_src")) {
			host->mclk.ufs_sel_max_clki = clki;
			clk_disable_unprepare(clki->clk);
			list_del(&clki->list);
		} else if (!strcmp(clki->name, "ufs_sel_min_src")) {
			host->mclk.ufs_sel_min_clki = clki;
			clk_disable_unprepare(clki->clk);
			list_del(&clki->list);
		} else if (!strcmp(clki->name, "ufs_fde")) {
			host->mclk.ufs_fde_clki = clki;
		} else if (!strcmp(clki->name, "ufs_fde_max_src")) {
			host->mclk.ufs_fde_max_clki = clki;
			clk_disable_unprepare(clki->clk);
			list_del(&clki->list);
		} else if (!strcmp(clki->name, "ufs_fde_min_src")) {
			host->mclk.ufs_fde_min_clki = clki;
			clk_disable_unprepare(clki->clk);
			list_del(&clki->list);
		}
	}

	list_for_each_entry(clki, head, list)
		dev_dbg(hba->dev, "clk \"%s\" present", clki->name);

	if (!ufs_mtk_is_clk_scale_ready(hba)) {
		hba->caps &= ~UFSHCD_CAP_CLK_SCALING;
		dev_info(hba->dev, "%s: Clock scaling unavailable", __func__);
		return;
	}

	if (!host->reg_vcore)
		return;

	if (of_property_read_u32(dev->of_node, "mediatek,clk-scale-up-vcore-min",
				 &volt)) {
		dev_err(dev, "Failed to get mediatek,clk-scale-up-vcore-min\n");
		return;
	}

	host->mclk.vcore_volt = volt;

	/* If default boot is max gear, request vcore */
	if (volt && host->clk_scale_up)
		if (regulator_set_voltage(host->reg_vcore, volt, INT_MAX))
			dev_err(hba->dev, "Failed to set vcore to %d\n", volt);
}

static void ufs_mtk_setup_clk_gating(struct ufs_hba *hba)
{
	unsigned long flags;
	u32 ah_ms = 10;
	u32 ah_scale, ah_timer;
	u32 scale_us[] = {1, 10, 100, 1000, 10000, 100000};

	if (ufshcd_is_clkgating_allowed(hba)) {
		if (ufshcd_is_auto_hibern8_supported(hba) && hba->ahit) {
			ah_scale = FIELD_GET(UFSHCI_AHIBERN8_SCALE_MASK,
					  hba->ahit);
			ah_timer = FIELD_GET(UFSHCI_AHIBERN8_TIMER_MASK,
					  hba->ahit);
			if (ah_scale <= 5)
				ah_ms = ah_timer * scale_us[ah_scale] / 1000;
		}

		spin_lock_irqsave(hba->host->host_lock, flags);
		hba->clk_gating.delay_ms = max(ah_ms, 10U);
		spin_unlock_irqrestore(hba->host->host_lock, flags);
	}
}

static void ufs_mtk_fix_ahit(struct ufs_hba *hba)
{
	unsigned int us;

	if (ufshcd_is_auto_hibern8_supported(hba)) {
		switch (hba->dev_info.wmanufacturerid) {
		case UFS_VENDOR_SAMSUNG:
			/* configure auto-hibern8 timer to 3.5 ms */
			us = 3500;
			break;

		case UFS_VENDOR_MICRON:
			/* configure auto-hibern8 timer to 2 ms */
			us = 2000;
			break;

		default:
			/* configure auto-hibern8 timer to 1 ms */
			us = 1000;
			break;
		}

		hba->ahit = ufshcd_us_to_ahit(us);
	}

	ufs_mtk_setup_clk_gating(hba);
}

static void ufs_mtk_fix_clock_scaling(struct ufs_hba *hba)
{
	/* UFS version is below 4.0, clock scaling is not necessary */
	if ((hba->dev_info.wspecversion < 0x0400)  &&
		ufs_mtk_is_clk_scale_ready(hba)) {
		hba->caps &= ~UFSHCD_CAP_CLK_SCALING;

		_ufs_mtk_clk_scale(hba, false);
	}
}

static void ufs_mtk_init_mcq_irq(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct platform_device *pdev;
	int i;
	int irq;

	host->mcq_nr_intr = UFSHCD_MAX_Q_NR;
	pdev = container_of(hba->dev, struct platform_device, dev);

	if (host->caps & UFS_MTK_CAP_DISABLE_MCQ)
		goto failed;

	for (i = 0; i < host->mcq_nr_intr; i++) {
		/* irq index 0 is legacy irq, sq/cq irq start from index 1 */
		irq = platform_get_irq(pdev, i + 1);
		if (irq < 0) {
			host->mcq_intr_info[i].irq = MTK_MCQ_INVALID_IRQ;
			goto failed;
		}
		host->mcq_intr_info[i].hba = hba;
		host->mcq_intr_info[i].irq = irq;
		dev_dbg(hba->dev, "get platform mcq irq: %d, %d\n", i, irq);
	}

	return;
failed:
       /* invalidate irq info */
	for (i = 0; i < host->mcq_nr_intr; i++)
		host->mcq_intr_info[i].irq = MTK_MCQ_INVALID_IRQ;

	host->mcq_nr_intr = 0;
}

/**
 * ufs_mtk_get_supplies - acquire variant-specific supplies
 * @host: pointer to driver's private &struct ufs_mtk_host instance
 *
 * Returns 0 on success, negative errno on error.
 */
static int ufs_mtk_get_supplies(struct ufs_mtk_host *host)
{
	struct device *dev = host->hba->dev;
	const struct ufs_mtk_soc_data *data = of_device_get_match_data(dev);
	int ret;

	host->reg_vcore = devm_regulator_get_optional(dev, "dvfsrc-vcore");
	if (IS_ERR(host->reg_vcore)) {
		if (PTR_ERR(host->reg_vcore) != -ENODEV) {
			dev_err(dev, "Failed to get dvfsrc-vcore supply: %pe\n",
				host->reg_vcore);
			return PTR_ERR(host->reg_vcore);
		}

		host->reg_vcore = NULL;
	}

	if (!data)
		return 0;

	if (data->num_reg_names) {
		host->reg_misc = devm_kcalloc(dev, data->num_reg_names,
					      sizeof(*host->reg_misc), GFP_KERNEL);
		if (!host->reg_misc)
			return -ENOMEM;

		regulator_bulk_set_supply_names(host->reg_misc, data->reg_names,
						data->num_reg_names);

		ret = devm_regulator_bulk_get(dev, data->num_reg_names, host->reg_misc);
		if (ret) {
			dev_err(dev, "Failed to get misc regulators: %pe\n", ERR_PTR(ret));
			return ret;
		}

		host->num_reg_misc = data->num_reg_names;

		ret = regulator_bulk_enable(host->num_reg_misc, host->reg_misc);
		if (ret) {
			dev_err(dev, "Failed to turn on misc regulators: %pe\n",
				ERR_PTR(ret));
			return ret;
		}
	}

	if (!data->has_avdd09)
		return 0;

	host->reg_avdd09 = devm_regulator_get_optional(dev, "avdd09");
	if (IS_ERR(host->reg_avdd09)) {
		if (PTR_ERR(host->reg_avdd09) != -ENODEV) {
			dev_err(dev, "Failed to get avdd09 regulator: %pe\n",
				host->reg_avdd09);
			return PTR_ERR(host->reg_avdd09);
		}

		host->reg_avdd09 = NULL;
	}

	return 0;
}

/**
 * ufs_mtk_init - find other essential mmio bases
 * @hba: host controller instance
 *
 * Binds PHY with controller and powers up PHY enabling clocks
 * and regulators.
 *
 * Return: -EPROBE_DEFER if binding fails, returns negative error
 * on phy power up failure and returns zero on success.
 */
static int ufs_mtk_init(struct ufs_hba *hba)
{
	struct device *dev = hba->dev;
	struct ufs_mtk_host *host;
	struct Scsi_Host *shost = hba->host;
	int err;
	struct arm_smccc_res res;

	host = devm_kzalloc(dev, sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENOMEM;

	host->hba = hba;
	ufshcd_set_variant(hba, host);

	err = ufs_mtk_get_supplies(host);
	if (err)
		goto out_variant_clear;

	/* Initialize host capability */
	ufs_mtk_init_host_caps(hba);

	ufs_mtk_init_mcq_irq(hba);

	host->mphy = devm_phy_get(dev, NULL);
	if (IS_ERR(host->mphy)) {
		err = dev_err_probe(dev, PTR_ERR(host->mphy), "Failed to get PHY\n");
		goto out_variant_clear;
	}

	err = phy_init(host->mphy);
	if (err) {
		dev_err_probe(dev, err, "Failed to initialize PHY\n");
		goto out_variant_clear;
	}

	err = ufs_mtk_init_reset(hba);
	if (err)
		goto out_phy_exit;

	/* Enable runtime autosuspend */
	hba->caps |= UFSHCD_CAP_RPM_AUTOSUSPEND;

	/* Enable clock-gating */
	hba->caps |= UFSHCD_CAP_CLK_GATING;

	/* Enable inline encryption */
	hba->caps |= UFSHCD_CAP_CRYPTO;

	/* Enable WriteBooster */
	hba->caps |= UFSHCD_CAP_WB_EN;

	/* Enable clk scaling*/
	hba->caps |= UFSHCD_CAP_CLK_SCALING;
	host->clk_scale_up = true; /* default is max freq */

	/* Set runtime pm delay to replace default */
	shost->rpm_autosuspend_delay = MTK_RPM_AUTOSUSPEND_DELAY_MS;

	hba->quirks |= UFSHCI_QUIRK_SKIP_MANUAL_WB_FLUSH_CTRL;

	hba->quirks |= UFSHCD_QUIRK_MCQ_BROKEN_INTR;

	hba->vps->wb_flush_threshold = UFS_WB_BUF_REMAIN_PERCENT(80);

	if (host->caps & UFS_MTK_CAP_DISABLE_AH8)
		hba->caps |= UFSHCD_CAP_HIBERN8_WITH_CLK_GATING;

	if (host->caps & UFS_MTK_CAP_DISABLE_MCQ)
		hba->quirks |= UFSHCD_QUIRK_BROKEN_LSDBS_CAP;

	ufs_mtk_init_clocks(hba);

	/*
	 * ufshcd_vops_init() is invoked after
	 * ufshcd_setup_clock(true) in ufshcd_hba_init() thus
	 * phy clock setup is skipped.
	 *
	 * Enable phy clocks specifically here.
	 */
	ufs_mtk_mphy_power_on(hba, true);

	if (ufs_mtk_is_rtff_mtcmos(hba)) {
		/* First Restore here, to avoid backup unexpected value */
		ufs_mtk_mtcmos_ctrl(false, res);

		/* Power on to init */
		ufs_mtk_mtcmos_ctrl(true, res);
	}

	ufs_mtk_setup_clocks(hba, true, POST_CHANGE);

	host->ip_ver = ufs_mtk_get_hw_ip_version(hba);
	host->legacy_ip_ver = ufs_mtk_is_legacy_chipset(host->ip_ver);

	dev_dbg(hba->dev, "IP version 0x%x, legacy = %s", host->ip_ver,
		str_true_false(host->legacy_ip_ver));

	return 0;

out_phy_exit:
	phy_exit(host->mphy);
out_variant_clear:
	ufshcd_set_variant(hba, NULL);
	return err;
}

static void ufs_mtk_exit(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	ufs_mtk_mphy_power_on(hba, false);

	phy_exit(host->mphy);
}

static bool ufs_mtk_pmc_via_fastauto(struct ufs_hba *hba,
				     struct ufs_pa_layer_attr *dev_req_params)
{
	if (!ufs_mtk_is_pmc_via_fastauto(hba))
		return false;

	if (dev_req_params->hs_rate == hba->pwr_info.hs_rate)
		return false;

	if (dev_req_params->pwr_tx != FAST_MODE &&
	    dev_req_params->gear_tx < UFS_HS_G4)
		return false;

	if (dev_req_params->pwr_rx != FAST_MODE &&
	    dev_req_params->gear_rx < UFS_HS_G4)
		return false;

	if (dev_req_params->pwr_tx == SLOW_MODE ||
	    dev_req_params->pwr_rx == SLOW_MODE)
		return false;

	return true;
}

static void ufs_mtk_adjust_sync_length(struct ufs_hba *hba)
{
	int i;
	u32 value;
	u32 cnt, att, min;
	struct attr_min {
		u32 attr;
		u32 min_value;
	} pa_min_sync_length[] = {
		{PA_TXHSG1SYNCLENGTH, 0x48},
		{PA_TXHSG2SYNCLENGTH, 0x48},
		{PA_TXHSG3SYNCLENGTH, 0x48},
		{PA_TXHSG4SYNCLENGTH, 0x48},
		{PA_TXHSG5SYNCLENGTH, 0x48}
	};

	cnt = sizeof(pa_min_sync_length) / sizeof(struct attr_min);
	for (i = 0; i < cnt; i++) {
		att = pa_min_sync_length[i].attr;
		min = pa_min_sync_length[i].min_value;
		ufshcd_dme_get(hba, UIC_ARG_MIB(att), &value);
		if (value < min)
			ufshcd_dme_set(hba, UIC_ARG_MIB(att), min);

		ufshcd_dme_peer_get(hba, UIC_ARG_MIB(att), &value);
		if (value < min)
			ufshcd_dme_peer_set(hba, UIC_ARG_MIB(att), min);
	}
}

static int ufs_mtk_pre_pwr_change(struct ufs_hba *hba,
				const struct ufs_pa_layer_attr *dev_max_params,
				struct ufs_pa_layer_attr *dev_req_params)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct ufs_host_params host_params;
	int ret;

	ufshcd_init_host_params(&host_params);
	host_params.hs_rx_gear = UFS_HS_G5;
	host_params.hs_tx_gear = UFS_HS_G5;

	if (dev_max_params->pwr_rx == SLOW_MODE ||
	    dev_max_params->pwr_tx == SLOW_MODE)
		host_params.desired_working_mode = UFS_PWM_MODE;

	ret = ufshcd_negotiate_pwr_params(&host_params, dev_max_params, dev_req_params);
	if (ret)
		dev_warn(hba->dev, "%s: failed to determine capabilities\n", __func__);

	if (ufs_mtk_pmc_via_fastauto(hba, dev_req_params)) {
		ufs_mtk_adjust_sync_length(hba);

		ufshcd_dme_set(hba, UIC_ARG_MIB(PA_TXTERMINATION), true);
		ufshcd_dme_set(hba, UIC_ARG_MIB(PA_TXGEAR), UFS_HS_G1);

		ufshcd_dme_set(hba, UIC_ARG_MIB(PA_RXTERMINATION), true);
		ufshcd_dme_set(hba, UIC_ARG_MIB(PA_RXGEAR), UFS_HS_G1);

		ufshcd_dme_set(hba, UIC_ARG_MIB(PA_ACTIVETXDATALANES),
			       dev_req_params->lane_tx);
		ufshcd_dme_set(hba, UIC_ARG_MIB(PA_ACTIVERXDATALANES),
			       dev_req_params->lane_rx);
		ufshcd_dme_set(hba, UIC_ARG_MIB(PA_HSSERIES),
			       dev_req_params->hs_rate);

		ufshcd_dme_set(hba, UIC_ARG_MIB(PA_TXHSADAPTTYPE),
			       PA_NO_ADAPT);

		if (!(hba->quirks & UFSHCD_QUIRK_SKIP_DEF_UNIPRO_TIMEOUT_SETTING)) {
			ufshcd_dme_set(hba, UIC_ARG_MIB(PA_PWRMODEUSERDATA0),
					DL_FC0ProtectionTimeOutVal_Default);
			ufshcd_dme_set(hba, UIC_ARG_MIB(PA_PWRMODEUSERDATA1),
					DL_TC0ReplayTimeOutVal_Default);
			ufshcd_dme_set(hba, UIC_ARG_MIB(PA_PWRMODEUSERDATA2),
					DL_AFC0ReqTimeOutVal_Default);
			ufshcd_dme_set(hba, UIC_ARG_MIB(PA_PWRMODEUSERDATA3),
					DL_FC1ProtectionTimeOutVal_Default);
			ufshcd_dme_set(hba, UIC_ARG_MIB(PA_PWRMODEUSERDATA4),
					DL_TC1ReplayTimeOutVal_Default);
			ufshcd_dme_set(hba, UIC_ARG_MIB(PA_PWRMODEUSERDATA5),
					DL_AFC1ReqTimeOutVal_Default);

			ufshcd_dme_set(hba, UIC_ARG_MIB(DME_LocalFC0ProtectionTimeOutVal),
					DL_FC0ProtectionTimeOutVal_Default);
			ufshcd_dme_set(hba, UIC_ARG_MIB(DME_LocalTC0ReplayTimeOutVal),
					DL_TC0ReplayTimeOutVal_Default);
			ufshcd_dme_set(hba, UIC_ARG_MIB(DME_LocalAFC0ReqTimeOutVal),
					DL_AFC0ReqTimeOutVal_Default);
		}

		ret = ufshcd_uic_change_pwr_mode(hba,
					FASTAUTO_MODE << 4 | FASTAUTO_MODE);

		if (ret)
			dev_err(hba->dev, "%s: HSG1B FASTAUTO failed: %pe\n",
				__func__, ERR_PTR(ret));
	}

	/* if already configured to the requested pwr_mode, skip adapt */
	if (dev_req_params->gear_rx == hba->pwr_info.gear_rx &&
	    dev_req_params->gear_tx == hba->pwr_info.gear_tx &&
	    dev_req_params->lane_rx == hba->pwr_info.lane_rx &&
	    dev_req_params->lane_tx == hba->pwr_info.lane_tx &&
	    dev_req_params->pwr_rx == hba->pwr_info.pwr_rx &&
	    dev_req_params->pwr_tx == hba->pwr_info.pwr_tx &&
	    dev_req_params->hs_rate == hba->pwr_info.hs_rate) {
		return ret;
	}

	if (dev_req_params->pwr_rx == FAST_MODE ||
	    dev_req_params->pwr_rx == FASTAUTO_MODE) {
		if (host->hw_ver.major >= 3) {
			ret = ufshcd_dme_configure_adapt(hba,
						   dev_req_params->gear_tx,
						   PA_INITIAL_ADAPT);
		} else {
			ret = ufshcd_dme_configure_adapt(hba,
				   dev_req_params->gear_tx,
				   PA_NO_ADAPT);
		}
	} else {
		ret = ufshcd_dme_configure_adapt(hba,
			   dev_req_params->gear_tx,
			   PA_NO_ADAPT);
	}

	return ret;
}

static int ufs_mtk_auto_hibern8_disable(struct ufs_hba *hba)
{
	int ret;

	/* disable auto-hibern8 */
	ufshcd_writel(hba, 0, REG_AUTO_HIBERNATE_IDLE_TIMER);

	/* wait host return to idle state when auto-hibern8 off */
	ret = ufs_mtk_wait_idle_state(hba);
	if (ret)
		goto out;

	ret = ufs_mtk_wait_link_state(hba, VS_LINK_UP, 100);

out:
	if (ret) {
		dev_err(hba->dev, "Failed to exit h8 state: %pe\n", ERR_PTR(ret));

		ufshcd_force_error_recovery(hba);

		/* trigger error handler and break suspend */
		ret = -EBUSY;
	}

	return ret;
}

static int ufs_mtk_pwr_change_notify(struct ufs_hba *hba,
				enum ufs_notify_change_status stage,
				const struct ufs_pa_layer_attr *dev_max_params,
				struct ufs_pa_layer_attr *dev_req_params)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	switch (stage) {
	case PRE_CHANGE:
		if (ufshcd_is_auto_hibern8_supported(hba)) {
			host->hibernate_idle_timer = ufshcd_readl(
				hba, REG_AUTO_HIBERNATE_IDLE_TIMER);
			ufs_mtk_auto_hibern8_disable(hba);
		}
		return ufs_mtk_pre_pwr_change(hba, dev_max_params, dev_req_params);
	case POST_CHANGE:
		if (ufshcd_is_auto_hibern8_supported(hba))
			ufshcd_writel(hba, host->hibernate_idle_timer,
				      REG_AUTO_HIBERNATE_IDLE_TIMER);
		return 0;
	}

	return -EINVAL;
}

static int ufs_mtk_unipro_set_lpm(struct ufs_hba *hba, bool lpm)
{
	int ret;
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	ret = ufshcd_dme_set(hba,
			     UIC_ARG_MIB_SEL(VS_UNIPROPOWERDOWNCONTROL, 0),
			     lpm ? 1 : 0);
	if (!ret || !lpm) {
		/*
		 * Forcibly set as non-LPM mode if UIC commands is failed
		 * to use default hba_enable_delay_us value for re-enabling
		 * the host.
		 */
		host->unipro_lpm = lpm;
	}

	return ret;
}

static int ufs_mtk_pre_link(struct ufs_hba *hba)
{
	int ret;
	u32 tmp;
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	ufs_mtk_get_controller_version(hba);

	ret = ufs_mtk_unipro_set_lpm(hba, false);
	if (ret)
		return ret;

	/*
	 * Setting PA_Local_TX_LCC_Enable to 0 before link startup
	 * to make sure that both host and device TX LCC are disabled
	 * once link startup is completed.
	 */
	ret = ufshcd_disable_host_tx_lcc(hba);
	if (ret)
		return ret;

	/* disable deep stall */
	ret = ufshcd_dme_get(hba, UIC_ARG_MIB(VS_SAVEPOWERCONTROL), &tmp);
	if (ret)
		return ret;

	tmp &= ~(1 << 6);

	ret = ufshcd_dme_set(hba, UIC_ARG_MIB(VS_SAVEPOWERCONTROL), tmp);

	/* Enable the 1144 functions setting */
	if (host->ip_ver == IP_VER_MT6989) {
		ret = ufshcd_dme_get(hba, UIC_ARG_MIB(VS_DEBUGOMC), &tmp);
		if (ret)
			return ret;

		tmp |= 0x10;
		ret = ufshcd_dme_set(hba, UIC_ARG_MIB(VS_DEBUGOMC), tmp);
	}

	return ret;
}

static void ufs_mtk_post_link(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	u32 tmp;

	/* fix device PA_INIT no adapt */
	if (host->ip_ver >= IP_VER_MT6899) {
		ufshcd_dme_get(hba, UIC_ARG_MIB(VS_DEBUGOMC), &tmp);
		tmp |= 0x100;
		ufshcd_dme_set(hba, UIC_ARG_MIB(VS_DEBUGOMC), tmp);
	}

	/* enable unipro clock gating feature */
	ufs_mtk_cfg_unipro_cg(hba, true);
}

static int ufs_mtk_link_startup_notify(struct ufs_hba *hba,
				       enum ufs_notify_change_status stage)
{
	int ret = 0;

	switch (stage) {
	case PRE_CHANGE:
		ret = ufs_mtk_pre_link(hba);
		break;
	case POST_CHANGE:
		ufs_mtk_post_link(hba);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int ufs_mtk_device_reset(struct ufs_hba *hba)
{
	struct arm_smccc_res res;

	ufs_mtk_device_reset_ctrl(0, res);

	/* disable hba in middle of device reset */
	ufshcd_hba_stop(hba);

	/*
	 * The reset signal is active low. UFS devices shall detect
	 * more than or equal to 1us of positive or negative RST_n
	 * pulse width.
	 *
	 * To be on safe side, keep the reset low for at least 10us.
	 */
	usleep_range(10, 15);

	ufs_mtk_device_reset_ctrl(1, res);

	/* Some devices may need time to respond to rst_n */
	usleep_range(10000, 15000);

	dev_dbg(hba->dev, "device reset done\n");

	return 0;
}

static int ufs_mtk_link_set_hpm(struct ufs_hba *hba)
{
	int err;
	u32 val;
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	err = ufshcd_hba_enable(hba);
	if (err)
		return err;

	err = ufs_mtk_unipro_set_lpm(hba, false);
	if (err) {
		if (host->ip_ver < IP_VER_MT6899) {
			ufs_mtk_dbg_sel(hba);
			val = ufshcd_readl(hba, REG_UFS_PROBE);
		} else {
			val = ufshcd_readl(hba, REG_UFS_UFS_MMIO_OTSD_CTRL);
		}
		ufshcd_update_evt_hist(hba, UFS_EVT_RESUME_ERR, (u32)val);
		val = ufshcd_readl(hba, REG_INTERRUPT_STATUS);
		ufshcd_update_evt_hist(hba, UFS_EVT_RESUME_ERR, (u32)val);
		return err;
	}

	err = ufshcd_uic_hibern8_exit(hba);
	if (err)
		return err;

	/* Check link state to make sure exit h8 success */
	err = ufs_mtk_wait_idle_state(hba);
	if (err) {
		dev_err(hba->dev, "Failed to wait for idle: %pe\n", ERR_PTR(err));
		return err;
	}
	err = ufs_mtk_wait_link_state(hba, VS_LINK_UP, 100);
	if (err) {
		dev_err(hba->dev, "Failed to wait for link to be up: %pe\n", ERR_PTR(err));
		return err;
	}
	ufshcd_set_link_active(hba);

	err = ufshcd_make_hba_operational(hba);
	if (err)
		return err;

	if (hba->mcq_enabled) {
		ufs_mtk_config_mcq(hba, false);
		ufshcd_mcq_make_queues_operational(hba);
		ufshcd_mcq_config_mac(hba, hba->nutrs);
		ufshcd_mcq_enable(hba);
	}

	return 0;
}

static int ufs_mtk_link_set_lpm(struct ufs_hba *hba)
{
	int err;

	/* Disable reset confirm feature by UniPro */
	ufshcd_writel(hba,
		      (ufshcd_readl(hba, REG_UFS_XOUFS_CTRL) & ~0x100),
		      REG_UFS_XOUFS_CTRL);

	err = ufs_mtk_unipro_set_lpm(hba, true);
	if (err) {
		/* Resume UniPro state for following error recovery */
		ufs_mtk_unipro_set_lpm(hba, false);
		return err;
	}

	return 0;
}

static void ufs_mtk_vccqx_set_lpm(struct ufs_hba *hba, bool lpm)
{
	struct ufs_vreg *vccqx = NULL;

	if (!hba->vreg_info.vccq && !hba->vreg_info.vccq2)
		return;

	if (hba->vreg_info.vccq)
		vccqx = hba->vreg_info.vccq;
	else
		vccqx = hba->vreg_info.vccq2;

	regulator_set_mode(vccqx->reg,
			   lpm ? REGULATOR_MODE_IDLE : REGULATOR_MODE_NORMAL);
}

static void ufs_mtk_vsx_set_lpm(struct ufs_hba *hba, bool lpm)
{
	struct arm_smccc_res res;

	ufs_mtk_device_pwr_ctrl(!lpm,
				(unsigned long)hba->dev_info.wspecversion,
				res);
}

static void ufs_mtk_dev_vreg_set_lpm(struct ufs_hba *hba, bool lpm)
{
	bool skip_vccqx = false;

	/* Prevent entering LPM when device is still active */
	if (lpm && ufshcd_is_ufs_dev_active(hba))
		return;

	/* Skip vccqx lpm control and control vsx only */
	if (!hba->vreg_info.vccq && !hba->vreg_info.vccq2)
		skip_vccqx = true;

	/* VCC is always-on, control vsx only */
	if (!hba->vreg_info.vcc)
		skip_vccqx = true;

	/* Broken vcc keep vcc always on, most case control vsx only */
	if (lpm && hba->vreg_info.vcc && hba->vreg_info.vcc->enabled) {
		/* Some device vccqx/vsx can enter lpm */
		if (ufs_mtk_is_allow_vccqx_lpm(hba))
			skip_vccqx = false;
		else /* control vsx only */
			skip_vccqx = true;
	}

	if (lpm) {
		if (!skip_vccqx)
			ufs_mtk_vccqx_set_lpm(hba, lpm);
		ufs_mtk_vsx_set_lpm(hba, lpm);
	} else {
		ufs_mtk_vsx_set_lpm(hba, lpm);
		if (!skip_vccqx)
			ufs_mtk_vccqx_set_lpm(hba, lpm);
	}
}

static int ufs_mtk_suspend(struct ufs_hba *hba, enum ufs_pm_op pm_op,
	enum ufs_notify_change_status status)
{
	int err;
	struct arm_smccc_res res;
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	if (status == PRE_CHANGE) {
		if (ufshcd_is_auto_hibern8_supported(hba))
			return ufs_mtk_auto_hibern8_disable(hba);
		return 0;
	}

	if (ufshcd_is_link_hibern8(hba)) {
		err = ufs_mtk_link_set_lpm(hba);
		if (err)
			goto fail;
	}

	if (!ufshcd_is_link_active(hba)) {
		/*
		 * Make sure no error will be returned to prevent
		 * ufshcd_suspend() re-enabling regulators while vreg is still
		 * in low-power mode.
		 */
		err = ufs_mtk_mphy_power_on(hba, false);
		if (err)
			goto fail;
	}

	if (ufshcd_is_link_off(hba))
		ufs_mtk_device_reset_ctrl(0, res);

	ufs_mtk_sram_pwr_ctrl(false, res);

	/* Release pm_qos/clk if in scale-up mode during suspend */
	if (ufshcd_is_clkscaling_supported(hba) && (host->clk_scale_up)) {
		ufshcd_pm_qos_update(hba, false);
		_ufs_mtk_clk_scale(hba, false);
	} else if ((!ufshcd_is_clkscaling_supported(hba) &&
		    hba->pwr_info.gear_rx >= UFS_HS_G5)) {
		_ufs_mtk_clk_scale(hba, false);
	}

	err = regulator_bulk_disable(host->num_reg_misc, host->reg_misc);

	return err;
fail:
	/*
	 * Set link as off state enforcedly to trigger
	 * ufshcd_host_reset_and_restore() in ufshcd_suspend()
	 * for completed host reset.
	 */
	ufshcd_set_link_off(hba);
	return -EAGAIN;
}

static int ufs_mtk_resume(struct ufs_hba *hba, enum ufs_pm_op pm_op)
{
	int err;
	struct arm_smccc_res res;
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	err = regulator_bulk_enable(host->num_reg_misc, host->reg_misc);
	if (err)
		return err;

	if (hba->ufshcd_state != UFSHCD_STATE_OPERATIONAL)
		ufs_mtk_dev_vreg_set_lpm(hba, false);

	ufs_mtk_sram_pwr_ctrl(true, res);

	err = ufs_mtk_mphy_power_on(hba, true);
	if (err)
		goto fail;

	/* Request pm_qos/clk if in scale-up mode after resume */
	if (ufshcd_is_clkscaling_supported(hba) && (host->clk_scale_up)) {
		ufshcd_pm_qos_update(hba, true);
		_ufs_mtk_clk_scale(hba, true);
	} else if ((!ufshcd_is_clkscaling_supported(hba) &&
		    hba->pwr_info.gear_rx >= UFS_HS_G5)) {
		_ufs_mtk_clk_scale(hba, true);
	}

	if (ufshcd_is_link_hibern8(hba)) {
		err = ufs_mtk_link_set_hpm(hba);
		if (err)
			goto fail;
	}

	return 0;

fail:
	/*
	 * Check if the platform (parent) device has resumed, and ensure that
	 * power, clock, and MTCMOS are all turned on.
	 */
	err = ufshcd_link_recovery(hba);
	if (err) {
		dev_err(hba->dev, "Device PM: req=%d, status:%d, err:%d\n",
			hba->dev->power.request,
			hba->dev->power.runtime_status,
			hba->dev->power.runtime_error);
	}

	return 0; /* Cannot return a failure, otherwise, the I/O will hang. */
}

static void ufs_mtk_dbg_register_dump(struct ufs_hba *hba)
{
	/* Dump ufshci register 0x140 ~ 0x14C */
	ufshcd_dump_regs(hba, REG_UFS_XOUFS_CTRL, 0x10,
			 "XOUFS Ctrl (0x140): ");

	ufshcd_dump_regs(hba, REG_UFS_EXTREG, 0x4, "Ext Reg ");

	/* Dump ufshci register 0x2200 ~ 0x22AC */
	ufshcd_dump_regs(hba, REG_UFS_MPHYCTRL,
			 REG_UFS_REJECT_MON - REG_UFS_MPHYCTRL + 4,
			 "MPHY Ctrl (0x2200): ");

	/* Direct debugging information to REG_MTK_PROBE */
	ufs_mtk_dbg_sel(hba);
	ufshcd_dump_regs(hba, REG_UFS_PROBE, 0x4, "Debug Probe ");
}

static int ufs_mtk_apply_dev_quirks(struct ufs_hba *hba)
{
	struct ufs_dev_info *dev_info = &hba->dev_info;
	u16 mid = dev_info->wmanufacturerid;
	unsigned int cpu;

	if (hba->mcq_enabled) {
		/* Iterate all cpus to set affinity for mcq irqs */
		for (cpu = 0; cpu < nr_cpu_ids; cpu++)
			ufs_mtk_mcq_set_irq_affinity(hba, cpu);
	}

	if (mid == UFS_VENDOR_SAMSUNG) {
		ufshcd_dme_set(hba, UIC_ARG_MIB(PA_TACTIVATE), 6);
		ufshcd_dme_set(hba, UIC_ARG_MIB(PA_HIBERN8TIME), 10);
	} else if (mid == UFS_VENDOR_MICRON) {
		/* Only for the host which have TX skew issue */
		if (ufs_mtk_is_tx_skew_fix(hba) &&
			(STR_PRFX_EQUAL("MT128GBCAV2U31", dev_info->model) ||
			STR_PRFX_EQUAL("MT256GBCAV4U31", dev_info->model) ||
			STR_PRFX_EQUAL("MT512GBCAV8U31", dev_info->model) ||
			STR_PRFX_EQUAL("MT256GBEAX4U40", dev_info->model) ||
			STR_PRFX_EQUAL("MT512GAYAX4U40", dev_info->model) ||
			STR_PRFX_EQUAL("MT001TAYAX8U40", dev_info->model))) {
			ufshcd_dme_set(hba, UIC_ARG_MIB(PA_TACTIVATE), 8);
		}
	}

	/*
	 * Decide waiting time before gating reference clock and
	 * after ungating reference clock according to vendors'
	 * requirements.
	 */
	if (mid == UFS_VENDOR_SAMSUNG)
		ufs_mtk_setup_ref_clk_wait_us(hba, 1);
	else if (mid == UFS_VENDOR_SKHYNIX)
		ufs_mtk_setup_ref_clk_wait_us(hba, 30);
	else if (mid == UFS_VENDOR_TOSHIBA)
		ufs_mtk_setup_ref_clk_wait_us(hba, 100);
	else
		ufs_mtk_setup_ref_clk_wait_us(hba,
					      REFCLK_DEFAULT_WAIT_US);
	return 0;
}

static void ufs_mtk_fixup_dev_quirks(struct ufs_hba *hba)
{
	ufshcd_fixup_dev_quirks(hba, ufs_mtk_dev_fixups);

	if (ufs_mtk_is_broken_vcc(hba) && hba->vreg_info.vcc) {
		hba->vreg_info.vcc->always_on = true;
		/*
		 * VCC will be kept always-on thus we don't
		 * need any delay before putting device's VCC in LPM mode.
		 */
		hba->dev_quirks &= ~UFS_DEVICE_QUIRK_DELAY_BEFORE_LPM;
	}

	ufs_mtk_fix_ahit(hba);
	ufs_mtk_fix_clock_scaling(hba);
}

static void ufs_mtk_event_notify(struct ufs_hba *hba,
				 enum ufs_event_type evt, void *data)
{
	unsigned int val = *(u32 *)data;
	unsigned long reg;
	u8 bit;

	trace_ufs_mtk_event(evt, val);

	/* Print details of UIC Errors */
	if (evt <= UFS_EVT_DME_ERR) {
		dev_err(hba->dev, "Host UIC Error Code (%s): %08x\n",
			ufs_uic_err_str[evt], val);
		reg = val;
	}

	if (evt == UFS_EVT_PA_ERR) {
		for_each_set_bit(bit, &reg, ARRAY_SIZE(ufs_uic_pa_err_str))
			dev_err(hba->dev, "%s\n", ufs_uic_pa_err_str[bit]);
	}

	if (evt == UFS_EVT_DL_ERR) {
		for_each_set_bit(bit, &reg, ARRAY_SIZE(ufs_uic_dl_err_str))
			dev_err(hba->dev, "%s\n", ufs_uic_dl_err_str[bit]);
	}
}

static void ufs_mtk_config_scaling_param(struct ufs_hba *hba,
				struct devfreq_dev_profile *profile,
				struct devfreq_simple_ondemand_data *data)
{
	/* Customize min gear in clk scaling */
	hba->clk_scaling.min_gear = UFS_HS_G4;

	hba->vps->devfreq_profile.polling_ms = 200;
	hba->vps->ondemand_data.upthreshold = 50;
	hba->vps->ondemand_data.downdifferential = 20;
}

static void _ufs_mtk_clk_scale(struct ufs_hba *hba, bool scale_up)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct ufs_mtk_clk *mclk = &host->mclk;
	struct ufs_clk_info *clki = mclk->ufs_sel_clki;
	struct ufs_clk_info *fde_clki = mclk->ufs_fde_clki;
	int volt, ret = 0;
	bool clk_bind_vcore = false;
	bool clk_fde_scale = false;

	if (!hba->clk_scaling.is_initialized)
		return;

	if (!clki || !fde_clki)
		return;

	volt = host->mclk.vcore_volt;
	if (host->reg_vcore && volt)
		clk_bind_vcore = true;

	if (mclk->ufs_fde_max_clki && mclk->ufs_fde_min_clki)
		clk_fde_scale = true;

	ret = clk_prepare_enable(clki->clk);
	if (ret) {
		dev_err(hba->dev, "%s: Failed to enable clock: %pe\n", __func__, ERR_PTR(ret));
		return;
	}

	if (clk_fde_scale) {
		ret = clk_prepare_enable(fde_clki->clk);
		if (ret) {
			dev_err(hba->dev, "%s: Failed to enable FDE clock: %pe\n",
				__func__, ERR_PTR(ret));
			clk_disable_unprepare(clki->clk);
			return;
		}
	}

	if (scale_up) {
		if (clk_bind_vcore) {
			ret = regulator_set_voltage(host->reg_vcore, volt, INT_MAX);
			if (ret) {
				dev_err(hba->dev, "Failed to set vcore to %d\n", volt);
				goto out;
			}
		}

		ret = clk_set_parent(clki->clk, mclk->ufs_sel_max_clki->clk);
		if (ret) {
			dev_err(hba->dev, "%s: Failed to set clock mux: %pe\n",
				__func__, ERR_PTR(ret));
			goto out;
		}

		if (clk_fde_scale) {
			ret = clk_set_parent(fde_clki->clk, mclk->ufs_fde_max_clki->clk);
			if (ret) {
				dev_err(hba->dev, "%s: Failed to set fde clock mux: %pe\n",
					__func__, ERR_PTR(ret));
				goto out;
			}
		}
	} else {
		if (clk_fde_scale) {
			ret = clk_set_parent(fde_clki->clk, mclk->ufs_fde_min_clki->clk);
			if (ret) {
				dev_err(hba->dev, "%s: Failed to set fde clock mux: %pe\n",
					__func__, ERR_PTR(ret));
				goto out;
			}
		}

		ret = clk_set_parent(clki->clk, mclk->ufs_sel_min_clki->clk);
		if (ret) {
			dev_err(hba->dev, "%s: Failed to set clock mux: %pe\n",
				__func__, ERR_PTR(ret));
			goto out;
		}

		if (clk_bind_vcore) {
			ret = regulator_set_voltage(host->reg_vcore, 0, INT_MAX);
			if (ret) {
				dev_err(hba->dev, "%s: Failed to set vcore to minimum: %pe\n",
					__func__, ERR_PTR(ret));
			}
		}
	}

out:
	clk_disable_unprepare(clki->clk);

	if (clk_fde_scale)
		clk_disable_unprepare(fde_clki->clk);
}

/**
 * ufs_mtk_clk_scale - Internal clk scaling operation
 *
 * MTK platform supports clk scaling by switching parent of ufs_sel(mux).
 * The ufs_sel downstream to ufs_ck which feeds directly to UFS hardware.
 * Max and min clocks rate of ufs_sel defined in dts should match rate of
 * "ufs_sel_max_src" and "ufs_sel_min_src" respectively.
 * This prevent changing rate of pll clock that is shared between modules.
 *
 * @hba: per adapter instance
 * @scale_up: True for scaling up and false for scaling down
 */
static void ufs_mtk_clk_scale(struct ufs_hba *hba, bool scale_up)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct ufs_mtk_clk *mclk = &host->mclk;
	struct ufs_clk_info *clki = mclk->ufs_sel_clki;

	if (host->clk_scale_up == scale_up)
		goto out;

	if (scale_up)
		_ufs_mtk_clk_scale(hba, true);
	else
		_ufs_mtk_clk_scale(hba, false);

	host->clk_scale_up = scale_up;

	/* Must always set before clk_set_rate() */
	if (scale_up)
		clki->curr_freq = clki->max_freq;
	else
		clki->curr_freq = clki->min_freq;
out:
	trace_ufs_mtk_clk_scale(clki->name, scale_up, clk_get_rate(clki->clk));
}

static int ufs_mtk_clk_scale_notify(struct ufs_hba *hba, bool scale_up,
				    unsigned long target_freq,
				    enum ufs_notify_change_status status)
{
	if (!ufshcd_is_clkscaling_supported(hba))
		return 0;

	if (status == PRE_CHANGE) {
		/* Switch parent before clk_set_rate() */
		ufs_mtk_clk_scale(hba, scale_up);
	} else {
		/* Request interrupt latency QoS accordingly */
		ufs_mtk_scale_perf(hba, scale_up);
	}

	return 0;
}

static int ufs_mtk_get_hba_mac(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	/* MCQ operation not permitted */
	if (host->caps & UFS_MTK_CAP_DISABLE_MCQ)
		return -EPERM;

	return MAX_SUPP_MAC;
}

static int ufs_mtk_op_runtime_config(struct ufs_hba *hba)
{
	struct ufshcd_mcq_opr_info_t *opr;
	int i;

	hba->mcq_opr[OPR_SQD].offset = REG_UFS_MTK_SQD;
	hba->mcq_opr[OPR_SQIS].offset = REG_UFS_MTK_SQIS;
	hba->mcq_opr[OPR_CQD].offset = REG_UFS_MTK_CQD;
	hba->mcq_opr[OPR_CQIS].offset = REG_UFS_MTK_CQIS;

	for (i = 0; i < OPR_MAX; i++) {
		opr = &hba->mcq_opr[i];
		opr->stride = REG_UFS_MCQ_STRIDE;
		opr->base = hba->mmio_base + opr->offset;
	}

	return 0;
}

static int ufs_mtk_mcq_config_resource(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);

	/* fail mcq initialization if interrupt is not filled properly */
	if (!host->mcq_nr_intr) {
		dev_err(hba->dev, "IRQs not ready. MCQ disabled.");
		return -EINVAL;
	}

	hba->mcq_base = hba->mmio_base + MCQ_QUEUE_OFFSET(hba->mcq_capabilities);
	return 0;
}

static irqreturn_t ufs_mtk_mcq_intr(int irq, void *__intr_info)
{
	struct ufs_mtk_mcq_intr_info *mcq_intr_info = __intr_info;
	struct ufs_hba *hba = mcq_intr_info->hba;
	struct ufs_hw_queue *hwq;
	u32 events;
	int qid = mcq_intr_info->qid;

	hwq = &hba->uhq[qid];

	events = ufshcd_mcq_read_cqis(hba, qid);
	if (events)
		ufshcd_mcq_write_cqis(hba, events, qid);

	if (events & UFSHCD_MCQ_CQIS_TAIL_ENT_PUSH_STS)
		ufshcd_mcq_poll_cqe_lock(hba, hwq);

	return IRQ_HANDLED;
}

static int ufs_mtk_config_mcq_irq(struct ufs_hba *hba)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	u32 irq, i;
	int ret;

	for (i = 0; i < host->mcq_nr_intr; i++) {
		irq = host->mcq_intr_info[i].irq;
		if (irq == MTK_MCQ_INVALID_IRQ) {
			dev_err(hba->dev, "invalid irq. %d\n", i);
			return -ENOPARAM;
		}

		host->mcq_intr_info[i].qid = i;
		ret = devm_request_irq(hba->dev, irq, ufs_mtk_mcq_intr, 0, UFSHCD,
				       &host->mcq_intr_info[i]);

		dev_dbg(hba->dev, "request irq %d intr %s\n", irq, ret ? "failed" : "");

		if (ret) {
			dev_err(hba->dev, "Cannot request irq %d\n", ret);
			return ret;
		}
	}
	host->is_mcq_intr_enabled = true;

	return 0;
}

static int ufs_mtk_config_mcq(struct ufs_hba *hba, bool irq)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	int ret = 0;

	if (!host->mcq_set_intr) {
		/* Disable irq option register */
		ufshcd_rmwl(hba, MCQ_INTR_EN_MSK, 0, REG_UFS_MMIO_OPT_CTRL_0);

		if (irq) {
			ret = ufs_mtk_config_mcq_irq(hba);
			if (ret)
				return ret;
		}

		host->mcq_set_intr = true;
	}

	ufshcd_rmwl(hba, MCQ_AH8, MCQ_AH8, REG_UFS_MMIO_OPT_CTRL_0);
	ufshcd_rmwl(hba, MCQ_INTR_EN_MSK, MCQ_MULTI_INTR_EN, REG_UFS_MMIO_OPT_CTRL_0);

	return 0;
}

static int ufs_mtk_config_esi(struct ufs_hba *hba)
{
	return ufs_mtk_config_mcq(hba, true);
}

static void ufs_mtk_config_scsi_dev(struct scsi_device *sdev)
{
	struct ufs_hba *hba = shost_priv(sdev->host);

	dev_dbg(hba->dev, "lu %llu scsi device configured", sdev->lun);
	if (sdev->lun == 2)
		blk_queue_flag_set(QUEUE_FLAG_SAME_FORCE, sdev->request_queue);
}

/*
 * struct ufs_hba_mtk_vops - UFS MTK specific variant operations
 *
 * The variant operations configure the necessary controller and PHY
 * handshake during initialization.
 */
static const struct ufs_hba_variant_ops ufs_hba_mtk_vops = {
	.name                = "mediatek.ufshci",
	.max_num_rtt         = MTK_MAX_NUM_RTT,
	.init                = ufs_mtk_init,
	.exit                = ufs_mtk_exit,
	.get_ufs_hci_version = ufs_mtk_get_ufs_hci_version,
	.setup_clocks        = ufs_mtk_setup_clocks,
	.hce_enable_notify   = ufs_mtk_hce_enable_notify,
	.link_startup_notify = ufs_mtk_link_startup_notify,
	.pwr_change_notify   = ufs_mtk_pwr_change_notify,
	.apply_dev_quirks    = ufs_mtk_apply_dev_quirks,
	.fixup_dev_quirks    = ufs_mtk_fixup_dev_quirks,
	.suspend             = ufs_mtk_suspend,
	.resume              = ufs_mtk_resume,
	.dbg_register_dump   = ufs_mtk_dbg_register_dump,
	.device_reset        = ufs_mtk_device_reset,
	.event_notify        = ufs_mtk_event_notify,
	.config_scaling_param = ufs_mtk_config_scaling_param,
	.clk_scale_notify    = ufs_mtk_clk_scale_notify,
	/* mcq vops */
	.get_hba_mac         = ufs_mtk_get_hba_mac,
	.op_runtime_config   = ufs_mtk_op_runtime_config,
	.mcq_config_resource = ufs_mtk_mcq_config_resource,
	.config_esi          = ufs_mtk_config_esi,
	.config_scsi_dev     = ufs_mtk_config_scsi_dev,
};

static const char *const ufs_mtk_regs_avdd12_avdd18[] = {
	"avdd12", "avdd18"
};

static const char *const ufs_mtk_regs_avdd12_ckbuf_avdd18[] = {
	"avdd12", "avdd12-ckbuf", "avdd18"
};

static const struct ufs_mtk_soc_data mt8183_data = {
	.has_avdd09 = true,
	.reg_names = ufs_mtk_regs_avdd12_avdd18,
	.num_reg_names = ARRAY_SIZE(ufs_mtk_regs_avdd12_avdd18),
};

static const struct ufs_mtk_soc_data mt8192_8195_data = {
	.has_avdd09 = false,
	.reg_names = ufs_mtk_regs_avdd12_ckbuf_avdd18,
	.num_reg_names = ARRAY_SIZE(ufs_mtk_regs_avdd12_ckbuf_avdd18),
};

static const struct of_device_id ufs_mtk_of_match[] = {
	{ .compatible = "mediatek,mt8183-ufshci", .data = &mt8183_data },
	{ .compatible = "mediatek,mt8192-ufshci", .data = &mt8192_8195_data },
	{ .compatible = "mediatek,mt8195-ufshci", .data = &mt8192_8195_data },
	{},
};
MODULE_DEVICE_TABLE(of, ufs_mtk_of_match);

/**
 * ufs_mtk_probe - probe routine of the driver
 * @pdev: pointer to Platform device handle
 *
 * Return: zero for success and non-zero for failure.
 */
static int ufs_mtk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ufs_hba *hba;
	int ret;

	/* perform generic probe */
	ret = ufshcd_pltfrm_init(pdev, &ufs_hba_mtk_vops);
	if (ret)
		return dev_err_probe(dev, ret, "Generic platform probe failed\n");

	hba = platform_get_drvdata(pdev);

	/*
	 * Because the default power setting of VSx (the upper layer of
	 * VCCQ/VCCQ2) is HWLP, we need to prevent VCCQ/VCCQ2 from
	 * entering LPM.
	 */
	ufs_mtk_dev_vreg_set_lpm(hba, false);

	return 0;
}

/**
 * ufs_mtk_remove - set driver_data of the device to NULL
 * @pdev: pointer to platform device handle
 */
static void ufs_mtk_remove(struct platform_device *pdev)
{
	ufshcd_pltfrm_remove(pdev);
}

static int ufs_mtk_system_suspend(struct device *dev)
{
	struct ufs_hba *hba = dev_get_drvdata(dev);
	struct arm_smccc_res res;
	int ret;

	if (hba->shutting_down) {
		ret = -EBUSY;
		goto out;
	}

	ret = ufshcd_system_suspend(dev);
	if (ret)
		goto out;

	if (pm_runtime_suspended(hba->dev))
		goto out;

	ufs_mtk_dev_vreg_set_lpm(hba, true);

	if (ufs_mtk_is_rtff_mtcmos(hba))
		ufs_mtk_mtcmos_ctrl(false, res);

out:
	return ret;
}

static int ufs_mtk_system_resume(struct device *dev)
{
	int ret = 0;
	struct ufs_hba *hba = dev_get_drvdata(dev);
	struct arm_smccc_res res;

	if (pm_runtime_suspended(hba->dev))
		goto out;

	if (ufs_mtk_is_rtff_mtcmos(hba))
		ufs_mtk_mtcmos_ctrl(true, res);

	ufs_mtk_dev_vreg_set_lpm(hba, false);

out:
	ret = ufshcd_system_resume(dev);

	return ret;
}

static int ufs_mtk_runtime_suspend(struct device *dev)
{
	struct ufs_hba *hba = dev_get_drvdata(dev);
	struct arm_smccc_res res;
	int ret;

	ret = ufshcd_runtime_suspend(dev);
	if (ret)
		return ret;

	ufs_mtk_dev_vreg_set_lpm(hba, true);

	if (ufs_mtk_is_rtff_mtcmos(hba))
		ufs_mtk_mtcmos_ctrl(false, res);

	ret = ufs_mtk_mphy_power_on(hba, false);
	if (ret) {
		dev_err(dev, "Failed to power off PHY: %pe\n", ERR_PTR(ret));
		return ret;
	}

	return 0;
}

static int ufs_mtk_runtime_resume(struct device *dev)
{
	struct ufs_hba *hba = dev_get_drvdata(dev);
	struct arm_smccc_res res;
	int ret;

	if (ufs_mtk_is_rtff_mtcmos(hba))
		ufs_mtk_mtcmos_ctrl(true, res);

	ret = ufs_mtk_mphy_power_on(hba, true);
	if (ret) {
		dev_err(dev, "Failed to power on PHY: %pe\n", ERR_PTR(ret));
		return ret;
	}

	ufs_mtk_dev_vreg_set_lpm(hba, false);

	return ufshcd_runtime_resume(dev);
}

static const struct dev_pm_ops ufs_mtk_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(ufs_mtk_system_suspend, ufs_mtk_system_resume)
	RUNTIME_PM_OPS(ufs_mtk_runtime_suspend, ufs_mtk_runtime_resume, NULL)
	.prepare	 = ufshcd_suspend_prepare,
	.complete	 = ufshcd_resume_complete,
};

static struct platform_driver ufs_mtk_pltform = {
	.probe      = ufs_mtk_probe,
	.remove = ufs_mtk_remove,
	.driver = {
		.name   = "ufshcd-mtk",
		.pm     = pm_ptr(&ufs_mtk_pm_ops),
		.of_match_table = ufs_mtk_of_match,
	},
};

MODULE_AUTHOR("Stanley Chu <stanley.chu@mediatek.com>");
MODULE_AUTHOR("Peter Wang <peter.wang@mediatek.com>");
MODULE_DESCRIPTION("MediaTek UFS Host Driver");
MODULE_LICENSE("GPL v2");

module_platform_driver(ufs_mtk_pltform);
