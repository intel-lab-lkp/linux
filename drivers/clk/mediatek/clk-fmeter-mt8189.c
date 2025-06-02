// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 MediaTek Inc.
 * Author: Qiqi Wang <qiqi.wang@mediatek.com>
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "clk-fmeter.h"
#include "clk-mt8189-fmeter.h"

#define FM_TIMEOUT			30

#define FM_PLL_CK			0
#define FM_PLL_CKDIV_CK			1
#define FM_CKDIV_SHIFT			(7)
#define FM_CKDIV_MASK			GENMASK(10, 7)
#define FM_POSTDIV_SHIFT		(24)
#define FM_POSTDIV_MASK			GENMASK(26, 24)

static DEFINE_SPINLOCK(meter_lock);
#define fmeter_lock(flags)   spin_lock_irqsave(&meter_lock, flags)
#define fmeter_unlock(flags) spin_unlock_irqrestore(&meter_lock, flags)

#define clk_readl(addr)		readl(addr)
#define clk_writel(addr, val)	\
	do { writel(val, addr); wmb(); } while (0) /* sync write */

/* check from topckgen&vlpcksys CODA */
#define CLK26CALI_0					(0x220)
#define CLK26CALI_1					(0x224)
#define CLK_MISC_CFG_0					(0x140)
#define CLK_DBG_CFG					(0x17C)
#define VLP_FQMTR_CON0					(0x230)
#define VLP_FQMTR_CON1					(0x234)

static void __iomem *fm_base[FM_SYS_NUM];

struct fmeter_data {
	enum fm_sys_id type;
	const char *name;
	unsigned int pll_con0;
	unsigned int pll_con1;
	unsigned int con0;
	unsigned int con1;
};

static struct fmeter_data subsys_fm[] = {
	[FM_VLP_CKSYS] = {FM_VLP_CKSYS, "fm_vlp_cksys",
		0, 0, VLP_FQMTR_CON0, VLP_FQMTR_CON1},
};

const char *comp_list[] = {
	[FM_APMIXEDSYS] = "mediatek,mt8189-apmixedsys",
	[FM_TOPCKGEN] = "mediatek,mt8189-topckgen",
	[FM_VLP_CKSYS] = "mediatek,mt8189-vlp_ckgen",
};

/*
 * clk fmeter
 */

#define FMCLK3(_t, _i, _n, _o, _g, _c) { .type = _t, \
		.id = _i, .name = _n, .ofs = _o, .grp = _g, .ck_div = _c}
#define FMCLK2(_t, _i, _n, _o, _p, _c) { .type = _t, \
		.id = _i, .name = _n, .ofs = _o, .pdn = _p, .ck_div = _c}
#define FMCLK(_t, _i, _n, _c) { .type = _t, .id = _i, .name = _n, .ck_div = _c}

static const struct fmeter_clk fclks[] = {
	/* CKGEN Part */
	FMCLK2(CKGEN, FM_AXI_CK, "fm_axi_ck", 0x0010, 7, 1),
	FMCLK2(CKGEN, FM_AXI_PERI_CK, "fm_axi_peri_ck", 0x0010, 15, 1),
	FMCLK2(CKGEN, FM_AXI_U_CK, "fm_axi_u_ck", 0x0010, 23, 1),
	FMCLK2(CKGEN, FM_BUS_AXIMEM_CK, "fm_bus_aximem_ck", 0x0010, 31, 1),
	FMCLK2(CKGEN, FM_DISP0_CK, "fm_disp0_ck", 0x0020, 7, 1),
	FMCLK2(CKGEN, FM_MMINFRA_CK, "fm_mminfra_ck", 0x0020, 15, 1),
	FMCLK2(CKGEN, FM_UART_CK, "fm_uart_ck", 0x0020, 23, 1),
	FMCLK2(CKGEN, FM_SPI0_CK, "fm_spi0_ck", 0x0020, 31, 1),
	FMCLK2(CKGEN, FM_SPI1_CK, "fm_spi1_ck", 0x0030, 7, 1),
	FMCLK2(CKGEN, FM_SPI2_CK, "fm_spi2_ck", 0x0030, 15, 1),
	FMCLK2(CKGEN, FM_SPI3_CK, "fm_spi3_ck", 0x0030, 23, 1),
	FMCLK2(CKGEN, FM_SPI4_CK, "fm_spi4_ck", 0x0030, 31, 1),
	FMCLK2(CKGEN, FM_SPI5_CK, "fm_spi5_ck", 0x0040, 7, 1),
	FMCLK2(CKGEN, FM_MSDC_MACRO_0P_CK, "fm_msdc_macro_0p_ck", 0x0040, 15, 1),
	FMCLK2(CKGEN, FM_MSDC5HCLK_CK, "fm_msdc5hclk_ck", 0x0040, 23, 1),
	FMCLK2(CKGEN, FM_MSDC50_0_CK, "fm_msdc50_0_ck", 0x0040, 31, 1),
	FMCLK2(CKGEN, FM_AES_MSDCFDE_CK, "fm_aes_msdcfde_ck", 0x0050, 7, 1),
	FMCLK2(CKGEN, FM_MSDC_MACRO_1P_CK, "fm_msdc_macro_1p_ck", 0x0050, 15, 1),
	FMCLK2(CKGEN, FM_MSDC30_1_CK, "fm_msdc30_1_ck", 0x0050, 23, 1),
	FMCLK2(CKGEN, FM_MSDC30_1_H_CK, "fm_msdc30_1_h_ck", 0x0050, 31, 1),
	FMCLK2(CKGEN, FM_MSDC_MACRO_2P_CK, "fm_msdc_macro_2p_ck", 0x0060, 7, 1),
	FMCLK2(CKGEN, FM_MSDC30_2_CK, "fm_msdc30_2_ck", 0x0060, 15, 1),
	FMCLK2(CKGEN, FM_MSDC30_2_2, "fm_msdc30_2_2", 0x0060, 15, 1),
	FMCLK2(CKGEN, FM_AUD_INTBUS_CK, "fm_aud_intbus_ck", 0x0060, 31, 1),
	FMCLK2(CKGEN, FM_ATB_CK, "fm_atb_ck", 0x0070, 7, 1),
	FMCLK2(CKGEN, FM_DISP_PWM_CK, "fm_disp_pwm_ck", 0x0070, 15, 1),
	FMCLK2(CKGEN, FM_USB_P0_CK, "fm_usb_p0_ck", 0x0070, 23, 1),
	FMCLK2(CKGEN, FM_USB_XHCI_P0_CK, "fm_usb_xhci_p0_ck", 0x0070, 31, 1),
	FMCLK2(CKGEN, FM_USB_P1_CK, "fm_usb_p1_ck", 0x0080, 7, 1),
	FMCLK2(CKGEN, FM_USB_XHCI_P1_CK, "fm_usb_xhci_p1_ck", 0x0080, 15, 1),
	FMCLK2(CKGEN, FM_USB_P2_CK, "fm_usb_p2_ck", 0x0080, 23, 1),
	FMCLK2(CKGEN, FM_USB_XHCI_P2_CK, "fm_usb_xhci_p2_ck", 0x0080, 31, 1),
	FMCLK2(CKGEN, FM_USB_P3_CK, "fm_usb_p3_ck", 0x0090, 7, 1),
	FMCLK2(CKGEN, FM_USB_XHCI_P3_CK, "fm_usb_xhci_p3_ck", 0x0090, 15, 1),
	FMCLK2(CKGEN, FM_USB_P4_CK, "fm_usb_p4_ck", 0x0090, 23, 1),
	FMCLK2(CKGEN, FM_USB_XHCI_P4_CK, "fm_usb_xhci_p4_ck", 0x0090, 31, 1),
	FMCLK2(CKGEN, FM_I2C_CK, "fm_i2c_ck", 0x00A0, 7, 1),
	FMCLK2(CKGEN, FM_SENINF_CK, "fm_seninf_ck", 0x00A0, 15, 1),
	FMCLK2(CKGEN, FM_SENINF1_CK, "fm_seninf1_ck", 0x00A0, 23, 1),
	FMCLK2(CKGEN, FM_AUD_ENGEN1_CK, "fm_aud_engen1_ck", 0x00A0, 31, 1),
	FMCLK2(CKGEN, FM_AUD_ENGEN2_CK, "fm_aud_engen2_ck", 0x00B0, 7, 1),
	FMCLK2(CKGEN, FM_AES_UFSFDE_CK, "fm_aes_ufsfde_ck", 0x00B0, 15, 1),
	FMCLK2(CKGEN, FM_U_CK, "fm_u_ck", 0x00B0, 23, 1),
	FMCLK2(CKGEN, FM_U_MBIST_CK, "fm_u_mbist_ck", 0x00B0, 31, 1),
	FMCLK2(CKGEN, FM_AUD_1_CK, "fm_aud_1_ck", 0x00C0, 7, 1),
	FMCLK2(CKGEN, FM_AUD_2_CK, "fm_aud_2_ck", 0x00C0, 15, 1),
	FMCLK2(CKGEN, FM_VENC_CK, "fm_venc_ck", 0x00C0, 23, 1),
	FMCLK2(CKGEN, FM_VDEC_CK, "fm_vdec_ck", 0x00C0, 31, 1),
	FMCLK2(CKGEN, FM_PWM_CK, "fm_pwm_ck", 0x00D0, 7, 1),
	FMCLK2(CKGEN, FM_AUDIO_H_CK, "fm_audio_h_ck", 0x00D0, 15, 1),
	FMCLK2(CKGEN, FM_MCUPM_CK, "fm_mcupm_ck", 0x00D0, 23, 1),
	FMCLK2(CKGEN, FM_MEM_SUB_CK, "fm_mem_sub_ck", 0x00D0, 31, 1),
	FMCLK2(CKGEN, FM_MEM_SUB_PERI_CK, "fm_mem_sub_peri_ck", 0x00E0, 7, 1),
	FMCLK2(CKGEN, FM_MEM_SUB_U_CK, "fm_mem_sub_u_ck", 0x00E0, 15, 1),
	FMCLK2(CKGEN, FM_EMI_N_CK, "fm_emi_n_ck", 0x00E0, 23, 1),
	FMCLK2(CKGEN, FM_DSI_OCC_CK, "fm_dsi_occ_ck", 0x00E0, 31, 1),
	FMCLK2(CKGEN, FM_AP2CONN_HOST_CK, "fm_ap2conn_host_ck", 0x00F0, 7, 1),
	FMCLK2(CKGEN, FM_IMG1_CK, "fm_img1_ck", 0x00F0, 15, 1),
	FMCLK2(CKGEN, FM_IPE_CK, "fm_ipe_ck", 0x00F0, 23, 1),
	FMCLK2(CKGEN, FM_CAM_CK, "fm_cam_ck", 0x00F0, 31, 1),
	FMCLK2(CKGEN, FM_CAMTM_CK, "fm_camtm_ck", 0x0100, 7, 1),
	FMCLK2(CKGEN, FM_DSP_CK, "fm_dsp_ck", 0x0100, 15, 1),
	FMCLK2(CKGEN, FM_SR_PKA_CK, "fm_sr_pka_ck", 0x0100, 23, 1),
	FMCLK2(CKGEN, FM_DXCC_CK, "fm_dxcc_ck", 0x0100, 31, 1),
	FMCLK2(CKGEN, FM_MFG_REF_CK, "fm_mfg_ref_ck", 0x0110, 7, 1),
	FMCLK2(CKGEN, FM_MDP0_CK, "fm_mdp0_ck", 0x0110, 15, 1),
	FMCLK2(CKGEN, FM_DP_CK, "fm_dp_ck", 0x0110, 23, 1),
	FMCLK2(CKGEN, FM_EDP_CK, "fm_edp_ck", 0x0110, 31, 1),
	FMCLK2(CKGEN, FM_EDP_FAVT_CK, "fm_edp_favt_ck", 0x0180, 7, 1),
	FMCLK2(CKGEN, FM_ETH_250M_CK, "fm_eth_250m_ck", 0x0180, 15, 1),
	FMCLK2(CKGEN, FM_ETH_62P4M_PTP_CK, "fm_eth_62p4m_ptp_ck", 0x0180, 23, 1),
	FMCLK2(CKGEN, FM_ETH_50M_RMII_CK, "fm_eth_50m_rmii_ck", 0x0180, 31, 1),
	FMCLK2(CKGEN, FM_SFLASH_CK, "fm_sflash_ck", 0x0190, 7, 1),
	FMCLK2(CKGEN, FM_GCPU_CK, "fm_gcpu_ck", 0x0190, 15, 1),
	FMCLK2(CKGEN, FM_CIE_MAC_TL_CK, "fm_cie_mac_tl_ck", 0x0190, 23, 1),
	FMCLK2(CKGEN, FM_VDSTX_DG_CTS_CK, "fm_vdstx_dg_cts_ck", 0x0190, 31, 1),
	FMCLK2(CKGEN, FM_PLL_DPIX_CK, "fm_pll_dpix_ck", 0x0240, 7, 1),
	FMCLK2(CKGEN, FM_ECC_CK, "fm_ecc_ck", 0x0240, 15, 1),
	/* ABIST Part */
	FMCLK(ABIST, FM_APLL1_CK, "fm_apll1_ck", 1),
	FMCLK(ABIST, FM_APLL2_CK, "fm_apll2_ck", 1),
	FMCLK(ABIST, FM_ARMPLL_BL_CK, "fm_armpll_bl_ck", 1),
	FMCLK3(ABIST, FM_ARMPLL_BL_CKDIV_CK, "fm_armpll_bl_ckdiv_ck", 0x0218, 3, 13),
	FMCLK(ABIST, FM_ARMPLL_LL_CK, "fm_armpll_ll_ck", 1),
	FMCLK3(ABIST, FM_ARMPLL_LL_CKDIV_CK, "fm_armpll_ll_ckdiv_ck", 0x0208, 3, 13),
	FMCLK(ABIST, FM_CCIPLL_CK, "fm_ccipll_ck", 1),
	FMCLK3(ABIST, FM_CCIPLL_CKDIV_CK, "fm_ccipll_ckdiv_ck", 0x0228, 3, 13),
	FMCLK3(ABIST, FM_MAINPLL_CKDIV_CK, "fm_mainpll_ckdiv_ck", 0x0308, 3, 13),
	FMCLK(ABIST, FM_MAINPLL_CK, "fm_mainpll_ck", 1),
	FMCLK3(ABIST, FM_MMPLL_CKDIV_CK, "fm_mmpll_ckdiv_ck", 0x0328, 3, 13),
	FMCLK(ABIST, FM_MMPLL_CK, "fm_mmpll_ck", 1),
	FMCLK(ABIST, FM_MMPLL_D3_CK, "fm_mmpll_d3_ck", 1),
	FMCLK(ABIST, FM_MSDCPLL_CK, "fm_msdcpll_ck", 1),
	FMCLK(ABIST, FM_UFSPLL_CK, "fm_ufspll_ck", 1),
	FMCLK(ABIST, FM_UNIVPLL_CK, "fm_univpll_ck", 1),
	FMCLK(ABIST, FM_UNIVPLL_192M_CK, "fm_univpll_192m_ck", 1),
	FMCLK3(ABIST, FM_APLL1_CKDIV_CK, "fm_apll1_ckdiv_ck", 0x0408, 3, 13),
	FMCLK3(ABIST, FM_APLL2_CKDIV_CK, "fm_apll2_ckdiv_ck", 0x041C, 3, 13),
	FMCLK3(ABIST, FM_UFSPLL_CKDIV_CK, "fm_ufspll_ckdiv_ck", 0x0538, 3, 13),
	FMCLK3(ABIST, FM_MSDCPLL_CKDIV_CK, "fm_msdcpll_ckdiv_ck", 0x0528, 3, 13),
	FMCLK(ABIST, FM_EMIPLL_CK, "fm_emipll_ck", 1),
	FMCLK(ABIST, FM_TVDPLL1_CK, "fm_tvdpll1_ck", 1),
	FMCLK(ABIST, FM_TVDPLL2_CK, "fm_tvdpll2_ck", 1),
	FMCLK(ABIST, FM_MFGPLL_OPP_CK, "fm_mfgpll_opp_ck", 1),
	FMCLK(ABIST, FM_ETHPLL_CK, "fm_ethpll_ck", 1),
	FMCLK(ABIST, FM_APUPLL_CK, "fm_apupll_ck", 1),
	FMCLK(ABIST, FM_APUPLL2_CK, "fm_apupll2_ck", 1),
	/* VLPCK Part */
	FMCLK2(VLPCK, FM_SCP_CK, "fm_scp_ck", 0x0008, 7, 1),
	FMCLK2(VLPCK, FM_PWRAP_ULPOSC_CK, "fm_pwrap_ulposc_ck", 0x0008, 15, 1),
	FMCLK2(VLPCK, FM_SPMI_P_CK, "fm_spmi_p_ck", 0x0008, 23, 1),
	FMCLK2(VLPCK, FM_DVFSRC_CK, "fm_dvfsrc_ck", 0x0008, 31, 1),
	FMCLK2(VLPCK, FM_PWM_VLP_CK, "fm_pwm_vlp_ck", 0x0014, 7, 1),
	FMCLK2(VLPCK, FM_AXI_VLP_CK, "fm_axi_vlp_ck", 0x0014, 15, 1),
	FMCLK2(VLPCK, FM_SYSTIMER_26M_CK, "fm_systimer_26m_ck", 0x0014, 23, 1),
	FMCLK2(VLPCK, FM_SSPM_CK, "fm_sspm_ck", 0x0014, 31, 1),
	FMCLK2(VLPCK, FM_SSPM_F26M_CK, "fm_sspm_f26m_ck", 0x0020, 7, 1),
	FMCLK2(VLPCK, FM_SRCK_CK, "fm_srck_ck", 0x0020, 15, 1),
	FMCLK2(VLPCK, FM_SCP_SPI_CK, "fm_scp_spi_ck", 0x0020, 23, 1),
	FMCLK2(VLPCK, FM_SCP_IIC_CK, "fm_scp_iic_ck", 0x0020, 31, 1),
	FMCLK2(VLPCK, FM_SCP_SPI_HS_CK, "fm_scp_spi_hs_ck", 0x002C, 7, 1),
	FMCLK2(VLPCK, FM_SCP_IIC_HS_CK, "fm_scp_iic_hs_ck", 0x002C, 15, 1),
	FMCLK2(VLPCK, FM_SSPM_ULPOSC_CK, "fm_sspm_ulposc_ck", 0x002C, 23, 1),
	FMCLK2(VLPCK, FM_APXGPT_26M_CK, "fm_apxgpt_26m_ck", 0x002C, 31, 1),
	FMCLK2(VLPCK, FM_VADSP_CK, "fm_vadsp_ck", 0x0038, 7, 1),
	FMCLK2(VLPCK, FM_VADSP_VOWPLL_CK, "fm_vadsp_vowpll_ck", 0x0038, 15, 1),
	FMCLK2(VLPCK, FM_VADSP_UARTHUB_B_CK, "fm_vadsp_uarthub_b_ck", 0x0038, 23, 1),
	FMCLK2(VLPCK, FM_CAMTG0_CK, "fm_camtg0_ck", 0x0038, 31, 1),
	FMCLK2(VLPCK, FM_CAMTG1_CK, "fm_camtg1_ck", 0x0044, 7, 1),
	FMCLK2(VLPCK, FM_CAMTG2_CK, "fm_camtg2_ck", 0x0044, 15, 1),
	FMCLK2(VLPCK, FM_AUD_ADC_CK, "fm_aud_adc_ck", 0x0044, 23, 1),
	FMCLK2(VLPCK, FM_KP_IRQ_GEN_CK, "fm_kp_irq_gen_ck", 0x0044, 31, 1),
	/* SUBSYS Part */
	{},
};

const struct fmeter_clk *mt8189_get_fmeter_clks(void)
{
	return fclks;
}

static unsigned int check_pdn(void __iomem *base,
		unsigned int type, unsigned int ID)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(fclks) - 1; i++) {
		if (fclks[i].type == type && fclks[i].id == ID)
			break;
	}

	if (i >= ARRAY_SIZE(fclks) - 1)
		return 1;

	if (!fclks[i].ofs)
		return 0;

	if (type == SUBSYS) {
		if ((clk_readl(base + fclks[i].ofs) & fclks[i].pdn)
				!= fclks[i].pdn) {
			return 1;
		}
	} else if (type != SUBSYS && ((clk_readl(base + fclks[i].ofs)
			& BIT(fclks[i].pdn)) == BIT(fclks[i].pdn)))
		return 1;

	return 0;
}

static unsigned int get_post_div(unsigned int type, unsigned int ID)
{
	unsigned int post_div = 1;
	int i;

	if ((ID <= 0) || (ID >= FM_ABIST_NUM))
		return post_div;

	for (i = 0; i < ARRAY_SIZE(fclks) - 1; i++) {
		if (fclks[i].type == type && fclks[i].id == ID
				&& fclks[i].grp != 0) {
			post_div =  clk_readl(fm_base[FM_APMIXEDSYS] + fclks[i].ofs);
			post_div = 1 << ((post_div >> 24) & 0x7);
			break;
		}
	}

	return post_div;
}

static unsigned int get_clk_div(unsigned int type, unsigned int ID)
{
	unsigned int clk_div = 1;
	int i;

	if ((ID <= 0) || (ID >= FM_CKGEN_NUM))
		return clk_div;

	for (i = 0; i < ARRAY_SIZE(fclks) - 1; i++)
		if (fclks[i].type == type && fclks[i].id == ID)
			break;

	if (i >= ARRAY_SIZE(fclks) - 1)
		return clk_div;

	return fclks[i].ck_div;
}

/* implement ckgen&abist api (example as below) */

static int __mt_get_freq(unsigned int ID, int type)
{
	void __iomem *dbg_addr = fm_base[FM_TOPCKGEN] + CLK_DBG_CFG;
	void __iomem *misc_addr = fm_base[FM_TOPCKGEN] + CLK_MISC_CFG_0;
	void __iomem *cali0_addr = fm_base[FM_TOPCKGEN] + CLK26CALI_0;
	void __iomem *cali1_addr = fm_base[FM_TOPCKGEN] + CLK26CALI_1;
	unsigned int temp, clk_dbg_cfg, clk_misc_cfg_0, clk26cali_1 = 0;
	unsigned int clk_div = 1, post_div = 1;
	unsigned long flags;
	int output = 0, i = 0;

	fmeter_lock(flags);

	if (type == CKGEN && check_pdn(fm_base[FM_TOPCKGEN], CKGEN, ID)) {
		pr_notice("ID-%d: MUX PDN, return 0.\n", ID);
		fmeter_unlock(flags);
		return 0;
	}

	while (clk_readl(cali0_addr) & 0x10) {
		udelay(10);
		i++;
		if (i > FM_TIMEOUT)
			break;
	}

	/* CLK26CALI_0[15]: rst 1 -> 0 */
	clk_writel(cali0_addr, (clk_readl(cali0_addr) & 0xFFFF7FFF));
	/* CLK26CALI_0[15]: rst 0 -> 1 */
	clk_writel(cali0_addr, (clk_readl(cali0_addr) | 0x00008000));

	if (type == CKGEN) {
		clk_dbg_cfg = clk_readl(dbg_addr);
		clk_writel(dbg_addr,
			(clk_dbg_cfg & 0xFFFF80FC) | (ID << 8) | (0x1));
	} else if (type == ABIST) {
		clk_dbg_cfg = clk_readl(dbg_addr);
		clk_writel(dbg_addr,
			(clk_dbg_cfg & 0xFF80FFFC) | (ID << 16));
	} else {
		fmeter_unlock(flags);
		return 0;
	}

	clk_misc_cfg_0 = clk_readl(misc_addr);
	clk_writel(misc_addr, (clk_misc_cfg_0 & 0x00FFFFFF) | (3 << 24));

	clk26cali_1 = clk_readl(cali1_addr);
	clk_writel(cali0_addr, 0x9000);
	clk_writel(cali0_addr, 0x9010);

	/* wait frequency meter finish */
	i = 0;
	do {
		udelay(10);
		i++;
		if (i > FM_TIMEOUT)
			break;
	} while (clk_readl(cali0_addr) & 0x10);

	temp = clk_readl(cali1_addr) & 0xFFFF;

	if (type == ABIST)
		post_div = get_post_div(type, ID);

	clk_div = get_clk_div(type, ID);

	output = (temp * 26000) / 1024 * clk_div / post_div;

	clk_writel(dbg_addr, clk_dbg_cfg);
	clk_writel(misc_addr, clk_misc_cfg_0);
	clk_writel(cali0_addr, 0x8000);
	clk_writel(cali1_addr, clk26cali_1);

	fmeter_unlock(flags);

	if (i > FM_TIMEOUT)
		return 0;

	if ((output * 4) < 1000) {
		pr_notice("%s(%d): CLK_DBG_CFG = 0x%x, CLK_MISC_CFG_0 = 0x%x, CLK26CALI_0 = 0x%x, CLK26CALI_1 = 0x%x\n",
			__func__,
			ID,
			clk_readl(dbg_addr),
			clk_readl(misc_addr),
			clk_readl(cali0_addr),
			clk_readl(cali1_addr));
	}

	return (output * 4);
}

/* implement ckgen&abist api (example as below) */
static int __mt_get_freq2(unsigned int  type, unsigned int id)
{
	void __iomem *con0 = fm_base[type] + subsys_fm[type].con0;
	void __iomem *con1 = fm_base[type] + subsys_fm[type].con1;
	unsigned int temp, clk_div = 1, post_div = 1;
	unsigned long flags;
	int output = 0, i = 0;

	fmeter_lock(flags);

	/* PLL4H_FQMTR_CON1[15]: rst 1 -> 0 */
	clk_writel(con0, clk_readl(con0) & 0xFFFF7FFF);
	/* PLL4H_FQMTR_CON1[15]: rst 0 -> 1 */
	clk_writel(con0, clk_readl(con0) | 0x8000);

	/* sel fqmtr_cksel */
	if (type == FM_VLP_CKSYS)
		clk_writel(con0, (clk_readl(con0) & 0xFFE0FFFF) | (id << 16));
	else
		clk_writel(con0, (clk_readl(con0) & 0x00FFFFF8) | (id << 0));
	/* set ckgen_load_cnt to 1024 */
	clk_writel(con1, (clk_readl(con1) & 0xFC00FFFF) | (0x3FF << 16));

	/* sel fqmtr_cksel and set ckgen_k1 to 0(DIV4) */
	clk_writel(con0, (clk_readl(con0) & 0x00FFFFFF) | (3 << 24));

	/* fqmtr_en set to 1, fqmtr_exc set to 0, fqmtr_start set to 0 */
	clk_writel(con0, (clk_readl(con0) & 0xFFFF8007) | 0x1000);
	/*fqmtr_start set to 1 */
	clk_writel(con0, clk_readl(con0) | 0x10);

	/* wait frequency meter finish */
	while (clk_readl(con0) & 0x10) {
		udelay(10);
		i++;
		if (i > 30) {
			pr_notice("[%d]con0: 0x%x, con1: 0x%x\n",
				id, clk_readl(con0), clk_readl(con1));
			break;
		}
	}

	temp = clk_readl(con1) & 0xFFFF;
	output = ((temp * 26000)) / 1024; // Khz

	if (clk_div == 0)
		clk_div = 1;

	clk_writel(con0, 0x8000);

	fmeter_unlock(flags);

	return (output * 4 * clk_div) / post_div;
}

static unsigned int mt8189_get_ckgen_freq(unsigned int ID)
{
	return __mt_get_freq(ID, CKGEN);
}

static unsigned int mt8189_get_abist_freq(unsigned int ID)
{
	return __mt_get_freq(ID, ABIST);
}

static unsigned int mt8189_get_vlpck_freq(unsigned int ID)
{
	return __mt_get_freq2(FM_VLP_CKSYS, ID);
}

static unsigned int mt8189_get_fmeter_freq(unsigned int id,
		enum FMETER_TYPE type)
{
	if (type == CKGEN)
		return mt8189_get_ckgen_freq(id);
	else if (type == ABIST)
		return mt8189_get_abist_freq(id);
	else if (type == VLPCK)
		return mt8189_get_vlpck_freq(id);

	return FT_NULL;
}

static void __iomem *get_base_from_comp(const char *comp)
{
	struct device_node *node;
	static void __iomem *base;

	node = of_find_compatible_node(NULL, NULL, comp);
	if (node) {
		base = of_iomap(node, 0);
		if (!base) {
			pr_info("%s() can't find iomem for %s\n",
					__func__, comp);
			return ERR_PTR(-EINVAL);
		}

		return base;
	}

	pr_info("%s can't find compatible node\n", __func__);

	return ERR_PTR(-EINVAL);
}

/*
 * init functions
 */

static struct fmeter_ops fm_ops = {
	.get_fmeter_clks = mt8189_get_fmeter_clks,
	.get_fmeter_freq = mt8189_get_fmeter_freq,
};

static int clk_fmeter_mt8189_probe(struct platform_device *pdev)
{
	int i;

	for (i = 0; i < FM_SYS_NUM; i++) {
		fm_base[i] = get_base_from_comp(comp_list[i]);
		if (IS_ERR(fm_base[i]))
			goto ERR;

	}

	fmeter_set_ops(&fm_ops);

	return 0;
ERR:
	pr_info("%s(%s) can't find base\n", __func__, comp_list[i]);

	return -EINVAL;
}

static struct platform_driver clk_fmeter_mt8189_drv = {
	.probe = clk_fmeter_mt8189_probe,
	.driver = {
		.name = "clk-fmeter-mt8189",
		.owner = THIS_MODULE,
	},
};

static int __init clk_fmeter_init(void)
{
	static struct platform_device *clk_fmeter_dev;

	clk_fmeter_dev = platform_device_register_simple("clk-fmeter-mt8189", -1, NULL, 0);
	if (IS_ERR(clk_fmeter_dev))
		pr_info("unable to register clk-fmeter device");

	return platform_driver_register(&clk_fmeter_mt8189_drv);
}

static void __exit clk_fmeter_exit(void)
{
	platform_driver_unregister(&clk_fmeter_mt8189_drv);
}

subsys_initcall(clk_fmeter_init);
module_exit(clk_fmeter_exit);
MODULE_LICENSE("GPL");
