/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Rockchip VEPU510 (RK3576) hardware video encoder registers.
 *
 * Copyright (C) 2026 Jiaxing Hu <gahing@gahingwoo.com>
 *
 * Field layout cross-checked against Rockchip's own open-source userspace
 * codec library (github.com/rockchip-linux/mpp,
 * mpp/hal/rkenc/{common/vepu510_common.h,h264e/hal_h264e_vepu510_reg.h},
 * Apache-2.0) — that library is the only place the RC/PIC/PARAM register
 * field names exist; the downstream rockchip-linux/kernel mpp_rkvenc2.c
 * driver treats those classes as an opaque byte blob and never names a
 * single field beyond the handful of control/status registers below.
 *
 * Register classes (byte offsets within the per-core MMIO window):
 *   BASE   0x0000-0x0120  control / IRQ / watchdog
 *   FRAME  0x0270-0x03f4  per-frame picture config + H.264 syntax
 *   RC_ROI 0x1000-0x110c  ROI / adaptive-quant helper tables
 *   PARAM  0x1700-0x19cc  RDO/ME/quant tuning tables
 *   SQI    0x2000-0x212c  subjective-quality tuning
 *   SCL    0x2200-0x2584  scaling lists
 *   STATUS 0x4000-0x424c  read-only status
 *   DEBUG  0x5000-0x5230  debug counters
 *
 * IMPORTANT — PARAM/SQI/SCL and the RC_ROI rc_adj/rc_dthd threshold ladder
 * are NOT optional despite mpp's own FIXQP-mode code path never touching
 * them (rc->rc_mode == MPP_ENC_RC_MODE_FIXQP returns early from
 * setup_vepu510_rc_base() before setting rc_adj0/1/rc_dthd_0_8, and
 * nothing in mpp ever documents PARAM/SQI as required). Board bring-up
 * spent a long time on exactly this assumption: a from-scratch test client
 * reproduced the driver's exact hardware-internal-watchdog stall
 * (int_sta.wdg_sta, ~50ms after kick) with these classes left at their
 * POR/never-written state, byte-for-byte matched everything else against a
 * real rockchip-linux/mpp `mpi_enc_test` capture, and the stall persisted
 * until PARAM+SQI+SCL were written and the RC_ROI threshold ladder was
 * populated (rc_dthd_0_8 left at 0 instead of the real "never trigger"
 * 0x7FFFFFFF sentinel is itself sufficient to reproduce the stall alone).
 * Whatever internal engine consumes these — almost certainly the RDO/
 * mode-decision and adaptive-quant hardware blocks — appears to need a
 * valid, non-degenerate configuration to complete at all, not just to
 * compute good output. See rkvenc-h264.c for what's actually written and
 * why (PAR/SQI are mpp's *default-tuning* constant tables, not per-frame
 * content-derived, so hardcoding them is a deliberate v1 choice, not a
 * placeholder).
 *
 * Each per-frame register below is a union of its named bitfields and a
 * raw u32 .val, so callers build the value field-by-field and write it
 * with a single rkvenc_write(dev, RKVENC_REG_*, reg.val).
 */

#ifndef RKVENC_REGS_H_
#define RKVENC_REGS_H_

#include <linux/bits.h>
#include <linux/types.h>

#define RKVENC_REG_FRAME_OFFSET		0x270
#define RKVENC_REG_STATUS_OFFSET	0x4000
#define RKVENC_REG_DEBUG_OFFSET		0x5000

/* ---- BASE class (0x0000-0x0120), a.k.a. Vepu510ControlCfg in mpp.
 * Bit layout below is transcribed directly from
 * mpp/hal/rkenc/common/vepu510_common.h (Vepu510ControlCfg_t) — this
 * supersedes an earlier, less precise reading of these same registers
 * from the downstream vendor *kernel* driver's comments (which got the
 * ENC_WDG field width/position and several INT_* bit meanings wrong).
 */
#define RKVENC_REG_VERSION		0x0000	/* read-only IP identification */
#define RKVENC_REG_ENC_START		0x0010	/* enc_strt */
#define RKVENC_REG_ENC_CLR		0x0014	/* bit0 safe_clr, bit1 force_clr */
#define RKVENC_ENC_CLR_SAFE		0x1
#define RKVENC_ENC_CLR_FORCE		0x2
#define RKVENC_SCLR_DONE_STA		BIT(2)	/* int_sta sclr_done, set when safe-clr completes */
#define RKVENC_REG_VS_LDLY		0x0018

union rkvenc_reg_version {
	struct {
		u32 sub_ver:8;
		u32 h264_cap:1;
		u32 hevc_cap:1;
		u32 reserved:2;
		u32 res_cap:4;
		u32 osd_cap:2;
		u32 filtr_cap:2;
		u32 bfrm_cap:1;
		u32 fbc_cap:2;
		u32 reserved1:1;
		u32 ip_id:8;
	};
	u32 val;
};
#define RKVENC_REG_INT_EN		0x0020
#define RKVENC_REG_INT_MASK		0x0024
#define RKVENC_REG_INT_CLR		0x0028
#define RKVENC_REG_INT_STA		0x002c
#define RKVENC_REG_DTRNS_MAP		0x0030
#define RKVENC_REG_DTRNS_CFG		0x0034
#define RKVENC_REG_ENC_WDG		0x0038
#define RKVENC_REG_OPT_STRG		0x0054

/* VEPU510-only "DVBM hold" quirk (see rkvenc_vepu510_quirk() in
 * rkvenc-h264.c). No field name exists in the vendor HAL for this — it is
 * not written by rockchip-linux/mpp at all, only by the downstream kernel
 * driver, so its exact purpose beyond the vendor comment ("vepu will hold
 * when encoding finish") is unverified. Treated as an opaque required
 * write sequence.
 */
#define RKVENC_REG_DVBM_HOLD		0x0074
#define RKVENC_DVBM_HOLD_VAL		0x23

/* enc_strt (0x0010): the actual "go" trigger is the 3-bit vepu_cmd field
 * at bits[10:8], NOT the whole register set to 1 — vepu_cmd=1 is mpp's
 * setup_vepu510_normal(); lkt_num (bits[7:0]) is the link-table task
 * count, irrelevant here since v1 doesn't use link/CCU mode.
 */
union rkvenc_reg_enc_strt {
	struct {
		u32 lkt_num:8;
		u32 vepu_cmd:3;
		u32 reserved:21;
	};
	u32 val;
};

#define RKVENC_VEPU_CMD_ENC		1

/* int_en / int_msk / int_clr / int_sta all share this bit layout. */
union rkvenc_reg_int_bits {
	struct {
		u32 enc_done:1;
		u32 lkt_node_done:1;
		u32 sclr_done:1;
		u32 vslc_done:1;
		u32 vbsf_oflw:1;
		u32 vbuf_lens:1;
		u32 enc_err:1;
		u32 vsrc_err:1;
		u32 wdg:1;
		u32 lkt_err_int:1;
		u32 lkt_err_stop:1;
		u32 lkt_force_stop:1;
		u32 jslc_done:1;
		u32 jbsf_oflw:1;
		u32 jbuf_lens:1;
		u32 dvbm_err:1;
		u32 reserved:16;
	};
	u32 val;
};

/* Hard-error bits worth aborting the frame over; enc_done/lkt_node_done/
 * sclr_done/vslc_done/vbuf_lens/jslc_done/jbsf_oflw/jbuf_lens are either
 * normal completion signals or JPEG-only (harmless no-ops for H.264).
 */
#define RKVENC_INT_ERROR_MASK \
	(BIT(4) /* vbsf_oflw */ | BIT(6) /* enc_err */ | BIT(7) /* vsrc_err */ | \
	 BIT(8) /* wdg */ | BIT(9) /* lkt_err_int */ | BIT(10) /* lkt_err_stop */ | \
	 BIT(11) /* lkt_force_stop */ | BIT(15) /* dvbm_err */)

union rkvenc_reg_enc_wdg {
	struct {
		u32 vs_load_thd:24;
		u32 reserved:8;
	};
	u32 val;
};

union rkvenc_reg_opt_strg {
	struct {
		u32 cke:1;
		u32 resetn_hw_en:1;
		u32 rfpr_err_e:1;
		u32 sram_ckg_en:1;
		u32 link_err_stop:1;
		u32 reserved:27;
	};
	u32 val;
};

union rkvenc_reg_dtrns_map {
	struct {
		u32 jpeg_bus_edin:4;
		u32 src_bus_edin:4;
		u32 meiw_bus_edin:4;
		u32 bsw_bus_edin:4;
		u32 reserved:8;
		u32 lktw_bus_edin:4;
		u32 rec_nfbc_bus_edin:4;
	};
	u32 val;
};

/* ---- FRAME class (0x0270-0x03f4), fields mirror Vepu510FrmCommon +
 * the H.264-specific trailer from H264eVepu510Frame in mpp.
 */
union rkvenc_reg_enc_pic {
	struct {
		u32 enc_stnd:2;			/* 0 = H.264 */
		u32 cur_frm_ref:1;
		u32 mei_stor:1;
		u32 bs_scp:1;
		u32 reserved0:3;
		u32 pic_qp:6;
		u32 num_pic_tot_cur_hevc:5;
		u32 log2_ctu_num_hevc:5;
		u32 reserved1:6;
		u32 slen_fifo:1;
		u32 rec_fbc_dis:1;
	};
	u32 val;
};

#define RKVENC_ENC_STND_H264		0

/* dual_core (0x304) is the CCU cross-core dual-encoder handshake register
 * (DCHS_REG_OFFSET in the downstream vendor kernel's mpp_rkvenc2.c) --
 * used for chaining a frame's encode across both VEPU510 cores. This
 * driver deliberately doesn't implement that (see the architecture
 * comment in rkvenc.c), but the register is still written explicitly every
 * frame to a defined value (dchs_txe=1, i.e. 0x14): the vendor kernel's
 * own rkvenc2_patch_dchs() writes exactly this on every task, even fully
 * standalone non-chained ones (confirmed against a real register-write
 * trace of the vendor stack), so a POR/leftover value is not what the
 * hardware expects.
 */
union rkvenc_reg_dual_core {
	struct {
		u32 dchs_txid:2;
		u32 dchs_rxid:2;
		u32 dchs_txe:1;
		u32 dchs_rxe:1;
		u32 reserved0:2;
		u32 dchs_dly:8;
		u32 dchs_ofst:10;
		u32 reserved1:6;
	};
	u32 val;
};

union rkvenc_reg_enc_id {
	struct {
		u32 frame_id:8;
		u32 frm_id_match:1;
		u32 reserved0:7;
		u32 ch_id:2;
		u32 vrsp_rtn_en:1;
		u32 vinf_req_en:1;
		u32 reserved1:12;
	};
	u32 val;
};

/* VEPU510-only quirk value, see rkvenc_vepu510_quirk() in rkvenc-h264.c. */
#define RKVENC_ENC_ID_QUIRK_VAL		(BIT(18) | BIT(16))

union rkvenc_reg_enc_rsl {
	struct {
		u32 pic_wd8_m1:11;
		u32 reserved0:5;
		u32 pic_hd8_m1:11;
		u32 reserved1:5;
	};
	u32 val;
};

union rkvenc_reg_src_fill {
	struct {
		u32 pic_wfill:6;
		u32 reserved0:10;
		u32 pic_hfill:6;
		u32 reserved1:10;
	};
	u32 val;
};

union rkvenc_reg_src_fmt {
	struct {
		u32 alpha_swap:1;
		u32 rbuv_swap:1;
		u32 src_cfmt:4;
		u32 src_rcne:1;
		u32 out_fmt:1;
		u32 src_range_trns_en:1;
		u32 src_range_trns_sel:1;
		u32 chroma_ds_mode:1;
		u32 reserved:21;
	};
	u32 val;
};

/* src_cfmt values (VepuFmt enum, vepu5xx_common.h) — only the one v1 uses. */
#define RKVENC_SRC_FMT_YUV420SP		6	/* NV12 */
/* out_fmt (bit7): real capture always has this set (src_fmt == 0x98, not
 * just src_cfmt's 0x18) for a real successful encode; mpp's own field name
 * suggests an output-plane format toggle but nothing in the userspace HAL
 * ever names its meaning beyond the bit position. Treated as a required
 * "must be 1" bit, like the DVBM-hold quirk.
 */

union rkvenc_reg_src_proc {
	struct {
		u32 cr_force_value:8;
		u32 cb_force_value:8;
		u32 chroma_force_en:1;
		u32 reserved0:9;
		u32 src_mirr:1;
		u32 src_rot:2;
		u32 tile4x4_en:1;
		u32 reserved1:2;
	};
	u32 val;
};

union rkvenc_reg_pic_ofst {
	struct {
		u32 pic_ofst_x:14;
		u32 reserved0:2;
		u32 pic_ofst_y:14;
		u32 reserved1:2;
	};
	u32 val;
};

union rkvenc_reg_src_strd0 {
	struct {
		u32 src_strd0:21;
		u32 reserved:11;
	};
	u32 val;
};

union rkvenc_reg_src_strd1 {
	struct {
		u32 src_strd1:16;
		u32 reserved:16;
	};
	u32 val;
};

union rkvenc_reg_rc_cfg {
	struct {
		u32 rc_en:1;
		u32 aq_en:1;
		u32 reserved:10;
		u32 rc_ctu_num:20;
	};
	u32 val;
};

union rkvenc_reg_rc_qp {
	struct {
		u32 reserved:16;
		u32 rc_qp_range:4;
		u32 rc_max_qp:6;
		u32 rc_min_qp:6;
	};
	u32 val;
};

union rkvenc_reg_rc_tgt {
	struct {
		u32 ctu_ebit:20;
		u32 reserved:12;
	};
	u32 val;
};

union rkvenc_reg_sli_splt {
	struct {
		u32 sli_splt:1;
		u32 sli_splt_mode:1;
		u32 sli_splt_cpst:1;
		u32 reserved0:12;
		u32 sli_flsh:1;
		u32 sli_max_num_m1:15;
		u32 reserved1:1;
	};
	u32 val;
};

union rkvenc_reg_sli_byte {
	struct {
		u32 sli_splt_byte:20;
		u32 reserved:12;
	};
	u32 val;
};

union rkvenc_reg_sli_cnum {
	struct {
		u32 sli_splt_cnum_m1:20;
		u32 reserved:12;
	};
	u32 val;
};

/* H.264-specific trailer, 0x3a0-0x3f4.
 * rdo_cfg.chrm_spcl/ccwa_e/atr_mult_sel_e: mpp's setup_vepu510_rdo_pred()
 * (hal_h264e_vepu510.c) sets these three unconditionally to 1 on every
 * single frame -- not gated by profile/level/tune config like rect_size/
 * vlc_lmt/atf_e/atr_e are. Found missing by diffing against that function
 * directly; previously left at their reset-value 0 here since nothing in
 * this driver ever set them.
 */
union rkvenc_reg_rdo_cfg {
	struct {
		u32 rect_size:1;
		u32 reserved0:2;
		u32 vlc_lmt:1;
		u32 chrm_spcl:1;
		u32 reserved1:8;
		u32 ccwa_e:1;
		u32 reserved2:1;
		u32 atr_e:1;
		u32 reserved3:4;
		u32 scl_lst_sel:2;
		u32 reserved4:6;
		u32 atf_e:1;
		u32 atr_mult_sel_e:1;
		u32 reserved5:2;
	};
	u32 val;
};

union rkvenc_reg_synt_nal {
	struct {
		u32 nal_ref_idc:2;
		u32 nal_unit_type:5;
		u32 reserved:25;
	};
	u32 val;
};

union rkvenc_reg_synt_sps {
	struct {
		u32 max_fnum:4;		/* log2_max_frame_num_minus4 */
		u32 drct_8x8:1;		/* direct_8x8_inference_flag */
		u32 mpoc_lm4:4;		/* log2_max_pic_order_cnt_lsb_minus4 */
		u32 poc_type:2;		/* pic_order_cnt_type */
		u32 reserved:21;
	};
	u32 val;
};

union rkvenc_reg_synt_pps {
	struct {
		u32 etpy_mode:1;	/* entropy_coding_mode_flag: 0=CAVLC 1=CABAC */
		u32 trns_8x8:1;		/* transform_8x8_mode_flag */
		u32 csip_flag:1;	/* constrained_intra_pred_flag */
		u32 num_ref0_idx:2;
		u32 num_ref1_idx:2;
		u32 pic_init_qp:6;
		u32 cb_ofst:5;
		u32 cr_ofst:5;
		u32 reserved:1;
		u32 dbf_cp_flg:1;	/* deblocking_filter_control_present_flag */
		u32 reserved1:7;
	};
	u32 val;
};

union rkvenc_reg_synt_sli0 {
	struct {
		u32 sli_type:2;
		u32 pps_id:8;
		u32 drct_smvp:1;
		u32 num_ref_ovrd:1;
		u32 cbc_init_idc:2;
		u32 reserved:2;
		u32 frm_num:16;
	};
	u32 val;
};

#define RKVENC_SLI_TYPE_P		0
#define RKVENC_SLI_TYPE_I		2

union rkvenc_reg_synt_sli1 {
	struct {
		u32 idr_pid:16;
		u32 poc_lsb:16;
	};
	u32 val;
};

union rkvenc_reg_synt_sli2 {
	struct {
		u32 rodr_pic_idx:2;
		u32 ref_list0_rodr:1;
		u32 sli_beta_ofst:4;
		u32 sli_alph_ofst:4;
		u32 dis_dblk_idc:2;
		u32 reserved:3;
		u32 rodr_pic_num:16;
	};
	u32 val;
};

/* Absolute byte offsets of each FRAME-class register this driver touches. */
#define RKVENC_REG_ADR_SRC0		0x280
#define RKVENC_REG_ADR_SRC1		0x284
#define RKVENC_REG_ADR_SRC2		0x288
#define RKVENC_REG_RFPW_H_ADDR		0x28c
#define RKVENC_REG_RFPW_B_ADDR		0x290
#define RKVENC_REG_RFPR_H_ADDR		0x294
#define RKVENC_REG_RFPR_B_ADDR		0x298
#define RKVENC_REG_MEIW_ADDR		0x2ac	/* motion-detection-info write ptr */
#define RKVENC_REG_DSPW_ADDR		0x2a4
#define RKVENC_REG_DSPR_ADDR		0x2a8
#define RKVENC_REG_BSBT_ADDR		0x2b0
#define RKVENC_REG_BSBB_ADDR		0x2b4
#define RKVENC_REG_ADR_BSBS		0x2b8
#define RKVENC_REG_BSBR_ADDR		0x2bc
/* rfpt/rfpb ("top"/"bottom" field recon addresses, interlaced-only):
 * real capture always carries the literal sentinel 0xffffffff in
 * rfpt_h/rfpt_b (not an fd-embedded pointer -- an fd of 0x3ff/1023 is
 * never valid on a real system, so this reads as an explicit
 * "disabled/no interlaced fields" pattern), with rfpb_h/adr_rfpb_b at 0.
 */
#define RKVENC_REG_RFPT_H_ADDR		0x2d0
#define RKVENC_REG_RFPB_H_ADDR		0x2d4
#define RKVENC_REG_RFPT_B_ADDR		0x2d8
#define RKVENC_REG_ADR_RFPB_B		0x2dc
#define RKVENC_RFPT_DISABLED		0xffffffff
#define RKVENC_REG_ADR_SMEAR_RD		0x2e0
#define RKVENC_REG_ADR_SMEAR_WR		0x2e4
/* Last DMA pointer in the FRAME class (ROI read address). The whole
 * 0x270-0x2e8 block is zeroed at the top of rkvenc_h264_run() to clear any
 * stale/garbage write pointers (lpfw/lpfr/ebuft/ebufb at 0x2c0-0x2cc, this,
 * and 0x29c/0x2a0) that the vendor always writes but this driver otherwise
 * skips -- see the big comment there.
 */
#define RKVENC_REG_ADR_ROIR		0x2e8
#define RKVENC_REG_ENC_PIC		0x300
#define RKVENC_REG_DUAL_CORE		0x304
#define RKVENC_REG_ENC_ID		0x308
#define RKVENC_REG_ENC_RSL		0x310
#define RKVENC_REG_SRC_FILL		0x314
#define RKVENC_REG_SRC_FMT		0x318
#define RKVENC_REG_SRC_PROC		0x32c
#define RKVENC_REG_PIC_OFST		0x330
#define RKVENC_REG_SRC_STRD0		0x334
#define RKVENC_REG_SRC_STRD1		0x338
#define RKVENC_REG_RC_CFG		0x350
#define RKVENC_REG_RC_QP		0x354
#define RKVENC_REG_RC_TGT		0x358
#define RKVENC_REG_SLI_SPLT		0x360
#define RKVENC_REG_SLI_BYTE		0x364
#define RKVENC_REG_SLI_CNUM		0x368

/* Motion-estimation search-range/config/cache registers: written
 * *unconditionally* by mpp's setup_vepu510_me() on every single frame
 * (no conditional guard — not gated behind P-frame/inter-mode use),
 * same "required regardless of what you'd assume from the feature
 * name" pattern as the roi_qthd and thumb/smear-buffer registers.
 * Values below are mpp's own hardcoded constants, not per-content
 * tuning — a zeroed cime_dist_thre/srgn_max_num here is a plausible
 * cause of a genuine ME-engine search/loop stall.
 */
#define RKVENC_REG_ME_RNGE		0x370
#define RKVENC_REG_ME_CFG		0x374
#define RKVENC_REG_ME_CACH		0x378

union rkvenc_reg_me_rnge {
	struct {
		u32 cime_srch_dwnh:4;
		u32 cime_srch_uph:4;
		u32 cime_srch_rgtw:4;
		u32 cime_srch_lftw:4;
		u32 dlt_frm_num:16;
	};
	u32 val;
};

union rkvenc_reg_me_cfg {
	struct {
		u32 srgn_max_num:7;
		u32 cime_dist_thre:13;
		u32 rme_srch_h:2;
		u32 rme_srch_v:2;
		u32 rme_dis:3;
		u32 reserved:1;
		u32 fme_dis:3;
		u32 reserved1:1;
	};
	u32 val;
};

union rkvenc_reg_me_cach {
	struct {
		u32 cime_zero_thre:13;
		u32 reserved:15;
		u32 fme_prefsu_en:2;
		u32 colmv_stor_hevc:1;
		u32 colmv_load_hevc:1;
	};
	u32 val;
};

#define RKVENC_REG_RDO_CFG		0x3a0
#define RKVENC_REG_SYNT_NAL		0x3b0
#define RKVENC_REG_SYNT_SPS		0x3b4
#define RKVENC_REG_SYNT_PPS		0x3b8
#define RKVENC_REG_SYNT_SLI0		0x3bc
#define RKVENC_REG_SYNT_SLI1		0x3c0
#define RKVENC_REG_SYNT_SLI2		0x3c4

/* ---- RC_ROI class (0x1000-0x110c), a.k.a. Vepu510RcRoi in mpp.
 *
 * rc_adj0/rc_adj1/rc_dthd_0_8 (0x1000-0x1028) are the adaptive-quant
 * bit-count-deviation threshold ladder: mpp's setup_vepu510_rc_base()
 * computes these from the frame's bit budget in its non-FIXQP path, but
 * skips them entirely (early `return`) in true MPP_ENC_RC_MODE_FIXQP mode
 * — this driver's original design assumed that meant "safe to leave at
 * 0", matching disabled RC/AQ. Board bring-up proved that assumption
 * wrong: leaving rc_dthd_0_8 at 0 (read by hardware as "always past
 * threshold" rather than mpp's real "never trigger" 0x7FFFFFFF sentinel
 * for the outer entries) reproduces the exact hardware-watchdog stall on
 * its own. This driver now always runs with rc_cfg.rc_en=1/aq_en=1 and a
 * real (if approximate — see rkvenc-h264.c) threshold ladder, but clamps
 * rc_qp.rc_min_qp==rc_max_qp and rc_qp_range=0 so the AQ engine has no
 * room to actually move the QP away from the requested value — genuine
 * fixed-QP *output*, just not fixed-QP *register configuration*.
 *
 * roi_qthd0-3: mpp's setup_vepu510_rc_base() writes these
 * *unconditionally*, before even checking RC mode — every ROI area's
 * [qpmin, qpmax] clamp range is set to the frame's actual
 * [quality_min, quality_max] regardless.
 */
#define RKVENC_REG_RC_ADJ0		0x1000
#define RKVENC_REG_RC_ADJ1		0x1004
#define RKVENC_REG_RC_DTHD(n)		(0x1008 + 4 * (n))	/* n = 0..8 */

union rkvenc_reg_rc_adj0 {
	struct {
		s32 qp_adj0:5;
		s32 qp_adj1:5;
		s32 qp_adj2:5;
		s32 qp_adj3:5;
		s32 qp_adj4:5;
		u32 reserved:7;
	};
	u32 val;
};

union rkvenc_reg_rc_adj1 {
	struct {
		s32 qp_adj5:5;
		s32 qp_adj6:5;
		s32 qp_adj7:5;
		s32 qp_adj8:5;
		u32 reserved:12;
	};
	u32 val;
};

#define RKVENC_RC_DTHD_SENTINEL	0x7fffffff

#define RKVENC_REG_ROI_QTHD0		0x1030
#define RKVENC_REG_ROI_QTHD1		0x1034
#define RKVENC_REG_ROI_QTHD2		0x1038
#define RKVENC_REG_ROI_QTHD3		0x103c

union rkvenc_reg_roi_qthd0 {
	struct {
		u32 qpmin_area0:6;
		u32 qpmax_area0:6;
		u32 qpmin_area1:6;
		u32 qpmax_area1:6;
		u32 qpmin_area2:6;
		u32 reserved:2;
	};
	u32 val;
};

union rkvenc_reg_roi_qthd1 {
	struct {
		u32 qpmax_area2:6;
		u32 qpmin_area3:6;
		u32 qpmax_area3:6;
		u32 qpmin_area4:6;
		u32 qpmax_area4:6;
		u32 reserved:2;
	};
	u32 val;
};

union rkvenc_reg_roi_qthd2 {
	struct {
		u32 qpmin_area5:6;
		u32 qpmax_area5:6;
		u32 qpmin_area6:6;
		u32 qpmax_area6:6;
		u32 qpmin_area7:6;
		u32 reserved:2;
	};
	u32 val;
};

union rkvenc_reg_roi_qthd3 {
	struct {
		u32 qpmax_area7:6;
		u32 reserved:24;
		u32 qpmap_mode:2;
	};
	u32 val;
};

/* RC_ROI tail (0x1044-0x107c): AQ activity threshold/step LUT, MAD-based
 * statistics thresholds, and a chroma KLUT offset -- all real fields in
 * mpp's Vepu510RcRoi struct (vepu510_common.h) that this driver used to
 * leave entirely unwritten. Found by diffing this driver's register writes
 * against mpp's actual setup_vepu510_aq()/setup_vepu510_me()/
 * setup_vepu510_rdo_pred() (hal_h264e_vepu510.c), not just the single
 * I-frame wtrace capture used to fix the original whole-class stall (see
 * the RC_ROI banner comment above) -- that capture never exercised these
 * particular offsets either, since a from-scratch driver only writes what
 * it's told to, and nothing had told it to write these yet.
 *
 * mpp writes aq_tthd/aq_stp/madi_st_thd/madp_st_thd0/madp_st_thd1
 * unconditionally on every frame regardless of slice type (setup_vepu510_me()
 * has no I/P branch for these), and klut_ofst.chrm_klut_ofst is 6 for I
 * slices and 6 or 9 for P slices depending on an IPC scene-tuning mode this
 * driver doesn't expose (so 6 is correct for both here). These sit in
 * exactly the register class board bring-up already proved needs
 * non-degenerate content for the hardware to complete at all (see the RC_ROI
 * banner comment) -- left at POR/leftover 0 instead of mpp's real nonzero
 * table, this is a plausible source of a real internal stall that happens to
 * only bite once real per-block activity/MAD statistics get computed and
 * compared against these thresholds, i.e. during genuine inter-mode
 * (P-frame) analysis rather than intra.
 */
#define RKVENC_REG_AQ_TTHD		0x1044	/* 4 words, 16 packed u8 thresholds */
#define RKVENC_REG_AQ_STP0		0x1054
#define RKVENC_REG_AQ_STP1		0x1058
#define RKVENC_REG_AQ_STP2		0x105c
#define RKVENC_REG_MADI_ST_THD		0x1064
#define RKVENC_REG_MADP_ST_THD0	0x1068
#define RKVENC_REG_MADP_ST_THD1	0x106c
#define RKVENC_REG_KLUT_OFST		0x107c

union rkvenc_reg_aq_stp0 {
	struct {
		s32 aq_stp_s0:5;
		s32 aq_stp_0t1:5;
		s32 aq_stp_1t2:5;
		s32 aq_stp_2t3:5;
		s32 aq_stp_3t4:5;
		s32 aq_stp_4t5:5;
		u32 reserved:2;
	};
	u32 val;
};

union rkvenc_reg_aq_stp1 {
	struct {
		s32 aq_stp_5t6:5;
		s32 aq_stp_6t7:5;
		s32 aq_stp_7t8:5;
		s32 aq_stp_8t9:5;
		s32 aq_stp_9t10:5;
		s32 aq_stp_10t11:5;
		u32 reserved:2;
	};
	u32 val;
};

union rkvenc_reg_aq_stp2 {
	struct {
		s32 aq_stp_11t12:5;
		s32 aq_stp_12t13:5;
		s32 aq_stp_13t14:5;
		s32 aq_stp_14t15:5;
		s32 aq_stp_b15:5;
		u32 reserved:7;
	};
	u32 val;
};

union rkvenc_reg_madi_st_thd {
	struct {
		u32 madi_th0:8;
		u32 madi_th1:8;
		u32 madi_th2:8;
		u32 reserved:8;
	};
	u32 val;
};

union rkvenc_reg_madp_st_thd0 {
	struct {
		u32 madp_th0:12;
		u32 reserved:4;
		u32 madp_th1:12;
		u32 reserved1:4;
	};
	u32 val;
};

union rkvenc_reg_madp_st_thd1 {
	struct {
		u32 madp_th2:12;
		u32 reserved:20;
	};
	u32 val;
};

union rkvenc_reg_klut_ofst {
	struct {
		u32 chrm_klut_ofst:4;
		u32 reserved:4;
		u32 inter_chrm_dist_multi:6;
		u32 reserved1:18;
	};
	u32 val;
};

/* ---- PARAM class (0x1700-0x19cc), a.k.a. Vepu510Param in mpp.
 * RDO lambda/cost tables plus a handful of small quant-tuning tables ahead
 * of them. mpp treats the bulk of this as a *default-tuning profile*
 * constant table (`vepu51x_h264e_lambda_default_60`/`_cvr_60`, selected by
 * a tuning "lambda_idx" knob this driver doesn't expose, not by
 * resolution/QP/content) — so a fixed constant table here, captured from
 * a real successful encode via wtrace and cross-checked against the
 * table's monotonically-increasing-lambda shape, is a deliberate v1
 * choice, not a placeholder. See RKVENC_PAR_CLASS_WORDS in rkvenc-h264.c.
 *
 * CORRECTION (2026-07-21, from a real multi-frame wtrace capture): the
 * "static blob, same for every frame" characterization above is WRONG for
 * four specific words inside it -- mpp's setup_vepu510_anti_ringing()
 * (hal_h264e_vepu510.c) computes real, different ATR (anti-ringing) weight
 * values depending on I-slice vs P-slice, not just the tune/scene-mode
 * config a single I-frame capture can see. A 3-frame real capture (frame 0
 * I, frames 1-2 P) showed these four offsets change between the I-frame and
 * BOTH P-frames, while staying identical between the two P-frames --
 * conclusive evidence of an I/P split, not per-content noise. This driver's
 * static `rkvenc_par_class` blob was captured from an I-frame-only session,
 * so every P-frame was silently getting the I-slice ATR weights. See
 * RKVENC_REG_ATR_THD1/WGT16/WGT8/WGT4 below and the patch in rkvenc-h264.c.
 */
#define RKVENC_REG_PAR_OFFSET		0x1700
#define RKVENC_REG_PAR_SIZE		0x02d0	/* 0x19cc - 0x1700 + 4 */

#define RKVENC_REG_ATR_THD1		0x1744	/* thd2 (I/P-dependent) + thdqp (constant) */
#define RKVENC_REG_ATR_WGT16		0x1750
#define RKVENC_REG_ATR_WGT8		0x1754
#define RKVENC_REG_ATR_WGT4		0x1758

union rkvenc_reg_atr_thd1 {
	struct {
		u32 thd2:8;
		u32 reserved0:8;
		u32 thdqp:6;
		u32 reserved1:10;
	};
	u32 val;
};

union rkvenc_reg_atr_wgt {
	struct {
		u32 wgt0:8;
		u32 wgt1:8;
		u32 wgt2:8;
		u32 reserved:8;
	};
	u32 val;
};

/* ---- SQI class (0x2000-0x212c), "subjective Adjust" quant tuning.
 * Same default-tuning-profile characterization as PARAM above — a real
 * successful encode only ever wrote the first 0x2000-0x20b8 (the rest of
 * the declared class range was never touched even by a genuinely working
 * encode, so this driver doesn't write it either).
 *
 * CORRECTION (2026-07-21): same caveat as PARAM above applies to one word,
 * smear_opt_cfg (0x2014, mpp's setup_vepu510_anti_smear()). Its
 * `stated_mode` field is 1 if (this slice is I) OR (the *previous* encoded
 * frame was I), else 2 -- a real multi-frame capture confirmed this exactly
 * (frame 0 I: 1; frame 1 P, previous=I: 1; frame 2 P, previous=P: 2), pure
 * slice-history state this driver can track trivially. Its
 * `rdo_smear_dlt_qp` field is a genuinely different, harder case: mpp
 * derives it from real per-block "smear" activity statistics read back from
 * the *previous* frame's actual hardware encode (`ctx->last_frame_fb`,
 * populated from DEBUG-class counters this driver has never implemented
 * readback for) -- not just slice type. The capture showed -1 (I) then -3
 * (both P frames), which this driver approximates as a static per-slice-
 * type constant (real content-adaptive tracking is a follow-up, see
 * this driver approximates it, since implementing full smear-statistics readback is out of
 * scope here and this field's effect is RDO cost tuning, not known to be
 * hang-relevant like the RC_ROI class was.
 */
#define RKVENC_REG_SQI_OFFSET		0x2000
#define RKVENC_REG_SQI_SIZE		0x00bc	/* 0x20b8 - 0x2000 + 4, not the full class */

#define RKVENC_REG_SMEAR_OPT_CFG	0x2014

union rkvenc_reg_smear_opt_cfg {
	struct {
		u32 rdo_smear_lvl16_multi:8;
		s32 rdo_smear_dlt_qp:4;
		u32 reserved:1;
		u32 stated_mode:2;
		u32 rdo_smear_en:1;
		u32 reserved1:16;
	};
	u32 val;
};

/* ---- SCL class (0x2200-0x2584), scaling lists.
 * All-zero for any encode that doesn't use a custom H.264 scaling list
 * (this driver never does) — written explicitly anyway, every frame,
 * rather than relying on POR/leftover-state being zero.
 */
#define RKVENC_REG_SCL_OFFSET		0x2200
#define RKVENC_REG_SCL_SIZE		0x0388	/* 0x2584 - 0x2200 + 4, full class */

/* ---- STATUS class (0x4000-0x424c), read-only ---- */
#define RKVENC_REG_ST_BS_LENGTH	0x4000	/* bs_lgth_l32: this-frame byte count */
#define RKVENC_REG_ST_BSB	0x402c	/* current bitstream write pointer */
#define RKVENC_REG_ST_SLICE_NUM	0x4034	/* slice-fifo occupancy, & 0x3f */
#define RKVENC_REG_ST_SLICE_LEN	0x4038	/* slice-length fifo read port */

#define RKVENC_SLICE_LEN_LAST		BIT(31)

/* ---- DEBUG class ---- */
#define RKVENC_REG_DBG_CLR		0x5300
#define RKVENC_DBG_CLR_VAL		0x2

#endif /* RKVENC_REGS_H_ */
