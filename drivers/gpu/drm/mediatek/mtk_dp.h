/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019-2024 MediaTek Inc.
 */

#ifndef _MTK_DP_H_
#define _MTK_DP_H_

#include "tlc_dp_hdcp.h"
#include <drm/drm_bridge.h>
#include <sound/hdmi-codec.h>
#include <video/videomode.h>

enum {
	MTK_DP_CAL_GLB_BIAS_TRIM = 0,
	MTK_DP_CAL_CLKTX_IMPSE,
	MTK_DP_CAL_LN_TX_IMPSEL_PMOS_0,
	MTK_DP_CAL_LN_TX_IMPSEL_PMOS_1,
	MTK_DP_CAL_LN_TX_IMPSEL_PMOS_2,
	MTK_DP_CAL_LN_TX_IMPSEL_PMOS_3,
	MTK_DP_CAL_LN_TX_IMPSEL_NMOS_0,
	MTK_DP_CAL_LN_TX_IMPSEL_NMOS_1,
	MTK_DP_CAL_LN_TX_IMPSEL_NMOS_2,
	MTK_DP_CAL_LN_TX_IMPSEL_NMOS_3,
	MTK_DP_CAL_MAX,
};

struct mtk_dp_audio_cfg {
	bool detect_monitor;
	int sad_count;
	int sample_rate;
	int word_length_bits;
	int channels;
};

struct mtk_dp_info {
	enum dp_pixelformat format;
	struct videomode vm;
	struct mtk_dp_audio_cfg audio_cur_cfg;
};

struct mtk_dp_train_info {
	bool sink_ssc;
	bool cable_plugged_in;
	/* link_rate is in multiple of 0.27Gbps */
	int link_rate;
	int lane_count;
	unsigned int channel_eq_pattern;
};

struct mtk_dp {
	bool enabled;
	bool need_debounce;
	int irq;
	u8 max_lanes;
	u8 max_linkrate;
	u8 rx_cap[DP_RECEIVER_CAP_SIZE];
	u32 cal_data[MTK_DP_CAL_MAX];
	u32 irq_thread_handle;
	/* irq_thread_lock is used to protect irq_thread_handle */
	spinlock_t irq_thread_lock;

	struct device *dev;
	struct drm_bridge bridge;
	struct drm_bridge *next_bridge;
	struct drm_connector *conn;
	struct drm_device *drm_dev;
	struct drm_dp_aux aux;

	const struct mtk_dp_data *data;
	struct mtk_dp_info info;
	struct mtk_dp_train_info train_info;
	struct mtk_hdcp_info hdcp_info;
	struct work_struct hdcp_work;
	struct delayed_work prop_work;
	struct workqueue_struct *hdcp_workqueue;

	struct platform_device *phy_dev;
	struct phy *phy;
	struct regmap *regs;
	struct timer_list debounce_timer;

	/* For audio */
	bool audio_enable;
	hdmi_codec_plugged_cb plugged_cb;
	struct platform_device *audio_pdev;

	struct device *codec_dev;
	/* protect the plugged_cb as it's used in both bridge ops and audio */
	struct mutex update_plugged_status_lock;
};

u32 mtk_dp_get_system_time(void);
u32 mtk_dp_get_time_diff(u32 pre_time);
u32 mtk_dp_read(struct mtk_dp *mtk_dp, u32 offset);
int mtk_dp_update_bits(struct mtk_dp *mtk_dp, u32 offset, u32 val, u32 mask);
void mtk_dp_authentication(struct mtk_hdcp_info *hdcp_info);

#endif /* _MTK_DP_H_ */
