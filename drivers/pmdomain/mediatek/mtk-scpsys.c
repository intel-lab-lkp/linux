// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 Pengutronix, Sascha Hauer <kernel@pengutronix.de>
 */
#include <linux/clk.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/soc/mediatek/infracfg.h>

#include <dt-bindings/power/mt2701-power.h>
#include <dt-bindings/power/mt2712-power.h>
#include <dt-bindings/power/mt6797-power.h>
#include <dt-bindings/power/mt7622-power.h>
#include <dt-bindings/power/mt7623a-power.h>
#include <dt-bindings/power/mt8173-power.h>

#define MTK_POLL_DELAY_US   10
#define MTK_POLL_TIMEOUT    USEC_PER_SEC
#define MTK_POLL_TIMEOUT_300MS		(300 * USEC_PER_MSEC)
#define MTK_POLL_IRQ_TIMEOUT		USEC_PER_SEC
#define MTK_POLL_VOTE_PREPARE_CNT	2500
#define MTK_POLL_VOTE_PREPARE_US	2
#define MTK_ACK_DELAY_US		50
#define MTK_RTFF_DELAY_US		10
#define MTK_STABLE_DELAY_US		100

#define MTK_BUS_PROTECTION_RETY_TIMES	10

#define MTK_SCPD_ACTIVE_WAKEUP		BIT(0)
#define MTK_SCPD_FWAIT_SRAM		BIT(1)
#define MTK_SCPD_SRAM_ISO		BIT(2)
#define MTK_SCPD_SRAM_SLP		BIT(3)
#define MTK_SCPD_BYPASS_INIT_ON		BIT(4)
#define MTK_SCPD_IS_PWR_CON_ON		BIT(5)
#define MTK_SCPD_VOTE_OPS		BIT(6)
#define MTK_SCPD_NON_CPU_RTFF		BIT(7)
#define MTK_SCPD_PEXTP_PHY_RTFF		BIT(8)
#define MTK_SCPD_UFS_RTFF		BIT(9)
#define MTK_SCPD_RTFF_DELAY		BIT(10)
#define MTK_SCPD_IRQ_SAFE		BIT(11)
#define MTK_SCPD_ALWAYS_ON		BIT(12)
#define MTK_SCPD_CAPS(_scpd, _x)	((_scpd)->data->caps & (_x))

#define SPM_VDE_PWR_CON			0x0210
#define SPM_MFG_PWR_CON			0x0214
#define SPM_VEN_PWR_CON			0x0230
#define SPM_ISP_PWR_CON			0x0238
#define SPM_DIS_PWR_CON			0x023c
#define SPM_CONN_PWR_CON		0x0280
#define SPM_VEN2_PWR_CON		0x0298
#define SPM_AUDIO_PWR_CON		0x029c	/* MT8173, MT2712 */
#define SPM_BDP_PWR_CON			0x029c	/* MT2701 */
#define SPM_ETH_PWR_CON			0x02a0
#define SPM_HIF_PWR_CON			0x02a4
#define SPM_IFR_MSC_PWR_CON		0x02a8
#define SPM_MFG_2D_PWR_CON		0x02c0
#define SPM_MFG_ASYNC_PWR_CON		0x02c4
#define SPM_USB_PWR_CON			0x02cc
#define SPM_USB2_PWR_CON		0x02d4	/* MT2712 */
#define SPM_ETHSYS_PWR_CON		0x02e0	/* MT7622 */
#define SPM_HIF0_PWR_CON		0x02e4	/* MT7622 */
#define SPM_HIF1_PWR_CON		0x02e8	/* MT7622 */
#define SPM_WB_PWR_CON			0x02ec	/* MT7622 */

#define SPM_PWR_STATUS			0x060c
#define SPM_PWR_STATUS_2ND		0x0610

#define PWR_RST_B_BIT			BIT(0)
#define PWR_ISO_BIT			BIT(1)
#define PWR_ON_BIT			BIT(2)
#define PWR_ON_2ND_BIT			BIT(3)
#define PWR_CLK_DIS_BIT			BIT(4)
#define PWR_SRAM_CLKISO_BIT		BIT(5)
#define PWR_SRAM_ISOINT_B_BIT		BIT(6)
#define PWR_RTFF_SAVE			BIT(24)
#define PWR_RTFF_NRESTORE		BIT(25)
#define PWR_RTFF_CLK_DIS		BIT(26)
#define PWR_RTFF_SAVE_FLAG		BIT(27)
#define PWR_RTFF_UFS_CLK_DIS		BIT(28)
#define PWR_ACK				BIT(30)
#define PWR_ACK_2ND			BIT(31)

#define PWR_STATUS_CONN			BIT(1)
#define PWR_STATUS_DISP			BIT(3)
#define PWR_STATUS_MFG			BIT(4)
#define PWR_STATUS_ISP			BIT(5)
#define PWR_STATUS_VDEC			BIT(7)
#define PWR_STATUS_BDP			BIT(14)
#define PWR_STATUS_ETH			BIT(15)
#define PWR_STATUS_HIF			BIT(16)
#define PWR_STATUS_IFR_MSC		BIT(17)
#define PWR_STATUS_USB2			BIT(19)	/* MT2712 */
#define PWR_STATUS_VENC_LT		BIT(20)
#define PWR_STATUS_VENC			BIT(21)
#define PWR_STATUS_MFG_2D		BIT(22)	/* MT8173 */
#define PWR_STATUS_MFG_ASYNC		BIT(23)	/* MT8173 */
#define PWR_STATUS_AUDIO		BIT(24)	/* MT8173, MT2712 */
#define PWR_STATUS_USB			BIT(25)	/* MT8173, MT2712 */
#define PWR_STATUS_ETHSYS		BIT(24)	/* MT7622 */
#define PWR_STATUS_HIF0			BIT(25)	/* MT7622 */
#define PWR_STATUS_HIF1			BIT(26)	/* MT7622 */
#define PWR_STATUS_WB			BIT(27)	/* MT7622 */

#define _BUS_PROT(_type, _set_ofs, _clr_ofs,			\
		_en_ofs, _sta_ofs, _mask, _ack_mask,		\
		_ignore_clr_ack) {				\
		.type = _type,					\
		.set_ofs = _set_ofs,				\
		.clr_ofs = _clr_ofs,				\
		.en_ofs = _en_ofs,				\
		.sta_ofs = _sta_ofs,				\
		.mask = _mask,					\
		.ack_mask = _ack_mask,				\
		.ignore_clr_ack = _ignore_clr_ack,		\
	}

#define BUS_PROT_IGN(_type, _set_ofs, _clr_ofs,	\
		_en_ofs, _sta_ofs, _mask)		\
		_BUS_PROT(_type, _set_ofs, _clr_ofs,	\
		_en_ofs, _sta_ofs, _mask, _mask, true)

enum clk_id {
	CLK_NONE,
	CLK_MM,
	CLK_MFG,
	CLK_VENC,
	CLK_VENC_LT,
	CLK_ETHIF,
	CLK_VDEC,
	CLK_HIFSEL,
	CLK_JPGDEC,
	CLK_AUDIO,
	CLK_MAX,
};

static const char * const clk_names[] = {
	NULL,
	"mm",
	"mfg",
	"venc",
	"venc_lt",
	"ethif",
	"vdec",
	"hif_sel",
	"jpgdec",
	"audio",
	NULL,
};

#define MAX_CLKS	3
#define MAX_STEPS	3

struct bus_prot {
	u32 type;
	u32 set_ofs;
	u32 clr_ofs;
	u32 en_ofs;
	u32 sta_ofs;
	u32 mask;
	u32 ack_mask;
	bool ignore_clr_ack;
};

/**
 * struct scp_domain_data - scp domain data for power on/off flow
 * @name: The domain name.
 * @vote_comp: The vote name.
 * @sta_mask: The mask for power on/off status bit.
 * @ctl_offs: The offset for main power control register.
 * @vote_done_ofs: The offset for vote done register.
 * @vote_ofs: The offset for vote register.
 * @vote_set_ofs: The offset for vote set register.
 * @vote_clr_ofs: The offset for vote clear register.
 * @vote_en_ofs: The offset for voted register.
 * @vote_set_sta_ofs: The offset for vote set status register.
 * @vote_clr_sta_ofs: The offset for vote clear status register.
 * @vote_ack_ofs: The offset for power control ack register.
 * @vote_shift: The bit of vote.
 * @sram_pdn_bits: The mask for sram power control bits.
 * @sram_pdn_ack_bits: The mask for sram power control acked bits.
 * @sram_slp_bits: The mask for sram low power control bits.
 * @sram_slp_ack_bits: The mask for sram low power control acked bits.
 * @bus_prot_mask: The mask for single step bus protection.
 * @clk_id: The basic clocks required by this power domain.
 * @bp_table: The bus protect configs for the power domain.
 * @caps: The flag for active wake-up action.
 */
struct scp_domain_data {
	const char *name;
	const char *vote_comp;
	u32 sta_mask;
	int ctl_offs;
	u32 vote_done_ofs;
	u32 vote_ofs;
	u32 vote_set_ofs;
	u32 vote_clr_ofs;
	u32 vote_en_ofs;
	u32 vote_set_sta_ofs;
	u32 vote_clr_sta_ofs;
	u32 vote_ack_ofs;
	u8 vote_shift;
	u32 sram_pdn_bits;
	u32 sram_pdn_ack_bits;
	u32 sram_slp_bits;
	u32 sram_slp_ack_bits;
	u32 bus_prot_mask;
	enum clk_id clk_id[MAX_CLKS];
	struct bus_prot bp_table[MAX_STEPS];
	u32 caps;
};

struct scp;

struct scp_domain {
	struct generic_pm_domain genpd;
	struct scp *scp;
	struct clk *clk[MAX_CLKS];
	const struct scp_domain_data *data;
	struct regulator *supply;
	struct regmap *vote_regmap;
	bool rtff_flag;
};

struct scp_ctrl_reg {
	int pwr_sta_offs;
	int pwr_sta2nd_offs;
};

struct scp {
	struct scp_domain *domains;
	struct genpd_onecell_data pd_data;
	struct device *dev;
	void __iomem *base;
	struct regmap *infracfg;
	struct scp_ctrl_reg ctrl_reg;
	bool bus_prot_reg_update;
	struct regmap **bp_regmap;
	int num_bp;
};

struct scp_subdomain {
	int origin;
	int subdomain;
};

typedef int (*scp_soc_post_probe_fn)(struct platform_device *pdev,
		struct scp *scp);

struct scp_soc_data {
	const struct scp_domain_data *domains;
	int num_domains;
	const struct scp_subdomain *subdomains;
	int num_subdomains;
	const struct scp_ctrl_reg regs;
	bool bus_prot_reg_update;
	const char **bp_list;
	int num_bp;
	scp_soc_post_probe_fn post_probe;
};

static int scpsys_domain_is_on(struct scp_domain *scpd)
{
	struct scp *scp = scpd->scp;

	u32 status = readl(scp->base + scp->ctrl_reg.pwr_sta_offs) &
						scpd->data->sta_mask;
	u32 status2 = readl(scp->base + scp->ctrl_reg.pwr_sta2nd_offs) &
						scpd->data->sta_mask;

	/*
	 * A domain is on when both status bits are set. If only one is set
	 * return an error. This happens while powering up a domain
	 */

	if (status && status2)
		return true;
	if (!status && !status2)
		return false;

	return -EINVAL;
}

static int scpsys_pwr_ack_is_on(struct scp_domain *scpd)
{
	u32 status = readl(scpd->scp->base + scpd->data->ctl_offs) & PWR_ACK;

	return status ? true : false;
}

static int scpsys_pwr_ack_2nd_is_on(struct scp_domain *scpd)
{
	u32 status = readl(scpd->scp->base + scpd->data->ctl_offs) & PWR_ACK_2ND;

	return status ? true : false;
}

static int scpsys_regulator_enable(struct scp_domain *scpd)
{
	if (!scpd->supply)
		return 0;

	return regulator_enable(scpd->supply);
}

static int scpsys_regulator_disable(struct scp_domain *scpd)
{
	if (!scpd->supply)
		return 0;

	return regulator_disable(scpd->supply);
}

static void scpsys_clk_disable(struct clk *clk[], int max_num)
{
	int i;

	for (i = max_num - 1; i >= 0; i--)
		clk_disable_unprepare(clk[i]);
}

static int scpsys_clk_enable(struct clk *clk[], int max_num)
{
	int i, ret = 0;

	for (i = 0; i < max_num && clk[i]; i++) {
		ret = clk_prepare_enable(clk[i]);
		if (ret) {
			scpsys_clk_disable(clk, i);
			break;
		}
	}

	return ret;
}

static int scpsys_sram_enable(struct scp_domain *scpd, void __iomem *ctl_addr)
{
	u32 val;
	u32 ack_mask, ack_sta;
	int tmp;

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_SRAM_SLP)) {
		ack_mask = scpd->data->sram_slp_ack_bits;
		ack_sta = ack_mask;
		val = readl(ctl_addr) | scpd->data->sram_slp_bits;
	} else {
		ack_mask = scpd->data->sram_pdn_ack_bits;
		ack_sta = 0;
		val = readl(ctl_addr) & ~scpd->data->sram_pdn_bits;
	}

	writel(val, ctl_addr);

	/* Either wait until SRAM_PDN_ACK all 0 or have a force wait */
	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_FWAIT_SRAM)) {
		/*
		 * Currently, MTK_SCPD_FWAIT_SRAM is necessary only for
		 * MT7622_POWER_DOMAIN_WB and thus just a trivial setup
		 * is applied here.
		 */
		usleep_range(12000, 12100);
	} else {
		/* Either wait until SRAM_PDN_ACK all 1 or 0 */
		int ret = readl_poll_timeout(ctl_addr, tmp,
				(tmp & ack_mask) == ack_sta,
				MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
		if (ret < 0)
			return ret;
	}

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_SRAM_ISO)) {
		val = readl(ctl_addr) | PWR_SRAM_ISOINT_B_BIT;
		writel(val, ctl_addr);
		udelay(1);
		val &= ~PWR_SRAM_CLKISO_BIT;
		writel(val, ctl_addr);
	}

	return 0;
}

static int scpsys_sram_disable(struct scp_domain *scpd, void __iomem *ctl_addr)
{
	u32 val;
	u32 ack_mask, ack_sta;
	int tmp;

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_SRAM_ISO)) {
		val = readl(ctl_addr) | PWR_SRAM_CLKISO_BIT;
		writel(val, ctl_addr);
		val &= ~PWR_SRAM_ISOINT_B_BIT;
		writel(val, ctl_addr);
		udelay(1);
	}

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_SRAM_SLP)) {
		ack_mask = scpd->data->sram_slp_ack_bits;
		ack_sta = 0;
		val = readl(ctl_addr) & ~scpd->data->sram_slp_bits;
	} else {
		ack_mask = scpd->data->sram_pdn_ack_bits;
		ack_sta = ack_mask;
		val = readl(ctl_addr) | scpd->data->sram_pdn_bits;
	}
	writel(val, ctl_addr);

	/* Either wait until SRAM_PDN_ACK all 1 or 0 */
	return readl_poll_timeout(ctl_addr, tmp,
			(tmp & ack_mask) == ack_sta,
			MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
}

static int set_bus_protection(struct regmap *map, struct bus_prot *bp)
{
	u32 val = 0;
	int retry = 0;
	int ret = 0;

	while (retry <= MTK_BUS_PROTECTION_RETY_TIMES) {
		if (bp->set_ofs)
			regmap_write(map,  bp->set_ofs, bp->mask);
		else
			regmap_update_bits(map, bp->en_ofs, bp->mask, bp->mask);

		/* check bus protect enable setting */
		regmap_read(map, bp->en_ofs, &val);
		if ((val & bp->mask) == bp->mask)
			break;

		retry++;
	}

	ret = regmap_read_poll_timeout_atomic(map, bp->sta_ofs, val,
					      (val & bp->ack_mask) == bp->ack_mask,
					      MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	if (ret < 0) {
		pr_err("%s val=0x%x, mask=0x%x, (val & mask)=0x%x\n",
		       __func__, val, bp->ack_mask, (val & bp->ack_mask));
	}

	return ret;
}

static int clear_bus_protection(struct regmap *map, struct bus_prot *bp)
{
	u32 val = 0;
	int ret = 0;

	if (bp->clr_ofs)
		regmap_write(map, bp->clr_ofs, bp->mask);
	else
		regmap_update_bits(map, bp->en_ofs, bp->mask, 0);

	if (bp->ignore_clr_ack)
		return 0;

	ret = regmap_read_poll_timeout_atomic(map, bp->sta_ofs, val,
					      !(val & bp->ack_mask),
					      MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	if (ret < 0) {
		pr_err("%s val=0x%x, mask=0x%x, (val & mask)=0x%x\n",
		       __func__, val, bp->ack_mask, (val & bp->ack_mask));
	}
	return ret;
}

static int scpsys_bus_protect_table_disable(struct scp_domain *scpd, unsigned int index)
{
	struct scp *scp = scpd->scp;
	const struct bus_prot *bp_table = scpd->data->bp_table;
	int ret = 0;
	int i;

	for (i = index; i >= 0; i--) {
		struct regmap *map;
		struct bus_prot bp = bp_table[i];

		if (bp.type == 0 || bp.type >= scp->num_bp)
			continue;

		map = scp->bp_regmap[bp.type];
		if (!map)
			continue;

		ret = clear_bus_protection(map, &bp);
		if (ret)
			break;
	}

	return ret;
}

static int scpsys_bus_protect_table_enable(struct scp_domain *scpd)
{
	struct scp *scp = scpd->scp;
	const struct bus_prot *bp_table = scpd->data->bp_table;
	int ret = 0;
	int i;

	for (i = 0; i < MAX_STEPS; i++) {
		struct regmap *map;
		struct bus_prot bp = bp_table[i];

		if (bp.type == 0 || bp.type >= scp->num_bp)
			continue;

		map = scp->bp_regmap[bp.type];
		if (!map)
			continue;

		ret = set_bus_protection(map, &bp);
		if (ret) {
			scpsys_bus_protect_table_disable(scpd, i);
			return ret;
		}
	}

	return ret;
}

static int scpsys_bus_protect_enable(struct scp_domain *scpd)
{
	struct scp *scp = scpd->scp;

	if (scp->bp_regmap && scp->num_bp > 0)
		return scpsys_bus_protect_table_enable(scpd);

	if (!scpd->data->bus_prot_mask)
		return 0;

	return mtk_infracfg_set_bus_protection(scp->infracfg,
			scpd->data->bus_prot_mask,
			scp->bus_prot_reg_update);
}

static int scpsys_bus_protect_disable(struct scp_domain *scpd)
{
	struct scp *scp = scpd->scp;

	if (scp->bp_regmap && scp->num_bp > 0)
		return scpsys_bus_protect_table_disable(scpd, MAX_STEPS - 1);

	if (!scpd->data->bus_prot_mask)
		return 0;

	return mtk_infracfg_clear_bus_protection(scp->infracfg,
			scpd->data->bus_prot_mask,
			scp->bus_prot_reg_update);
}

static int scpsys_power_on(struct generic_pm_domain *genpd)
{
	struct scp_domain *scpd = container_of(genpd, struct scp_domain, genpd);
	struct scp *scp = scpd->scp;
	void __iomem *ctl_addr = scp->base + scpd->data->ctl_offs;
	u32 val;
	int ret, tmp;

	ret = scpsys_regulator_enable(scpd);
	if (ret < 0)
		return ret;

	ret = scpsys_clk_enable(scpd->clk, MAX_CLKS);
	if (ret)
		goto err_clk;

	/* subsys power on */
	val = readl(ctl_addr);
	val |= PWR_ON_BIT;
	writel(val, ctl_addr);
	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_IS_PWR_CON_ON)) {
		ret = readx_poll_timeout_atomic(scpsys_pwr_ack_is_on, scpd, tmp, tmp > 0,
						MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
		if (ret < 0)
			goto err_pwr_ack;

		udelay(MTK_ACK_DELAY_US);
	}

	val |= PWR_ON_2ND_BIT;
	writel(val, ctl_addr);

	/* wait until PWR_ACK = 1 */
	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_IS_PWR_CON_ON))
		ret = readx_poll_timeout_atomic(scpsys_pwr_ack_2nd_is_on, scpd, tmp, tmp > 0,
						MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	else
		ret = readx_poll_timeout(scpsys_domain_is_on, scpd, tmp, tmp > 0,
					 MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	if (ret < 0)
		goto err_pwr_ack;

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_PEXTP_PHY_RTFF) && scpd->rtff_flag) {
		val |= PWR_RTFF_CLK_DIS;
		writel(val, ctl_addr);
	}

	val &= ~PWR_CLK_DIS_BIT;
	writel(val, ctl_addr);

	val &= ~PWR_ISO_BIT;
	writel(val, ctl_addr);

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_RTFF_DELAY) && scpd->rtff_flag)
		udelay(MTK_RTFF_DELAY_US);

	val |= PWR_RST_B_BIT;
	writel(val, ctl_addr);

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_NON_CPU_RTFF)) {
		val = readl(ctl_addr);
		if (val & PWR_RTFF_SAVE_FLAG) {
			val &= ~PWR_RTFF_SAVE_FLAG;
			writel(val, ctl_addr);

			val |= PWR_RTFF_CLK_DIS;
			writel(val, ctl_addr);

			val &= ~PWR_RTFF_NRESTORE;
			writel(val, ctl_addr);

			val |= PWR_RTFF_NRESTORE;
			writel(val, ctl_addr);

			val &= ~PWR_RTFF_CLK_DIS;
			writel(val, ctl_addr);
		}
	} else if (MTK_SCPD_CAPS(scpd, MTK_SCPD_PEXTP_PHY_RTFF)) {
		val = readl(ctl_addr);
		if (val & PWR_RTFF_SAVE_FLAG) {
			val &= ~PWR_RTFF_SAVE_FLAG;
			writel(val, ctl_addr);

			val &= ~PWR_RTFF_NRESTORE;
			writel(val, ctl_addr);

			val |= PWR_RTFF_NRESTORE;
			writel(val, ctl_addr);

			val &= ~PWR_RTFF_CLK_DIS;
			writel(val, ctl_addr);
		}
	} else if (MTK_SCPD_CAPS(scpd, MTK_SCPD_UFS_RTFF) && scpd->rtff_flag) {
		val |= PWR_RTFF_UFS_CLK_DIS;
		writel(val, ctl_addr);

		val &= ~PWR_RTFF_NRESTORE;
		writel(val, ctl_addr);

		val |= PWR_RTFF_NRESTORE;
		writel(val, ctl_addr);

		val &= ~PWR_RTFF_UFS_CLK_DIS;
		writel(val, ctl_addr);

		scpd->rtff_flag = false;
	}

	ret = scpsys_sram_enable(scpd, ctl_addr);
	if (ret < 0)
		goto err_pwr_ack;

	ret = scpsys_bus_protect_disable(scpd);
	if (ret < 0)
		goto err_pwr_ack;

	return 0;

err_pwr_ack:
	scpsys_clk_disable(scpd->clk, MAX_CLKS);
err_clk:
	scpsys_regulator_disable(scpd);

	dev_err(scp->dev, "Failed to power on domain %s\n", genpd->name);

	return ret;
}

static int scpsys_power_off(struct generic_pm_domain *genpd)
{
	struct scp_domain *scpd = container_of(genpd, struct scp_domain, genpd);
	struct scp *scp = scpd->scp;
	void __iomem *ctl_addr = scp->base + scpd->data->ctl_offs;
	u32 val;
	int ret, tmp;

	ret = scpsys_bus_protect_enable(scpd);
	if (ret < 0)
		goto out;

	ret = scpsys_sram_disable(scpd, ctl_addr);
	if (ret < 0)
		goto out;

	/* subsys power off */
	val = readl(ctl_addr);

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_NON_CPU_RTFF) ||
	    MTK_SCPD_CAPS(scpd, MTK_SCPD_PEXTP_PHY_RTFF)) {
		val |= PWR_RTFF_CLK_DIS;
		writel(val, ctl_addr);

		val |= PWR_RTFF_SAVE;
		writel(val, ctl_addr);

		val &= ~PWR_RTFF_SAVE;
		writel(val, ctl_addr);

		val &= ~PWR_RTFF_CLK_DIS;
		writel(val, ctl_addr);

		val |= PWR_RTFF_SAVE_FLAG;
		writel(val, ctl_addr);
	} else if (MTK_SCPD_CAPS(scpd, MTK_SCPD_UFS_RTFF)) {
		val |= PWR_RTFF_UFS_CLK_DIS;
		writel(val, ctl_addr);

		val |= PWR_RTFF_SAVE;
		writel(val, ctl_addr);

		val &= ~PWR_RTFF_SAVE;
		writel(val, ctl_addr);

		val &= ~PWR_RTFF_UFS_CLK_DIS;
		writel(val, ctl_addr);
		if (MTK_SCPD_CAPS(scpd, MTK_SCPD_UFS_RTFF))
			scpd->rtff_flag = true;
	}

	val |= PWR_ISO_BIT;
	writel(val, ctl_addr);

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_RTFF_DELAY) && scpd->rtff_flag)
		udelay(1);

	val &= ~PWR_RST_B_BIT;
	writel(val, ctl_addr);

	val |= PWR_CLK_DIS_BIT;
	writel(val, ctl_addr);

	val &= ~PWR_ON_BIT;
	writel(val, ctl_addr);

	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_IS_PWR_CON_ON)) {
		ret = readx_poll_timeout_atomic(scpsys_pwr_ack_is_on, scpd, tmp, tmp == 0,
						MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
		if (ret < 0)
			goto out;
	}

	val &= ~PWR_ON_2ND_BIT;
	writel(val, ctl_addr);

	/* wait until PWR_ACK = 0 */
	if (MTK_SCPD_CAPS(scpd, MTK_SCPD_IS_PWR_CON_ON))
		ret = readx_poll_timeout_atomic(scpsys_pwr_ack_2nd_is_on, scpd, tmp, tmp == 0,
						MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	else
		ret = readx_poll_timeout(scpsys_domain_is_on, scpd, tmp, tmp == 0,
					 MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT);
	if (ret < 0)
		goto out;

	scpsys_clk_disable(scpd->clk, MAX_CLKS);

	ret = scpsys_regulator_disable(scpd);
	if (ret < 0)
		goto out;

	return 0;

out:
	dev_err(scp->dev, "Failed to power off domain %s\n", genpd->name);

	return ret;
}

static int mtk_vote_is_done(struct scp_domain *scpd)
{
	u32 val = 0, mask = 0;

	regmap_read(scpd->vote_regmap, scpd->data->vote_done_ofs, &val);
	mask = BIT(scpd->data->vote_shift);
	if ((val & mask) == mask)
		return 1;

	return 0;
}

static int mtk_vote_is_enable_done(struct scp_domain *scpd)
{
	u32 done = 0, en = 0, set_sta = 0, mask = 0, ack = 0;

	regmap_read(scpd->vote_regmap, scpd->data->vote_done_ofs, &done);
	regmap_read(scpd->vote_regmap, scpd->data->vote_en_ofs, &en);
	regmap_read(scpd->vote_regmap, scpd->data->vote_set_sta_ofs, &set_sta);
	mask = BIT(scpd->data->vote_shift);

	if ((done & mask) && (en & mask) && !(set_sta & mask)) {
		if (scpd->data->vote_ack_ofs) {
			regmap_read(scpd->vote_regmap, scpd->data->vote_ack_ofs, &ack);
			if (!(ack & mask))
				return 0;
		}

		return 1;
	}

	return 0;
}

static int mtk_vote_is_disable_done(struct scp_domain *scpd)
{
	u32 val = 0, val2 = 0;

	regmap_read(scpd->vote_regmap, scpd->data->vote_done_ofs, &val);
	regmap_read(scpd->vote_regmap, scpd->data->vote_clr_sta_ofs, &val2);

	if ((val & BIT(scpd->data->vote_shift)) &&
	    ((val2 & BIT(scpd->data->vote_shift)) == 0x0))
		return 1;

	return 0;
}

static int scpsys_vote_power_on(struct generic_pm_domain *genpd)
{
	struct scp_domain *scpd = container_of(genpd, struct scp_domain, genpd);
	struct scp *scp = scpd->scp;
	u32 val = 0;
	int ret = 0;
	int tmp;
	int i = 0;

	ret = scpsys_regulator_enable(scpd);
	if (ret < 0)
		goto out;

	ret = scpsys_clk_enable(scpd->clk, MAX_CLKS);
	if (ret)
		goto out;

	ret = readx_poll_timeout_atomic(mtk_vote_is_done, scpd, tmp, tmp > 0,
					MTK_POLL_DELAY_US, MTK_POLL_IRQ_TIMEOUT);
	if (ret < 0)
		goto out;

	val = BIT(scpd->data->vote_shift);
	regmap_write(scpd->vote_regmap, scpd->data->vote_set_ofs, val);
	do {
		regmap_read(scpd->vote_regmap, scpd->data->vote_set_ofs, &val);
		if ((val & BIT(scpd->data->vote_shift)) != 0)
			break;

		if (i > MTK_POLL_VOTE_PREPARE_CNT)
			goto out;

		udelay(MTK_POLL_VOTE_PREPARE_US);
		i++;
	} while (1);

	/* add debounce time */
	udelay(1);

	/* wait until VOTER_ACK = 1 */
	ret = readx_poll_timeout_atomic(mtk_vote_is_enable_done, scpd, tmp, tmp > 0,
					MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT_300MS);
	if (ret < 0)
		goto out;

	return 0;
out:
	dev_err(scp->dev, "Failed to power on domain %s(%d)\n", genpd->name, ret);
	return ret;
}

static int scpsys_vote_power_off(struct generic_pm_domain *genpd)
{
	struct scp_domain *scpd = container_of(genpd, struct scp_domain, genpd);
	struct scp *scp = scpd->scp;
	u32 val = 0;
	int ret = 0;
	int tmp;
	int i = 0;

	ret = readx_poll_timeout_atomic(mtk_vote_is_done, scpd, tmp, tmp > 0,
					MTK_POLL_DELAY_US, MTK_POLL_IRQ_TIMEOUT);
	if (ret < 0)
		goto out;

	val = BIT(scpd->data->vote_shift);
	regmap_write(scpd->vote_regmap, scpd->data->vote_clr_ofs, val);
	do {
		regmap_read(scpd->vote_regmap, scpd->data->vote_clr_ofs, &val);
		if ((val & BIT(scpd->data->vote_shift)) == 0)
			break;

		if (i > MTK_POLL_VOTE_PREPARE_CNT)
			goto out;

		i++;
		udelay(MTK_POLL_VOTE_PREPARE_US);
	} while (1);

	/* delay 100us for stable status */
	udelay(MTK_STABLE_DELAY_US);

	/* wait until VOTER_ACK = 0 */
	ret = readx_poll_timeout_atomic(mtk_vote_is_disable_done, scpd, tmp, tmp > 0,
					MTK_POLL_DELAY_US, MTK_POLL_TIMEOUT_300MS);
	if (ret < 0)
		goto out;

	scpsys_clk_disable(scpd->clk, MAX_CLKS);

	ret = scpsys_regulator_disable(scpd);
	if (ret < 0)
		goto out;

	return 0;
out:
	dev_err(scp->dev, "Failed to power off domain %s(%d)\n", genpd->name, ret);
	return ret;
}

static void init_clks(struct platform_device *pdev, struct clk **clk)
{
	int i;

	for (i = CLK_NONE + 1; i < CLK_MAX; i++)
		clk[i] = devm_clk_get(&pdev->dev, clk_names[i]);
}

static int mtk_pd_get_regmap(struct platform_device *pdev, struct regmap **regmap,
			     const char *name)
{
	*regmap = syscon_regmap_lookup_by_phandle(pdev->dev.of_node, name);
	if (PTR_ERR(*regmap) == -ENODEV) {
		dev_notice(&pdev->dev, "%s regmap is null(%ld)\n", name, PTR_ERR(*regmap));
		*regmap = NULL;
	} else if (IS_ERR(*regmap)) {
		dev_notice(&pdev->dev, "Cannot find %s controller: %ld\n", name, PTR_ERR(*regmap));
		return PTR_ERR(*regmap);
	}

	return 0;
}

static struct scp *init_scp(struct platform_device *pdev, const struct scp_soc_data *soc)
{
	struct genpd_onecell_data *pd_data;
	int i, j;
	struct scp *scp;
	struct clk *clk[CLK_MAX];
	int ret;

	scp = devm_kzalloc(&pdev->dev, sizeof(*scp), GFP_KERNEL);
	if (!scp)
		return ERR_PTR(-ENOMEM);

	scp->ctrl_reg.pwr_sta_offs = soc->regs.pwr_sta_offs;
	scp->ctrl_reg.pwr_sta2nd_offs = soc->regs.pwr_sta2nd_offs;

	scp->bus_prot_reg_update = soc->bus_prot_reg_update;

	scp->dev = &pdev->dev;

	scp->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(scp->base))
		return ERR_CAST(scp->base);

	scp->domains = devm_kcalloc(&pdev->dev,
				soc->num_domains, sizeof(*scp->domains), GFP_KERNEL);
	if (!scp->domains)
		return ERR_PTR(-ENOMEM);

	pd_data = &scp->pd_data;

	pd_data->domains = devm_kcalloc(&pdev->dev,
			soc->num_domains, sizeof(*pd_data->domains), GFP_KERNEL);
	if (!pd_data->domains)
		return ERR_PTR(-ENOMEM);

	if (soc->bp_list && soc->num_bp > 0) {
		scp->num_bp = soc->num_bp;
		scp->bp_regmap = devm_kcalloc(&pdev->dev, scp->num_bp,
					      sizeof(*scp->bp_regmap), GFP_KERNEL);
		if (!scp->bp_regmap)
			return ERR_PTR(-ENOMEM);

		/* get bus prot regmap from dts node, 0 means invalid bus type */
		for (i = 1; i < scp->num_bp; i++) {
			ret = mtk_pd_get_regmap(pdev, &scp->bp_regmap[i], soc->bp_list[i]);
			if (ret)
				return ERR_PTR(ret);
		}
	} else {
		scp->infracfg = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
				"infracfg");
		if (IS_ERR(scp->infracfg)) {
			dev_err(&pdev->dev, "Cannot find infracfg controller: %ld\n",
					PTR_ERR(scp->infracfg));
			return ERR_CAST(scp->infracfg);
		}
	}

	for (i = 0; i < soc->num_domains; i++) {
		struct scp_domain *scpd = &scp->domains[i];
		const struct scp_domain_data *data = &soc->domains[i];

		scpd->supply = devm_regulator_get_optional(&pdev->dev, data->name);
		if (IS_ERR(scpd->supply)) {
			if (PTR_ERR(scpd->supply) == -ENODEV)
				scpd->supply = NULL;
			else
				return ERR_CAST(scpd->supply);
		}
	}

	pd_data->num_domains = soc->num_domains;

	init_clks(pdev, clk);

	for (i = 0; i < soc->num_domains; i++) {
		struct scp_domain *scpd = &scp->domains[i];
		struct generic_pm_domain *genpd = &scpd->genpd;
		const struct scp_domain_data *data = &soc->domains[i];

		pd_data->domains[i] = genpd;
		scpd->scp = scp;

		scpd->data = data;

		for (j = 0; j < MAX_CLKS && data->clk_id[j]; j++) {
			struct clk *c = clk[data->clk_id[j]];

			if (IS_ERR(c)) {
				dev_err(&pdev->dev, "%s: clk unavailable\n",
					data->name);
				return ERR_CAST(c);
			}

			scpd->clk[j] = c;
		}

		if (data->vote_comp) {
			ret = mtk_pd_get_regmap(pdev, &scpd->vote_regmap, data->vote_comp);
			if (ret)
				return ERR_PTR(ret);
		}

		genpd->name = data->name;
		if (MTK_SCPD_CAPS(scpd, MTK_SCPD_VOTE_OPS)) {
			genpd->power_on = scpsys_vote_power_on;
			genpd->power_off = scpsys_vote_power_off;
		} else {
			genpd->power_off = scpsys_power_off;
			genpd->power_on = scpsys_power_on;
		}
		if (MTK_SCPD_CAPS(scpd, MTK_SCPD_ACTIVE_WAKEUP))
			genpd->flags |= GENPD_FLAG_ACTIVE_WAKEUP;
		if (MTK_SCPD_CAPS(scpd, MTK_SCPD_IRQ_SAFE))
			genpd->flags |= GENPD_FLAG_IRQ_SAFE;
		if (MTK_SCPD_CAPS(scpd, MTK_SCPD_ALWAYS_ON))
			genpd->flags |= GENPD_FLAG_ALWAYS_ON;
	}

	return scp;
}

static void mtk_register_power_domains(struct platform_device *pdev,
				struct scp *scp, int num)
{
	struct genpd_onecell_data *pd_data;
	int i, ret;

	for (i = 0; i < num; i++) {
		struct scp_domain *scpd = &scp->domains[i];
		struct generic_pm_domain *genpd = &scpd->genpd;
		bool on;

		/*
		 * Initially turn on all domains to make the domains usable
		 * with !CONFIG_PM and to get the hardware in sync with the
		 * software.  The unused domains will be switched off during
		 * late_init time.
		 */
		if (MTK_SCPD_CAPS(scpd, MTK_SCPD_BYPASS_INIT_ON))
			on = false;
		else
			on = !WARN_ON(genpd->power_on(genpd) < 0);

		pm_genpd_init(genpd, NULL, !on);
	}

	/*
	 * We are not allowed to fail here since there is no way to unregister
	 * a power domain. Once registered above we have to keep the domains
	 * valid.
	 */

	pd_data = &scp->pd_data;

	ret = of_genpd_add_provider_onecell(pdev->dev.of_node, pd_data);
	if (ret)
		dev_err(&pdev->dev, "Failed to add OF provider: %d\n", ret);
}

/*
 * MT2701 power domain support
 */

static const struct scp_domain_data scp_domain_data_mt2701[] = {
	[MT2701_POWER_DOMAIN_CONN] = {
		.name = "conn",
		.sta_mask = PWR_STATUS_CONN,
		.ctl_offs = SPM_CONN_PWR_CON,
		.bus_prot_mask = MT2701_TOP_AXI_PROT_EN_CONN_M |
				 MT2701_TOP_AXI_PROT_EN_CONN_S,
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2701_POWER_DOMAIN_DISP] = {
		.name = "disp",
		.sta_mask = PWR_STATUS_DISP,
		.ctl_offs = SPM_DIS_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.clk_id = {CLK_MM},
		.bus_prot_mask = MT2701_TOP_AXI_PROT_EN_MM_M0,
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2701_POWER_DOMAIN_MFG] = {
		.name = "mfg",
		.sta_mask = PWR_STATUS_MFG,
		.ctl_offs = SPM_MFG_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_MFG},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2701_POWER_DOMAIN_VDEC] = {
		.name = "vdec",
		.sta_mask = PWR_STATUS_VDEC,
		.ctl_offs = SPM_VDE_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_MM},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2701_POWER_DOMAIN_ISP] = {
		.name = "isp",
		.sta_mask = PWR_STATUS_ISP,
		.ctl_offs = SPM_ISP_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(13, 12),
		.clk_id = {CLK_MM},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2701_POWER_DOMAIN_BDP] = {
		.name = "bdp",
		.sta_mask = PWR_STATUS_BDP,
		.ctl_offs = SPM_BDP_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2701_POWER_DOMAIN_ETH] = {
		.name = "eth",
		.sta_mask = PWR_STATUS_ETH,
		.ctl_offs = SPM_ETH_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_ETHIF},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2701_POWER_DOMAIN_HIF] = {
		.name = "hif",
		.sta_mask = PWR_STATUS_HIF,
		.ctl_offs = SPM_HIF_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_ETHIF},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2701_POWER_DOMAIN_IFR_MSC] = {
		.name = "ifr_msc",
		.sta_mask = PWR_STATUS_IFR_MSC,
		.ctl_offs = SPM_IFR_MSC_PWR_CON,
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
};

/*
 * MT2712 power domain support
 */
static const struct scp_domain_data scp_domain_data_mt2712[] = {
	[MT2712_POWER_DOMAIN_MM] = {
		.name = "mm",
		.sta_mask = PWR_STATUS_DISP,
		.ctl_offs = SPM_DIS_PWR_CON,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_MM},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_VDEC] = {
		.name = "vdec",
		.sta_mask = PWR_STATUS_VDEC,
		.ctl_offs = SPM_VDE_PWR_CON,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_MM, CLK_VDEC},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_VENC] = {
		.name = "venc",
		.sta_mask = PWR_STATUS_VENC,
		.ctl_offs = SPM_VEN_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_MM, CLK_VENC, CLK_JPGDEC},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_ISP] = {
		.name = "isp",
		.sta_mask = PWR_STATUS_ISP,
		.ctl_offs = SPM_ISP_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(13, 12),
		.clk_id = {CLK_MM},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_AUDIO] = {
		.name = "audio",
		.sta_mask = PWR_STATUS_AUDIO,
		.ctl_offs = SPM_AUDIO_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_AUDIO},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_USB] = {
		.name = "usb",
		.sta_mask = PWR_STATUS_USB,
		.ctl_offs = SPM_USB_PWR_CON,
		.sram_pdn_bits = GENMASK(10, 8),
		.sram_pdn_ack_bits = GENMASK(14, 12),
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_USB2] = {
		.name = "usb2",
		.sta_mask = PWR_STATUS_USB2,
		.ctl_offs = SPM_USB2_PWR_CON,
		.sram_pdn_bits = GENMASK(10, 8),
		.sram_pdn_ack_bits = GENMASK(14, 12),
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_MFG] = {
		.name = "mfg",
		.sta_mask = PWR_STATUS_MFG,
		.ctl_offs = SPM_MFG_PWR_CON,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(16, 16),
		.clk_id = {CLK_MFG},
		.bus_prot_mask = BIT(14) | BIT(21) | BIT(23),
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_MFG_SC1] = {
		.name = "mfg_sc1",
		.sta_mask = BIT(22),
		.ctl_offs = 0x02c0,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(16, 16),
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_MFG_SC2] = {
		.name = "mfg_sc2",
		.sta_mask = BIT(23),
		.ctl_offs = 0x02c4,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(16, 16),
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT2712_POWER_DOMAIN_MFG_SC3] = {
		.name = "mfg_sc3",
		.sta_mask = BIT(30),
		.ctl_offs = 0x01f8,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(16, 16),
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
};

static const struct scp_subdomain scp_subdomain_mt2712[] = {
	{MT2712_POWER_DOMAIN_MM, MT2712_POWER_DOMAIN_VDEC},
	{MT2712_POWER_DOMAIN_MM, MT2712_POWER_DOMAIN_VENC},
	{MT2712_POWER_DOMAIN_MM, MT2712_POWER_DOMAIN_ISP},
	{MT2712_POWER_DOMAIN_MFG, MT2712_POWER_DOMAIN_MFG_SC1},
	{MT2712_POWER_DOMAIN_MFG_SC1, MT2712_POWER_DOMAIN_MFG_SC2},
	{MT2712_POWER_DOMAIN_MFG_SC2, MT2712_POWER_DOMAIN_MFG_SC3},
};

/*
 * MT6797 power domain support
 */

static const struct scp_domain_data scp_domain_data_mt6797[] = {
	[MT6797_POWER_DOMAIN_VDEC] = {
		.name = "vdec",
		.sta_mask = BIT(7),
		.ctl_offs = 0x300,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_VDEC},
	},
	[MT6797_POWER_DOMAIN_VENC] = {
		.name = "venc",
		.sta_mask = BIT(21),
		.ctl_offs = 0x304,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_NONE},
	},
	[MT6797_POWER_DOMAIN_ISP] = {
		.name = "isp",
		.sta_mask = BIT(5),
		.ctl_offs = 0x308,
		.sram_pdn_bits = GENMASK(9, 8),
		.sram_pdn_ack_bits = GENMASK(13, 12),
		.clk_id = {CLK_NONE},
	},
	[MT6797_POWER_DOMAIN_MM] = {
		.name = "mm",
		.sta_mask = BIT(3),
		.ctl_offs = 0x30C,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_MM},
		.bus_prot_mask = (BIT(1) | BIT(2)),
	},
	[MT6797_POWER_DOMAIN_AUDIO] = {
		.name = "audio",
		.sta_mask = BIT(24),
		.ctl_offs = 0x314,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_NONE},
	},
	[MT6797_POWER_DOMAIN_MFG_ASYNC] = {
		.name = "mfg_async",
		.sta_mask = BIT(13),
		.ctl_offs = 0x334,
		.sram_pdn_bits = 0,
		.sram_pdn_ack_bits = 0,
		.clk_id = {CLK_MFG},
	},
	[MT6797_POWER_DOMAIN_MJC] = {
		.name = "mjc",
		.sta_mask = BIT(20),
		.ctl_offs = 0x310,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_NONE},
	},
};

#define SPM_PWR_STATUS_MT6797		0x0180
#define SPM_PWR_STATUS_2ND_MT6797	0x0184

static const struct scp_subdomain scp_subdomain_mt6797[] = {
	{MT6797_POWER_DOMAIN_MM, MT6797_POWER_DOMAIN_VDEC},
	{MT6797_POWER_DOMAIN_MM, MT6797_POWER_DOMAIN_ISP},
	{MT6797_POWER_DOMAIN_MM, MT6797_POWER_DOMAIN_VENC},
	{MT6797_POWER_DOMAIN_MM, MT6797_POWER_DOMAIN_MJC},
};

/*
 * MT7622 power domain support
 */

static const struct scp_domain_data scp_domain_data_mt7622[] = {
	[MT7622_POWER_DOMAIN_ETHSYS] = {
		.name = "ethsys",
		.sta_mask = PWR_STATUS_ETHSYS,
		.ctl_offs = SPM_ETHSYS_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_NONE},
		.bus_prot_mask = MT7622_TOP_AXI_PROT_EN_ETHSYS,
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT7622_POWER_DOMAIN_HIF0] = {
		.name = "hif0",
		.sta_mask = PWR_STATUS_HIF0,
		.ctl_offs = SPM_HIF0_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_HIFSEL},
		.bus_prot_mask = MT7622_TOP_AXI_PROT_EN_HIF0,
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT7622_POWER_DOMAIN_HIF1] = {
		.name = "hif1",
		.sta_mask = PWR_STATUS_HIF1,
		.ctl_offs = SPM_HIF1_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_HIFSEL},
		.bus_prot_mask = MT7622_TOP_AXI_PROT_EN_HIF1,
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT7622_POWER_DOMAIN_WB] = {
		.name = "wb",
		.sta_mask = PWR_STATUS_WB,
		.ctl_offs = SPM_WB_PWR_CON,
		.sram_pdn_bits = 0,
		.sram_pdn_ack_bits = 0,
		.clk_id = {CLK_NONE},
		.bus_prot_mask = MT7622_TOP_AXI_PROT_EN_WB,
		.caps = MTK_SCPD_ACTIVE_WAKEUP | MTK_SCPD_FWAIT_SRAM,
	},
};

/*
 * MT7623A power domain support
 */

static const struct scp_domain_data scp_domain_data_mt7623a[] = {
	[MT7623A_POWER_DOMAIN_CONN] = {
		.name = "conn",
		.sta_mask = PWR_STATUS_CONN,
		.ctl_offs = SPM_CONN_PWR_CON,
		.bus_prot_mask = MT2701_TOP_AXI_PROT_EN_CONN_M |
				 MT2701_TOP_AXI_PROT_EN_CONN_S,
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT7623A_POWER_DOMAIN_ETH] = {
		.name = "eth",
		.sta_mask = PWR_STATUS_ETH,
		.ctl_offs = SPM_ETH_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_ETHIF},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT7623A_POWER_DOMAIN_HIF] = {
		.name = "hif",
		.sta_mask = PWR_STATUS_HIF,
		.ctl_offs = SPM_HIF_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_ETHIF},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT7623A_POWER_DOMAIN_IFR_MSC] = {
		.name = "ifr_msc",
		.sta_mask = PWR_STATUS_IFR_MSC,
		.ctl_offs = SPM_IFR_MSC_PWR_CON,
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
};

/*
 * MT8173 power domain support
 */

static const struct scp_domain_data scp_domain_data_mt8173[] = {
	[MT8173_POWER_DOMAIN_VDEC] = {
		.name = "vdec",
		.sta_mask = PWR_STATUS_VDEC,
		.ctl_offs = SPM_VDE_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_MM},
	},
	[MT8173_POWER_DOMAIN_VENC] = {
		.name = "venc",
		.sta_mask = PWR_STATUS_VENC,
		.ctl_offs = SPM_VEN_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_MM, CLK_VENC},
	},
	[MT8173_POWER_DOMAIN_ISP] = {
		.name = "isp",
		.sta_mask = PWR_STATUS_ISP,
		.ctl_offs = SPM_ISP_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(13, 12),
		.clk_id = {CLK_MM},
	},
	[MT8173_POWER_DOMAIN_MM] = {
		.name = "mm",
		.sta_mask = PWR_STATUS_DISP,
		.ctl_offs = SPM_DIS_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_MM},
		.bus_prot_mask = MT8173_TOP_AXI_PROT_EN_MM_M0 |
			MT8173_TOP_AXI_PROT_EN_MM_M1,
	},
	[MT8173_POWER_DOMAIN_VENC_LT] = {
		.name = "venc_lt",
		.sta_mask = PWR_STATUS_VENC_LT,
		.ctl_offs = SPM_VEN2_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_MM, CLK_VENC_LT},
	},
	[MT8173_POWER_DOMAIN_AUDIO] = {
		.name = "audio",
		.sta_mask = PWR_STATUS_AUDIO,
		.ctl_offs = SPM_AUDIO_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_NONE},
	},
	[MT8173_POWER_DOMAIN_USB] = {
		.name = "usb",
		.sta_mask = PWR_STATUS_USB,
		.ctl_offs = SPM_USB_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_NONE},
		.caps = MTK_SCPD_ACTIVE_WAKEUP,
	},
	[MT8173_POWER_DOMAIN_MFG_ASYNC] = {
		.name = "mfg_async",
		.sta_mask = PWR_STATUS_MFG_ASYNC,
		.ctl_offs = SPM_MFG_ASYNC_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = 0,
		.clk_id = {CLK_MFG},
	},
	[MT8173_POWER_DOMAIN_MFG_2D] = {
		.name = "mfg_2d",
		.sta_mask = PWR_STATUS_MFG_2D,
		.ctl_offs = SPM_MFG_2D_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(13, 12),
		.clk_id = {CLK_NONE},
	},
	[MT8173_POWER_DOMAIN_MFG] = {
		.name = "mfg",
		.sta_mask = PWR_STATUS_MFG,
		.ctl_offs = SPM_MFG_PWR_CON,
		.sram_pdn_bits = GENMASK(13, 8),
		.sram_pdn_ack_bits = GENMASK(21, 16),
		.clk_id = {CLK_NONE},
		.bus_prot_mask = MT8173_TOP_AXI_PROT_EN_MFG_S |
			MT8173_TOP_AXI_PROT_EN_MFG_M0 |
			MT8173_TOP_AXI_PROT_EN_MFG_M1 |
			MT8173_TOP_AXI_PROT_EN_MFG_SNOOP_OUT,
	},
};

static const struct scp_subdomain scp_subdomain_mt8173[] = {
	{MT8173_POWER_DOMAIN_MFG_ASYNC, MT8173_POWER_DOMAIN_MFG_2D},
	{MT8173_POWER_DOMAIN_MFG_2D, MT8173_POWER_DOMAIN_MFG},
};

static const struct scp_soc_data mt2701_data = {
	.domains = scp_domain_data_mt2701,
	.num_domains = ARRAY_SIZE(scp_domain_data_mt2701),
	.regs = {
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND
	},
	.bus_prot_reg_update = true,
};

static const struct scp_soc_data mt2712_data = {
	.domains = scp_domain_data_mt2712,
	.num_domains = ARRAY_SIZE(scp_domain_data_mt2712),
	.subdomains = scp_subdomain_mt2712,
	.num_subdomains = ARRAY_SIZE(scp_subdomain_mt2712),
	.regs = {
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND
	},
	.bus_prot_reg_update = false,
};

static const struct scp_soc_data mt6797_data = {
	.domains = scp_domain_data_mt6797,
	.num_domains = ARRAY_SIZE(scp_domain_data_mt6797),
	.subdomains = scp_subdomain_mt6797,
	.num_subdomains = ARRAY_SIZE(scp_subdomain_mt6797),
	.regs = {
		.pwr_sta_offs = SPM_PWR_STATUS_MT6797,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND_MT6797
	},
	.bus_prot_reg_update = true,
};

static const struct scp_soc_data mt7622_data = {
	.domains = scp_domain_data_mt7622,
	.num_domains = ARRAY_SIZE(scp_domain_data_mt7622),
	.regs = {
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND
	},
	.bus_prot_reg_update = true,
};

static const struct scp_soc_data mt7623a_data = {
	.domains = scp_domain_data_mt7623a,
	.num_domains = ARRAY_SIZE(scp_domain_data_mt7623a),
	.regs = {
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND
	},
	.bus_prot_reg_update = true,
};

static const struct scp_soc_data mt8173_data = {
	.domains = scp_domain_data_mt8173,
	.num_domains = ARRAY_SIZE(scp_domain_data_mt8173),
	.subdomains = scp_subdomain_mt8173,
	.num_subdomains = ARRAY_SIZE(scp_subdomain_mt8173),
	.regs = {
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND
	},
	.bus_prot_reg_update = true,
};

/*
 * scpsys driver init
 */

static const struct of_device_id of_scpsys_match_tbl[] = {
	{
		.compatible = "mediatek,mt2701-scpsys",
		.data = &mt2701_data,
	}, {
		.compatible = "mediatek,mt2712-scpsys",
		.data = &mt2712_data,
	}, {
		.compatible = "mediatek,mt6797-scpsys",
		.data = &mt6797_data,
	}, {
		.compatible = "mediatek,mt7622-scpsys",
		.data = &mt7622_data,
	}, {
		.compatible = "mediatek,mt7623a-scpsys",
		.data = &mt7623a_data,
	}, {
		.compatible = "mediatek,mt8173-scpsys",
		.data = &mt8173_data,
	}, {
		/* sentinel */
	}
};

static int scpsys_probe(struct platform_device *pdev)
{
	const struct scp_subdomain *sd;
	const struct scp_soc_data *soc;
	struct scp *scp;
	struct genpd_onecell_data *pd_data;
	int i, ret;

	soc = of_device_get_match_data(&pdev->dev);

	scp = init_scp(pdev, soc);
	if (IS_ERR(scp))
		return PTR_ERR(scp);

	mtk_register_power_domains(pdev, scp, soc->num_domains);

	pd_data = &scp->pd_data;

	for (i = 0, sd = soc->subdomains; i < soc->num_subdomains; i++, sd++) {
		ret = pm_genpd_add_subdomain(pd_data->domains[sd->origin],
					     pd_data->domains[sd->subdomain]);
		if (ret && IS_ENABLED(CONFIG_PM))
			dev_err(&pdev->dev, "Failed to add subdomain: %d\n",
				ret);
	}

	if (soc->post_probe) {
		ret = soc->post_probe(pdev, scp);
		if (ret)
			return ret;
	}

	return 0;
}

static struct platform_driver scpsys_drv = {
	.probe = scpsys_probe,
	.driver = {
		.name = "mtk-scpsys",
		.suppress_bind_attrs = true,
		.of_match_table = of_scpsys_match_tbl,
	},
};
builtin_platform_driver(scpsys_drv);
