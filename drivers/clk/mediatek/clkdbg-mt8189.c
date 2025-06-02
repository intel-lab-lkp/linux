// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 MediaTek Inc.
 * Author: Qiqi Wang <qiqi.wang@mediatek.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/seq_file.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <clk-mux.h>
#include "clkdbg.h"
#include "clkchk.h"
#include "clk-fmeter.h"

#define PWR_STA_GROUP_MT8189_NR 3

static void __iomem *scpsys_base;

const char * const *get_mt8189_all_clk_names(void)
{
	static const char * const clks[] = {
		/* topckgen */
		"axi_sel",
		"axi_peri_sel",
		"axi_u_sel",
		"bus_aximem_sel",
		"disp0_sel",
		"mminfra_sel",
		"uart_sel",
		"spi0_sel",
		"spi1_sel",
		"spi2_sel",
		"spi3_sel",
		"spi4_sel",
		"spi5_sel",
		"msdc_macro_0p_sel",
		"msdc5hclk_sel",
		"msdc50_0_sel",
		"aes_msdcfde_sel",
		"msdc_macro_1p_sel",
		"msdc30_1_sel",
		"msdc30_1_h_sel",
		"msdc_macro_2p_sel",
		"msdc30_2_sel",
		"msdc30_2_h_sel",
		"aud_intbus_sel",
		"atb_sel",
		"disp_pwm_sel",
		"usb_p0_sel",
		"ssusb_xhci_p0_sel",
		"usb_p1_sel",
		"ssusb_xhci_p1_sel",
		"usb_p2_sel",
		"ssusb_xhci_p2_sel",
		"usb_p3_sel",
		"ssusb_xhci_p3_sel",
		"usb_p4_sel",
		"ssusb_xhci_p4_sel",
		"i2c_sel",
		"seninf_sel",
		"seninf1_sel",
		"aud_engen1_sel",
		"aud_engen2_sel",
		"aes_ufsfde_sel",
		"ufs_sel",
		"ufs_mbist_sel",
		"aud_1_sel",
		"aud_2_sel",
		"venc_sel",
		"vdec_sel",
		"pwm_sel",
		"audio_h_sel",
		"mcupm_sel",
		"mem_sub_sel",
		"mem_sub_peri_sel",
		"mem_sub_u_sel",
		"emi_n_sel",
		"dsi_occ_sel",
		"ap2conn_host_sel",
		"img1_sel",
		"ipe_sel",
		"cam_sel",
		"camtm_sel",
		"dsp_sel",
		"sr_pka_sel",
		"dxcc_sel",
		"mfg_ref_sel",
		"mdp0_sel",
		"dp_sel",
		"edp_sel",
		"edp_favt_sel",
		"snps_eth_250m_sel",
		"snps_eth_62p4m_ptp_sel",
		"snps_eth_50m_rmii_sel",
		"sflash_sel",
		"gcpu_sel",
		"pcie_mac_tl_sel",
		"vdstx_dg_cts_sel",
		"pll_dpix_sel",
		"ecc_sel",
		"apll_i2sin0_m_sel",
		"apll_i2sin1_m_sel",
		"apll_i2sin2_m_sel",
		"apll_i2sin3_m_sel",
		"apll_i2sin4_m_sel",
		"apll_i2sin6_m_sel",
		"apll_i2sout0_m_sel",
		"apll_i2sout1_m_sel",
		"apll_i2sout2_m_sel",
		"apll_i2sout3_m_sel",
		"apll_i2sout4_m_sel",
		"apll_i2sout6_m_sel",
		"apll_fmi2s_m_sel",
		"apll_tdmout_m_sel",
		"mfg_sel_mfgpll",

		/* topckgen */
		"apll12_div_i2sin0",
		"apll12_div_i2sin1",
		"apll12_div_i2sout0",
		"apll12_div_i2sout1",
		"apll12_div_fmi2s",
		"apll12_div_tdmout_m",

		/* topckgen */
		"fmcnt_p0_en",
		"fmcnt_p1_en",
		"fmcnt_p2_en",
		"fmcnt_p3_en",
		"fmcnt_p4_en",
		"ssusb_f26m",
		"sspxtp_f26m",
		"usb2_phy_rf_p0_en",
		"usb2_phy_rf_p1_en",
		"usb2_phy_rf_p2_en",
		"usb2_phy_rf_p3_en",
		"usb2_phy_rf_p4_en",
		"usb2_26m_p0_en",
		"usb2_26m_p1_en",
		"usb2_26m_p2_en",
		"usb2_26m_p3_en",
		"usb2_26m_p4_en",
		"pcie_f26m",
		"ap2con",
		"eint_n",
		"TOPCKGEN_fmipi_csi_up26m",
		"eint_e",
		"eint_w",
		"eint_s",

		/* infracfg_ao */
		"ifrao_dma",
		"ifrao_debugsys",
		"ifrao_dbg_trace",
		"ifrao_cq_dma",

		/* apmixedsys */
		"armpll-ll",
		"armpll-bl",
		"ccipll",
		"mainpll",
		"univpll",
		"mmpll",
		"mfgpll",
		"apll1",
		"apll2",
		"emipll",
		"apupll2",
		"apupll",
		"tvdpll1",
		"tvdpll2",
		"ethpll",
		"msdcpll",
		"ufspll",

		/* pericfg_ao */
		"perao_uart0",
		"perao_uart1",
		"perao_uart2",
		"perao_uart3",
		"perao_pwm_h",
		"perao_pwm_b",
		"perao_pwm_fb1",
		"perao_pwm_fb2",
		"perao_pwm_fb3",
		"perao_pwm_fb4",
		"perao_disp_pwm0",
		"perao_disp_pwm1",
		"perao_spi0_b",
		"perao_spi1_b",
		"perao_spi2_b",
		"perao_spi3_b",
		"perao_spi4_b",
		"perao_spi5_b",
		"perao_spi0_h",
		"perao_spi1_h",
		"perao_spi2_h",
		"perao_spi3_h",
		"perao_spi4_h",
		"perao_spi5_h",
		"perao_axi",
		"perao_ahb_apb",
		"perao_tl",
		"perao_ref",
		"perao_i2c",
		"perao_dma_b",
		"perao_ssusb0_ref",
		"perao_ssusb0_frmcnt",
		"perao_ssusb0_sys",
		"perao_ssusb0_xhci",
		"perao_ssusb0_f",
		"perao_ssusb0_h",
		"perao_ssusb1_ref",
		"perao_ssusb1_frmcnt",
		"perao_ssusb1_sys",
		"perao_ssusb1_xhci",
		"perao_ssusb1_f",
		"perao_ssusb1_h",
		"perao_ssusb2_ref",
		"perao_ssusb2_frmcnt",
		"perao_ssusb2_sys",
		"perao_ssusb2_xhci",
		"perao_ssusb2_f",
		"perao_ssusb2_h",
		"perao_ssusb3_ref",
		"perao_ssusb3_frmcnt",
		"perao_ssusb3_sys",
		"perao_ssusb3_xhci",
		"perao_ssusb3_f",
		"perao_ssusb3_h",
		"perao_ssusb4_ref",
		"perao_ssusb4_frmcnt",
		"perao_ssusb4_sys",
		"perao_ssusb4_xhci",
		"perao_ssusb4_f",
		"perao_ssusb4_h",
		"perao_msdc0",
		"perao_msdc0_h",
		"perao_msdc0_faes",
		"perao_msdc0_mst_f",
		"perao_msdc0_slv_h",
		"perao_msdc1",
		"perao_msdc1_h",
		"perao_msdc1_mst_f",
		"perao_msdc1_slv_h",
		"perao_msdc2",
		"perao_msdc2_h",
		"perao_msdc2_mst_f",
		"perao_msdc2_slv_h",
		"perao_sflash",
		"perao_sflash_f",
		"perao_sflash_h",
		"perao_sflash_p",
		"perao_audio0",
		"perao_audio1",
		"perao_audio2",
		"perao_auxadc_26m",

		/* afe */
		"afe_dl0_dac_tml",
		"afe_dl0_dac_hires",
		"afe_dl0_dac",
		"afe_dl0_predis",
		"afe_dl0_nle",
		"afe_pcm0",
		"afe_cm1",
		"afe_cm0",
		"afe_hw_gain23",
		"afe_hw_gain01",
		"afe_fm_i2s",
		"afe_mtkaifv4",
		"afe_dmic1_aht",
		"afe_dmic1_adc_hires",
		"afe_dmic1_tml",
		"afe_dmic1_adc",
		"afe_dmic0_aht",
		"afe_dmic0_adc_hires",
		"afe_dmic0_tml",
		"afe_dmic0_adc",
		"afe_ul0_aht",
		"afe_ul0_adc_hires",
		"afe_ul0_tml",
		"afe_ul0_adc",
		"afe_etdm_in1",
		"afe_etdm_in0",
		"afe_etdm_out4",
		"afe_etdm_out1",
		"afe_etdm_out0",
		"afe_tdm_out",
		"afe_general4_asrc",
		"afe_general3_asrc",
		"afe_general2_asrc",
		"afe_general1_asrc",
		"afe_general0_asrc",
		"afe_connsys_i2s_asrc",
		"afe_audio_hopping_ck",
		"afe_audio_f26m_ck",
		"afe_apll1_ck",
		"afe_apll2_ck",
		"afe_h208m_ck",
		"afe_apll_tuner2",
		"afe_apll_tuner1",

		/* ufscfg_ao_reg */
		"ufscfg_ao_unipro_tx_sym",
		"ufscfg_ao_unipro_rx_sym0",
		"ufscfg_ao_unipro_rx_sym1",
		"ufscfg_ao_unipro_sys",
		"ufscfg_ao_u_sap_cfg",
		"ufscfg_ao_u_phy_ahb_s_bus",

		/* ufscfg_pdn_reg */
		"ufscfg_ufshci_ufs",
		"ufscfg_ufshci_aes",
		"ufscfg_ufshci_u_ahb",
		"ufscfg_ufshci_u_axi",

		/* imp_iic_wrap_ws */
		"impws_i2c2",

		/* imp_iic_wrap_e */
		"impe_i2c0",
		"impe_i2c1",

		/* imp_iic_wrap_s */
		"imps_i2c3",
		"imps_i2c4",
		"imps_i2c5",
		"imps_i2c6",

		/* imp_iic_wrap_en */
		"impen_i2c7",
		"impen_i2c8",

		/* mfg */
		"mfg_bg3d",

		/* dispsys_config */
		"mm_disp_ovl0_4l",
		"mm_disp_ovl1_4l",
		"mm_vpp_rsz0",
		"mm_vpp_rsz1",
		"mm_disp_rdma0",
		"mm_disp_rdma1",
		"mm_disp_color0",
		"mm_disp_color1",
		"mm_disp_ccorr0",
		"mm_disp_ccorr1",
		"mm_disp_ccorr2",
		"mm_disp_ccorr3",
		"mm_disp_aal0",
		"mm_disp_aal1",
		"mm_disp_gamma0",
		"mm_disp_gamma1",
		"mm_disp_dither0",
		"mm_disp_dither1",
		"mm_disp_dsc_wrap0",
		"mm_vpp_merge0",
		"mmsys_0_disp_dvo",
		"mmsys_0_CLK0",
		"mm_dp_intf0",
		"mm_dpi0",
		"mm_disp_wdma0",
		"mm_disp_wdma1",
		"mm_disp_fake_eng0",
		"mm_disp_fake_eng1",
		"mm_smi_larb",
		"mm_disp_mutex0",
		"mm_dipsys_config",
		"mm_dummy",
		"mmsys_1_CLK0",
		"mmsys_1_lvds_encoder",
		"mmsys_1_dpi0",
		"mmsys_1_disp_dvo",
		"mm_dp_intf",
		"mmsys_1_lvds_encoder_cts",
		"mmsys_1_disp_dvo_avt",

		/* imgsys1 */
		"imgsys1_larb9",
		"imgsys1_larb11",
		"imgsys1_dip",
		"imgsys1_gals",

		/* imgsys2 */
		"imgsys2_larb9",
		"imgsys2_larb11",
		"imgsys2_mfb",
		"imgsys2_wpe",
		"imgsys2_mss",
		"imgsys2_gals",

		/* vdec_core */
		"vdec_core_larb_cken",
		"vdec_core_vdec_cken",
		"vdec_core_vdec_active",

		/* venc_gcon */
		"ven1_larb",
		"ven1_venc",
		"ven1_jpgenc",
		"ven1_jpgdec",
		"ven1_jpgdec_c1",
		"ven1_gals",
		"ven1_gals_sram",

		/* vlpcfg_reg */
		"vlpcfg_scp_ck",
		"vlpcfg_r_apxgpt_26m_ck",
		"vlpcfg_dpmsrck_test_ck",
		"vlpcfg_dpmsrrtc_test_ck",
		"vlpcfg_dpmsrulp_test_ck",
		"vlpcfg_spmi_p_ck",
		"vlpcfg_spmi_p_32k_ck",
		"vlpcfg_pmif_spmi_p_sys_ck",
		"vlpcfg_pmif_spmi_p_tmr_ck",
		"vlpcfg_pmif_spmi_m_sys_ck",
		"vlpcfg_pmif_spmi_m_tmr_ck",
		"vlpcfg_dvfsrc_ck",
		"vlpcfg_pwm_vlp_ck",
		"vlpcfg_srck_ck",
		"vlpcfg_sspm_f26m_ck",
		"vlpcfg_sspm_f32k_ck",
		"vlpcfg_sspm_ulposc_ck",
		"vlpcfg_vlp_32k_com_ck",
		"vlpcfg_vlp_26m_com_ck",
		/* vlp_cksys */
		"vlp_scp_sel",
		"vlp_pwrap_ulposc_sel",
		"vlp_spmi_p_sel",
		"vlp_dvfsrc_sel",
		"vlp_pwm_vlp_sel",
		"vlp_axi_vlp_sel",
		"vlp_systimer_26m_sel",
		"vlp_sspm_sel",
		"vlp_sspm_f26m_sel",
		"vlp_srck_sel",
		"vlp_scp_spi_sel",
		"vlp_scp_iic_sel",
		"vlp_scp_spi_hs_sel",
		"vlp_scp_iic_hs_sel",
		"vlp_sspm_ulposc_sel",
		"vlp_apxgpt_26m_sel",
		"vlp_vadsp_sel",
		"vlp_vadsp_vowpll_sel",
		"vlp_vadsp_uarthub_b_sel",
		"vlp_camtg0_sel",
		"vlp_camtg1_sel",
		"vlp_camtg2_sel",
		"vlp_aud_adc_sel",
		"vlp_kp_irq_gen_sel",

		/* vlp_cksys */
		"vlp_vadsys_vlp_26m",
		"VLP_fmipi_csi_up26m",

		/* scp_iic */
		"scp_iic_i2c0_w1s",
		"scp_iic_i2c1_w1s",

		/* scp */
		"scp_set_spi0",
		"scp_set_spi1",

		/* vadsys */
		"vad_core0",
		"vad_busemi_en",
		"vad_timer_en",
		"vad_dma0_en",
		"vad_uart_en",
		"vad_vowpll_en",
		"vadsys_26m",
		"vadsys_bus",

		/* camsys_main */
		"cam_m_larb13",
		"cam_m_larb14",
		"cam_m_camsys_main_cam",
		"cam_m_camsys_main_camtg",
		"cam_m_seninf",
		"cam_m_camsv1",
		"cam_m_camsv2",
		"cam_m_camsv3",
		"cam_m_fake_eng",
		"cam_m_cam2mm_gals",
		"cam_m_camsv4",
		"cam_m_pda",

		/* camsys_rawa */
		"cam_ra_camsys_rawa_larbx",
		"cam_ra_camsys_rawa_cam",
		"cam_ra_camsys_rawa_camtg",

		/* camsys_rawb */
		"cam_rb_camsys_rawb_larbx",
		"cam_rb_camsys_rawb_cam",
		"cam_rb_camsys_rawb_camtg",

		/* ipesys */
		"ipe_larb19",
		"ipe_larb20",
		"ipe_smi_subcom",
		"ipe_fd",
		"ipe_fe",
		"ipe_rsc",
		"ipesys_gals",

		/* vlpcfg_ao_reg */
		"en",

		/* dvfsrc_top */
		"dvfsrc_dvfsrc_en",

		/* mminfra_config */
		"mminfra_gce_d",
		"mminfra_gce_m",
		"mminfra_smi",
		"mminfra_gce_26m",

		/* gce_d */
		"gce_d_top",

		/* gce_m */
		"gce_m_top",

		/* mdpsys_config */
		"mdp_mutex0",
		"mdp_apb_bus",
		"mdp_smi0",
		"mdp_rdma0",
		"mdp_rdma2",
		"mdp_hdr0",
		"mdp_aal0",
		"mdp_rsz0",
		"mdp_tdshp0",
		"mdp_color0",
		"mdp_wrot0",
		"mdp_fake_eng0",
		"mdpsys_config",
		"mdp_rdma1",
		"mdp_rdma3",
		"mdp_hdr1",
		"mdp_aal1",
		"mdp_rsz1",
		"mdp_tdshp1",
		"mdp_color1",
		"mdp_wrot1",
		"mdp_rsz2",
		"mdp_wrot2",
		"mdp_rsz3",
		"mdp_wrot3",
		"mdp_birsz0",
		"mdp_birsz1",

		/* dbgao */
		"dbgao_atb_en",

		/* dem */
		"dem_atb_en",
		"dem_busclk_en",
		"dem_sysclk_en",
	};

	return clks;
}


/*
 * clkdbg dump all fmeter clks
 */
static const struct fmeter_clk *get_all_fmeter_clks(void)
{
	return mt_get_fmeter_clks();
}

static u32 fmeter_freq_op(const struct fmeter_clk *fclk)
{
	return mt_get_fmeter_freq(fclk->id, fclk->type);
}

static const char * const *get_pwr_names(void)
{
	static const char * const pwr_names[] = {
		/* PWR_STATUS & PWR_STATUS_2ND */
		[0] = "",
		[1] = "CONN",
		[2] = "IFR",
		[3] = "PERI",
		[4] = "UFS0",
		[5] = "",
		[6] = "AUDIO",
		[7] = "ADSP_TOP",
		[8] = "ADSP_INFRA",
		[9] = "ADSP_AO",
		[10] = "ISP_IMG1",
		[11] = "ISP_IMG2",
		[12] = "ISP_IPE",
		[13] = "",
		[14] = "VDE0",
		[15] = "",
		[16] = "VEN0",
		[17] = "",
		[18] = "CAM_MAIN",
		[19] = "",
		[20] = "CAM_SUBA",
		[21] = "CAM_SUBB",
		[22] = "",
		[23] = "",
		[24] = "",
		[25] = "",
		[26] = "MDP0",
		[27] = "",
		[28] = "DIS0",
		[29] = "",
		[30] = "MM_INFRA",
		[31] = "",
		/* PWR_MSB_STATUS & PWR_MSB_STATUS_2ND */
		[32] = "DP_TX",
		[33] = "SCP_CORE",
		[34] = "SCP_PERI",
		[35] = "DPM0",
		[36] = "DPM1",
		[37] = "EMI0",
		[38] = "EMI1",
		[39] = "CSI_RX",
		[40] = "",
		[41] = "SSPM",
		[42] = "SSUSB",
		[43] = "SSUSB_PHY",
		[44] = "EDP_TX",
		[45] = "PCIE",
		[46] = "PCIE_PHY",
		[47] = "",
		[48] = "",
		[49] = "",
		[50] = "",
		[51] = "",
		[52] = "",
		[53] = "",
		[54] = "",
		[55] = "",
		[56] = "",
		[57] = "",
		[58] = "",
		[59] = "",
		[60] = "",
		[61] = "",
		[62] = "",
		[63] = "",
		/* CPU_PWR_STATUS & CPU_PWR_STATUS_2ND */
		[64] = "C_EB",
		[65] = "MFG0",
		[66] = "MFG1",
		[67] = "MFG2",
		[68] = "MFG3",
		[69] = "",
		[70] = "",
		[71] = "",
		[72] = "",
		[73] = "",
		[74] = "",
		[75] = "",
		[76] = "",
		[77] = "",
		[78] = "",
		[79] = "",
		[80] = "",
		[81] = "",
		[82] = "",
		[83] = "",
		[84] = "",
		[85] = "",
		[86] = "",
		[87] = "",
		[88] = "",
		[89] = "",
		[90] = "",
		[91] = "",
		[92] = "",
		[93] = "",
		[94] = "",
		[95] = "",
		/* END */
		[96] = NULL,
	};

	return pwr_names;
}

static u32 _get_pwr_status(u32 pwr_sta_ofs, u32 pwr_sta_2nd_ofs)
{
	static void __iomem *pwr_sta, *pwr_sta_2nd;

	pwr_sta = scpsys_base + pwr_sta_ofs;
	pwr_sta_2nd = scpsys_base + pwr_sta_2nd_ofs;

	return readl(pwr_sta) & readl(pwr_sta_2nd);
}

static u32 *get_all_pwr_status(void)
{
	static struct regs {
		u32 pwr_sta_ofs;
		u32 pwr_sta_2nd_ofs;
	} g[] = {
		{0xF40, 0xF44},
		{0xF48, 0xF4C},
		{0xF50, 0xF54},
	};

	static u32 pwr_sta[PWR_STA_GROUP_MT8189_NR];
	int i;

	for (i = 0; i < PWR_STA_GROUP_MT8189_NR; i++)
		pwr_sta[i] = _get_pwr_status(g[i].pwr_sta_ofs, g[i].pwr_sta_2nd_ofs);

	return pwr_sta;
}

/*
 * init functions
 */

static struct clkdbg_ops clkdbg_mt8189_ops = {
	.get_all_fmeter_clks = get_all_fmeter_clks,
	.prepare_fmeter = NULL,
	.unprepare_fmeter = NULL,
	.fmeter_freq = fmeter_freq_op,
	.get_all_clk_names = get_mt8189_all_clk_names,
	.get_pwr_names = get_pwr_names,
	.get_all_pwr_status = get_all_pwr_status,
};

static int clk_dbg_mt8189_probe(struct platform_device *pdev)
{
	set_clkdbg_ops(&clkdbg_mt8189_ops);

	return 0;
}

static struct platform_driver clk_dbg_mt8189_drv = {
	.probe = clk_dbg_mt8189_probe,
	.driver = {
		.name = "clk-dbg-mt8189",
		.owner = THIS_MODULE,
	},
};

/*
 * init functions
 */

static int __init clkdbg_mt8189_init(void)
{
	scpsys_base = ioremap(0x1C001000, PAGE_SIZE);

	return clk_dbg_driver_register(&clk_dbg_mt8189_drv, "clk-dbg-mt8189");
}

static void __exit clkdbg_mt8189_exit(void)
{
	platform_driver_unregister(&clk_dbg_mt8189_drv);
}

subsys_initcall(clkdbg_mt8189_init);
module_exit(clkdbg_mt8189_exit);
MODULE_LICENSE("GPL");
