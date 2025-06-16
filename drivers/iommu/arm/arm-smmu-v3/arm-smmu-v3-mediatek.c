// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 MediaTek Inc.
 * Author: Ning li <ning.li@mediatek.com>
 * Author: Xueqi Zhang <xueqi.zhang@mediatek.com>
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>

#include "arm-smmu-v3.h"

#include <linux/soc/mediatek/mtk_sip_svc.h>
#include <linux/arm-smccc.h>

#define SMMUWP_GLB_CTL0			(0x0)
#define CTL0_STD_AXI_MODE_DIS		BIT(0)
#define CTL0_MON_DIS			BIT(1)
#define CTL0_DCM_EN			BIT(2)
#define CTL0_WRAPPER_CK_AOEN		BIT(3)
#define CTL0_AUTO_AXDOMAIN_EN		BIT(4)
#define CTL0_IRQ_BUSY_EN		BIT(5)
#define CTL0_ABT_CNT_CLR		BIT(6)
#define CTL0_LEGACY_AXCACHE		BIT(7)
#define CTL0_COMMIT_DIS			BIT(8)
#define CTL0_AUTO_SLP_DIS		BIT(9)
#define CTL0_STTSL_DIS			BIT(10)
#define CTL0_CFG_TAB_DCM_EN		BIT(11)
#define CTL0_CPU_PARTID_DIS		BIT(14)
/* New bits of SMMU wrapper extension */
#define CTL0_TCU2SLC_DCM_EN		BIT(18)
#define CTL0_APB_DCM_EN			BIT(19)
#define CTL0_DVM_DCM_EN			BIT(20)
#define CTL0_CPU_TBU_PARTID_DIS		BIT(21)

#define SMMUWP_IRQ_STA			(0x80)
#define STA_TCU_GLB_INTR		BIT(0)
#define STA_TCU_CMD_SYNC_INTR		BIT(1)
#define STA_TCU_EVTQ_INTR		BIT(2)
#define STA_TCU_PRI_INTR		BIT(3)
#define STA_TCU_PMU_INTR		BIT(4)
#define STA_TCU_RAS_CRI			BIT(5)
#define STA_TCU_RAS_ERI			BIT(6)
#define STA_TCU_RAS_FHI			BIT(7)

#define SMMUWP_IRQ_ACK			(0x84)

#define SMMUWP_IRQ_ACK_CNT		(0x88)
#define IRQ_ACK_CNT_MSK			GENMASK(7, 0)

/* SMMU non-secure interrupt pending count register, count 20 */
#define SMMUWP_IRQ_CNTx(cnt)		(0x100 + 0x4 * (cnt))

#define SMMU_TCU_CTL1_AXSLC		(0x204)
#define AXSLC_BIT_FIELD			GENMASK(8, 4)
#define AXSLC_CACHE			BIT(5)
#define AXSLC_ALLOCATE			BIT(6)
#define AXSLC_SPECULATIVE		BIT(7)
#define AXSLC_SET			(AXSLC_CACHE | AXSLC_ALLOCATE | AXSLC_SPECULATIVE)
#define SLC_SB_ONLY_EN			BIT(1)

/* SMMU TBUx read translation fault monitor0 */
#define SMMUWP_TBUx_RTFM0(tbu)		(0x380 + 0x100 * (tbu))
#define RTFM0_FAULT_AXI_ID		GENMASK_ULL(19, 0)
#define RTFM0_FAULT_DET			BIT(31)

/* SMMU TBUx read translation fault monitor1 */
#define SMMUWP_TBUx_RTFM1(tbu)		(0x384 + 0x100 * (tbu))
#define RTFM1_FAULT_VA_35_32		GENMASK_ULL(3, 0)
#define RTFM1_FAULT_VA_31_12		GENMASK_ULL(31, 12)

/* SMMU TBUx read translation fault monitor2 */
#define SMMUWP_TBUx_RTFM2(tbu)		(0x388 + 0x100 * (tbu))
#define RTFM2_FAULT_SID			GENMASK_ULL(7, 0)
#define RTFM2_FAULT_SSID		GENMASK_ULL(15, 8)
#define RTFM2_FAULT_SSIDV		BIT(16)
#define RTFM2_FAULT_SECSID		BIT(17)

/* SMMU TBUx write translation fault monitor0 */
#define SMMUWP_TBUx_WTFM0(tbu)		(0x390 + 0x100 * (tbu))
#define WTFM0_FAULT_AXI_ID		GENMASK_ULL(19, 0)
#define WTFM0_FAULT_DET			BIT(31)

/* SMMU TBUx write translation fault monitor1 */
#define SMMUWP_TBUx_WTFM1(tbu)		(0x394 + 0x100 * (tbu))
#define WTFM1_FAULT_VA_35_32		GENMASK_ULL(3, 0)
#define WTFM1_FAULT_VA_31_12		GENMASK_ULL(31, 12)

/* SMMU TBUx write translation fault monitor2 */
#define SMMUWP_TBUx_WTFM2(tbu)		(0x398 + 0x100 * (tbu))
#define WTFM2_FAULT_SID			GENMASK_ULL(7, 0)
#define WTFM2_FAULT_SSID		GENMASK_ULL(15, 8)
#define WTFM2_FAULT_SSIDV		BIT(16)
#define WTFM2_FAULT_SECSID		BIT(17)

/* SMMU TBU Manual OG Control High Register0 */
#define SMMUWP_TBU0_MOGH0		(0x3b4)
#define MOGH_EN				BIT(29)
#define MOGH_RW				BIT(28)

/* SMMU translation fault TBUx */
#define SMMUWP_TF_TBU_MSK		GENMASK(26, 24)
#define SMMUWP_TF_TBU(tbu)		FIELD_PREP(SMMUWP_TF_TBU_MSK, tbu)

#define SMMU_FAULT_RS_INTERVAL		DEFAULT_RATELIMIT_INTERVAL
#define SMMU_FAULT_RS_BURST		(1)

#define STRSEC(sec)			((sec) ? "SECURE" : "NORMAL")

#define WP_OFFSET_MT8196		0x1e0000

#define MTK_SMMU_COMP_STR_LEN		64

#define MTK_SMMU_FAULT_IOVA(low, high) ((low) | (((u64)(high) & 0xf) << 32))

#define SMMU_SUCCESS			(0)
#define SMMU_ID_ERR			(1)
#define SMMU_CMD_ERR			(2)
#define SMMU_PARA_INVALID		(3)
#define SMMU_NEED			(4)
#define SMMU_NONEED			(5)

/* plat flags: */
#define SMMU_SKIP_PM_CLK		BIT(0)
#define SMMU_CLK_AO_EN			BIT(1)
#define SMMU_AXSLC_EN			BIT(2)
#define SMMU_DIS_CPU_PARTID		BIT(3)
#define SMMU_DIS_CPU_TBU_PARTID		BIT(4)
#define SMMU_REQUIRE_PARENT		BIT(5)
#define MTK_SMMU_HAS_FLAG(pdata, _x)    (!!(((pdata)->flags) & (_x)))

enum mtk_smmu_type {
	MTK_SMMU_MM,
	MTK_SMMU_APU,
	MTK_SMMU_TYPE_NUM,
};

struct mtk_smmu_v3_plat {
	u32			wp_offset;
	unsigned int		tbu_cnt;
	enum mtk_smmu_type	smmu_type;
	u32			flags;
};

struct mtk_smmu_v3 {
	struct arm_smmu_device	smmu;
	void __iomem			*wp_base;
	const struct mtk_smmu_v3_plat *plat_data;
};

static const struct mtk_smmu_v3_plat mt8196_data_mm = {
	.wp_offset		= WP_OFFSET_MT8196,
	.tbu_cnt		= 3,
	.smmu_type		= MTK_SMMU_MM,
	.flags			= SMMU_AXSLC_EN,
};

static const struct mtk_smmu_v3_plat mt8196_data_apu = {
	.wp_offset		= WP_OFFSET_MT8196,
	.tbu_cnt		= 3,
	.smmu_type		= MTK_SMMU_APU,
	.flags			= SMMU_AXSLC_EN | SMMU_REQUIRE_PARENT,
};

struct mtk_smmu_v3_of_device_data {
	char			compatible[MTK_SMMU_COMP_STR_LEN];
	const void		*data;
};

static const struct mtk_smmu_v3_of_device_data mtk_smmu_v3_of_ids[] = {
	{ .compatible = "mediatek,mt8196-apu-smmu", .data = &mt8196_data_apu},
	{ .compatible = "mediatek,mt8196-mm-smmu", .data = &mt8196_data_mm},
};

static inline struct mtk_smmu_v3 *to_mtk_smmu_v3(struct arm_smmu_device *smmu)
{
	return container_of(smmu, struct mtk_smmu_v3, smmu);
}

static const struct mtk_smmu_v3_plat *mtk_smmu_v3_get_plat_data(const struct device_node *np)
{
	const struct mtk_smmu_v3_of_device_data *of_device = mtk_smmu_v3_of_ids;
	int i;

	for (i = 0; i < ARRAY_SIZE(mtk_smmu_v3_of_ids); i++, of_device++) {
		if (of_device_is_compatible(np, of_device->compatible))
			return of_device->data;
	}
	return NULL;
}

static inline void smmu_write_field(void __iomem *base,
				    unsigned int reg,
				    unsigned int mask,
				    unsigned int val)
{
	unsigned int regval;

	regval = readl_relaxed(base + reg);
	regval = (regval & (~mask)) | val;
	writel_relaxed(regval, base + reg);
}

static void smmu_init_wpcfg(struct arm_smmu_device *smmu)
{
	struct mtk_smmu_v3 *mtk_smmu_v3 = to_mtk_smmu_v3(smmu);
	void __iomem *wp_base = mtk_smmu_v3->wp_base;

	/* DCM basic setting */
	smmu_write_field(wp_base, SMMUWP_GLB_CTL0, CTL0_DCM_EN, CTL0_DCM_EN);
	smmu_write_field(wp_base, SMMUWP_GLB_CTL0, CTL0_CFG_TAB_DCM_EN,
			 CTL0_CFG_TAB_DCM_EN);

	smmu_write_field(wp_base, SMMUWP_GLB_CTL0,
			 CTL0_TCU2SLC_DCM_EN | CTL0_APB_DCM_EN |
			 CTL0_DVM_DCM_EN,
			 CTL0_TCU2SLC_DCM_EN | CTL0_APB_DCM_EN |
			 CTL0_DVM_DCM_EN);

	if (MTK_SMMU_HAS_FLAG(mtk_smmu_v3->plat_data, SMMU_DIS_CPU_PARTID))
		smmu_write_field(wp_base, SMMUWP_GLB_CTL0, CTL0_CPU_PARTID_DIS,
				 CTL0_CPU_PARTID_DIS);
	if (MTK_SMMU_HAS_FLAG(mtk_smmu_v3->plat_data, SMMU_DIS_CPU_TBU_PARTID))
		smmu_write_field(wp_base, SMMUWP_GLB_CTL0,
				 CTL0_CPU_TBU_PARTID_DIS, CTL0_CPU_TBU_PARTID_DIS);

	/* Used for MM_SMMMU read command overtaking */
	if (mtk_smmu_v3->plat_data->smmu_type == MTK_SMMU_MM)
		smmu_write_field(wp_base, SMMUWP_GLB_CTL0, CTL0_STD_AXI_MODE_DIS,
				 CTL0_STD_AXI_MODE_DIS);

	/* Set AXSLC */
	if (MTK_SMMU_HAS_FLAG(mtk_smmu_v3->plat_data, SMMU_AXSLC_EN)) {
		smmu_write_field(wp_base, SMMUWP_GLB_CTL0,
				 CTL0_STD_AXI_MODE_DIS, CTL0_STD_AXI_MODE_DIS);
		smmu_write_field(wp_base, SMMU_TCU_CTL1_AXSLC, AXSLC_BIT_FIELD,
				 AXSLC_SET);
		smmu_write_field(wp_base, SMMU_TCU_CTL1_AXSLC, SLC_SB_ONLY_EN,
				 SLC_SB_ONLY_EN);
	}
}

/* Consume SMMU wrapper interrupt bit */
static unsigned int
smmuwp_consume_intr(void __iomem *wp_base, unsigned int irq_bit)
{
	unsigned int pend_cnt;

	pend_cnt = readl_relaxed(wp_base + SMMUWP_IRQ_CNTx(__ffs(irq_bit)));
	smmu_write_field(wp_base, SMMUWP_IRQ_ACK_CNT, IRQ_ACK_CNT_MSK, pend_cnt);
	writel_relaxed(irq_bit, wp_base + SMMUWP_IRQ_ACK);

	return pend_cnt;
}

/* clear translation fault mark */
static void smmuwp_clear_tf(void __iomem *wp_base)
{
	smmu_write_field(wp_base, SMMUWP_GLB_CTL0, CTL0_ABT_CNT_CLR, CTL0_ABT_CNT_CLR);
	smmu_write_field(wp_base, SMMUWP_GLB_CTL0, CTL0_ABT_CNT_CLR, 0);
}

static u32 smmuwp_fault_id(u32 axi_id, u32 tbu_id)
{
	u32 fault_id = (axi_id & ~SMMUWP_TF_TBU_MSK) | (SMMUWP_TF_TBU(tbu_id));

	return fault_id;
}

/* Process TBU translation fault Monitor */
static bool smmuwp_process_tf(struct arm_smmu_device *smmu)
{
	struct mtk_smmu_v3 *mtk_smmu_v3 = to_mtk_smmu_v3(smmu);
	void __iomem *wp_base = mtk_smmu_v3->wp_base;
	unsigned int sid, ssid, secsidv, ssidv;
	u32 i, regval, va35_32, axiid, fault_id;
	u64 fault_iova;
	bool tf_det = false;

	for (i = 0; i < mtk_smmu_v3->plat_data->tbu_cnt; i++) {
		regval = readl_relaxed(wp_base + SMMUWP_TBUx_RTFM0(i));
		if (!(regval & RTFM0_FAULT_DET))
			goto write;

		tf_det = true;
		axiid = FIELD_GET(RTFM0_FAULT_AXI_ID, regval);
		fault_id = smmuwp_fault_id(axiid, i);

		regval = readl_relaxed(wp_base + SMMUWP_TBUx_RTFM1(i));
		va35_32 = FIELD_GET(RTFM1_FAULT_VA_35_32, regval);
		fault_iova = MTK_SMMU_FAULT_IOVA(regval & RTFM1_FAULT_VA_31_12, va35_32);

		regval = readl_relaxed(wp_base + SMMUWP_TBUx_RTFM2(i));
		sid = FIELD_GET(RTFM2_FAULT_SID, regval);
		ssid = FIELD_GET(RTFM2_FAULT_SSID, regval);
		ssidv = FIELD_GET(RTFM2_FAULT_SSIDV, regval);
		secsidv = FIELD_GET(RTFM2_FAULT_SECSID, regval);
		dev_err_ratelimited(smmu->dev, "TF read in %s world, TBU_id-%d-fault_id:0x%x(0x%x)\n",
				    STRSEC(secsidv), i, fault_id, axiid);
		dev_err_ratelimited(smmu->dev,
				    "iova:0x%llx, sid:%d, ssid:%d, ssidv:%d, secsidv:%d\n",
				    fault_iova, sid, ssid, ssidv, secsidv);

write:
		regval = readl_relaxed(wp_base + SMMUWP_TBUx_WTFM0(i));
		if (!(regval & WTFM0_FAULT_DET))
			continue;

		tf_det = true;
		axiid = FIELD_GET(WTFM0_FAULT_AXI_ID, regval);
		fault_id = smmuwp_fault_id(axiid, i);

		regval = readl_relaxed(wp_base + SMMUWP_TBUx_WTFM1(i));
		va35_32 = FIELD_GET(WTFM1_FAULT_VA_35_32, regval);
		fault_iova = MTK_SMMU_FAULT_IOVA(regval & RTFM1_FAULT_VA_31_12, va35_32);

		regval = readl_relaxed(wp_base + SMMUWP_TBUx_WTFM2(i));
		sid = FIELD_GET(WTFM2_FAULT_SID, regval);
		ssid = FIELD_GET(WTFM2_FAULT_SSID, regval);
		ssidv = FIELD_GET(WTFM2_FAULT_SSIDV, regval);
		secsidv = FIELD_GET(WTFM2_FAULT_SECSID, regval);
		dev_err_ratelimited(smmu->dev, "TF write in %s world, TBU_id-%d-fault_id:0x%x(0x%x)\n",
				    STRSEC(secsidv), i, fault_id, axiid);
		dev_err_ratelimited(smmu->dev,
				    "iova:0x%llx, sid:%d, ssid:%d, ssidv:%d, secsidv:%d\n",
				    fault_iova, sid, ssid, ssidv, secsidv);
	}

	if (!tf_det)
		dev_info(smmu->dev, "No TF detected or has been cleaned\n");

	return tf_det;
}

static int
mtk_smmu_evt_handler(int irq, struct arm_smmu_device *smmu, u64 *evt, struct ratelimit_state *rs)
{
	if (!__ratelimit(rs)) {
		smmuwp_clear_tf(smmu);
		return 0;
	}

	smmuwp_process_tf(smmu);
	smmuwp_clear_tf(smmu);

	return 0;
}

/* Process SMMU wrapper interrupt */
static int mtk_smmu_v3_smmuwp_irq_handler(int irq, struct arm_smmu_device *smmu)
{
	struct mtk_smmu_v3 *mtk_smmuv3 = to_mtk_smmu_v3(smmu);
	void __iomem *wp_base = mtk_smmuv3->wp_base;
	unsigned int irq_sta, pend_cnt;

	irq_sta = readl_relaxed(wp_base + SMMUWP_IRQ_STA);
	if (irq_sta == 0)
		return 0;

	if (irq_sta & STA_TCU_GLB_INTR) {
		pend_cnt = smmuwp_consume_intr(wp_base, STA_TCU_GLB_INTR);
		dev_dbg(smmu->dev,
			"IRQ_STA:0x%x, Non-secure TCU global interrupt detected pending_cnt: %d\n",
			irq_sta, pend_cnt);
	}

	if (irq_sta & STA_TCU_CMD_SYNC_INTR) {
		pend_cnt = smmuwp_consume_intr(wp_base, STA_TCU_CMD_SYNC_INTR);
		dev_dbg(smmu->dev,
			"IRQ_STA:0x%x, Non-secure TCU CMD_SYNC interrupt detected pending_cnt: %d\n",
			irq_sta, pend_cnt);
	}

	if (irq_sta & STA_TCU_EVTQ_INTR) {
		pend_cnt = smmuwp_consume_intr(wp_base, STA_TCU_EVTQ_INTR);
		dev_dbg(smmu->dev,
			"IRQ_STA:0x%x, Non-secure TCU EVTQ interrupt detected pending_cnt: %d\n",
			irq_sta, pend_cnt);
	}

	if (irq_sta & STA_TCU_PRI_INTR) {
		pend_cnt = smmuwp_consume_intr(wp_base, STA_TCU_PRI_INTR);
		dev_dbg(smmu->dev, "IRQ_STA:0x%x, TCU PRI interrupt detected pending_cnt: %d\n",
			irq_sta, pend_cnt);
	}

	if (irq_sta & STA_TCU_PMU_INTR) {
		pend_cnt = smmuwp_consume_intr(wp_base, STA_TCU_PMU_INTR);
		dev_dbg(smmu->dev, "IRQ_STA:0x%x, TCU PMU interrupt detected pending_cnt: %d\n",
			irq_sta, pend_cnt);
	}

	return 0;
}

/*
 * SMMU TF-A SMC cmd format:
 * sec[11:11] + smmu_type[10:8] + cmd_id[7:0]
 */
#define SMMU_ATF_SET_CMD(smmu_type, sec, cmd_id) \
	((cmd_id) | ((smmu_type) << 8) | ((sec) << 11))

enum smmu_atf_cmd {
	SMMU_SECURE_PM_GET,
	SMMU_SECURE_PM_PUT,
	SMMU_CMD_NUM
};

/*
 * a0/in0 = MTK_IOMMU_SECURE_CONTROL(IOMMU SMC ID)
 * a1/in1 = SMMU TF-A SMC cmd (sec + smmu_type + cmd_id)
 * a2/in2 ~ a7/in7: user defined
 */
static int mtk_smmu_atf_call(u32 smmu_type, unsigned long cmd,
			     unsigned long in2, unsigned long in3, unsigned long in4,
			     unsigned long in5, unsigned long in6, unsigned long in7)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_KERNEL_IOMMU_CONTROL, cmd, in2, in3, in4, in5, in6, in7, &res);

	return res.a0;
}

static int mtk_smmu_atf_call_common(u32 smmu_type, unsigned long cmd_id)
{
	unsigned long cmd = SMMU_ATF_SET_CMD(smmu_type, 1, cmd_id);

	return mtk_smmu_atf_call(smmu_type, cmd, 0, 0, 0, 0, 0, 0);
}

static int mtk_smmu_pm_get(struct device *dev, uint32_t smmu_type)
{
	int ret;

	ret = mtk_smmu_atf_call_common(smmu_type, SMMU_SECURE_PM_GET);
	if (ret) {
		dev_dbg(dev, "%s, smc call fail. ret:%d, type:%u\n", __func__, ret, smmu_type);
		return -ENODEV;
	}
	return 0;
}

static int mtk_smmu_pm_put(struct device *dev, uint32_t smmu_type)
{
	int ret;

	ret = mtk_smmu_atf_call_common(smmu_type, SMMU_SECURE_PM_PUT);
	if (ret) {
		dev_dbg(dev, "%s, smc call fail:%d, type:%u\n", __func__, ret, smmu_type);
		return -EINVAL;
	}
	return 0;
}

static int mtk_smmu_power_get(struct arm_smmu_device *smmu)
{
	struct mtk_smmu_v3 *mtk_smmuv3 = to_mtk_smmu_v3(smmu);
	const struct mtk_smmu_v3_plat *plat_data = mtk_smmuv3->plat_data;

	return mtk_smmu_pm_get(smmu->dev, plat_data->smmu_type);
}

static int mtk_smmu_power_put(struct arm_smmu_device *smmu)
{
	struct mtk_smmu_v3 *mtk_smmuv3 = to_mtk_smmu_v3(smmu);
	const struct mtk_smmu_v3_plat *plat_data = mtk_smmuv3->plat_data;

	return mtk_smmu_pm_put(smmu->dev, plat_data->smmu_type);
}

static const struct arm_smmu_v3_impl mtk_smmu_v3_impl = {
	.combined_irq_handle = mtk_smmu_v3_smmuwp_irq_handler,
	.smmu_evt_handler = mtk_smmu_evt_handler,
	.smmu_power_get = mtk_smmu_power_get,
	.smmu_power_put = mtk_smmu_power_put,
};

struct arm_smmu_device *arm_smmu_v3_impl_mtk_init(struct arm_smmu_device *smmu)
{
	struct mtk_smmu_v3 *mtk_smmu_v3;
	struct device *dev = smmu->dev;
	struct platform_device *pdev = to_platform_device(dev), *parent_pdev;
	struct resource *res, wp_res;
	struct device_node *parent_node;

	mtk_smmu_v3 = devm_krealloc(dev, smmu, sizeof(*mtk_smmu_v3), GFP_KERNEL);
	if (!mtk_smmu_v3)
		return ERR_PTR(-ENOMEM);

	mtk_smmu_v3->smmu.impl = &mtk_smmu_v3_impl;
	mtk_smmu_v3->plat_data = mtk_smmu_v3_get_plat_data(dev->of_node);
	if (!mtk_smmu_v3->plat_data) {
		dev_err(dev, "Get platform data fail\n");
		return ERR_PTR(-EINVAL);
	}

	if (MTK_SMMU_HAS_FLAG(mtk_smmu_v3->plat_data, SMMU_REQUIRE_PARENT)) {
		parent_node = of_parse_phandle(dev->of_node, "mediatek,smmu-parent", 0);
		if (!parent_node) {
			dev_err(dev, "Lack its parent node.\n");
			return ERR_PTR(-EINVAL);
		}
		if (!of_device_is_available(parent_node)) {
			of_node_put(parent_node);
			return ERR_PTR(-EINVAL);
		}

		parent_pdev = of_find_device_by_node(parent_node);
		of_node_put(parent_node);
		if (!parent_pdev) {
			dev_err(dev, "Lack its parent devices.\n");
			return ERR_PTR(-ENODEV);
		}

		if (!platform_get_drvdata(parent_pdev)) {
			dev_err(dev, "Delay since its parent driver is not ready.\n");
			return ERR_PTR(-EPROBE_DEFER);
		}
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return ERR_PTR(-EINVAL);
	wp_res = DEFINE_RES_MEM(res->start + mtk_smmu_v3->plat_data->wp_offset,  SZ_4K);
	mtk_smmu_v3->wp_base = devm_ioremap_resource(dev, &wp_res);
	if (IS_ERR(mtk_smmu_v3->wp_base))
		return mtk_smmu_v3->wp_base;

	mtk_smmu_pm_get(dev, mtk_smmu_v3->plat_data->smmu_type);
	smmu_init_wpcfg(smmu);
	mtk_smmu_pm_put(dev, mtk_smmu_v3->plat_data->smmu_type);
	return &mtk_smmu_v3->smmu;
}
