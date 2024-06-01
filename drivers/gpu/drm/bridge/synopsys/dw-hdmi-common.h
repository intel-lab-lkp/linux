/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __DW_HDMI_COMMON_H__
#define __DW_HDMI_COMMON_H__

#include <linux/i2c.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/spinlock_types.h>

#include <drm/bridge/dw_hdmi.h>
#include <drm/drm_connector.h>
#include <drm/drm_bridge.h>
#include <drm/drm_modes.h>

#include <sound/hdmi-codec.h>

struct cec_notifier;
struct device;
struct drm_bridge_state;
struct drm_crtc_state;
struct drm_edid;
struct pinctrl;
struct pinctrl_state;
struct platform_device;
struct regmap;

#define DDC_CI_ADDR		0x37
#define DDC_SEGMENT_ADDR	0x30

#define HDMI_EDID_LEN		512

/* DW-HDMI Controller >= 0x200a are at least compliant with SCDC version 1 */
#define SCDC_MIN_SOURCE_VERSION	0x1

#define HDMI14_MAX_TMDSCLK	340000000

struct hdmi_vmode {
	bool mdataenablepolarity;

	unsigned int mpixelclock;
	unsigned int mpixelrepetitioninput;
	unsigned int mpixelrepetitionoutput;
	unsigned int mtmdsclock;
};

struct hdmi_data_info {
	unsigned int enc_in_bus_format;
	unsigned int enc_out_bus_format;
	unsigned int enc_in_encoding;
	unsigned int enc_out_encoding;
	unsigned int pix_repet_factor;
	unsigned int hdcp_enable;
	struct hdmi_vmode video_mode;
	bool rgb_limited_range;
};

struct dw_hdmi_i2c {
	struct i2c_adapter	adap;

	struct mutex		lock;	/* used to serialize data transfers */
	struct completion	cmp;
	u8			stat;

	u8			slave_reg;
	bool			is_regaddr;
	bool			is_segment;
};

struct dw_hdmi_phy_data {
	enum dw_hdmi_phy_type type;
	const char *name;
	unsigned int gen;
	bool has_svsret;
	int (*configure)(struct dw_hdmi *hdmi,
			 const struct dw_hdmi_plat_data *pdata,
			 unsigned long mpixelclock);
};

struct dw_hdmi {
	struct drm_connector connector;
	struct drm_bridge bridge;
	struct drm_bridge *next_bridge;

	unsigned int version;

	struct platform_device *audio;
	struct platform_device *cec;
	struct device *dev;
	struct dw_hdmi_i2c *i2c;

	struct hdmi_data_info hdmi_data;
	const struct dw_hdmi_plat_data *plat_data;

	int vic;

	u8 edid[HDMI_EDID_LEN];

	struct {
		const struct dw_hdmi_phy_ops *ops;
		const char *name;
		void *data;
		bool enabled;
	} phy;

	struct drm_display_mode previous_mode;

	struct i2c_adapter *ddc;
	void __iomem *regs;
	bool sink_is_hdmi;
	bool sink_has_audio;

	struct pinctrl *pinctrl;
	struct pinctrl_state *default_state;
	struct pinctrl_state *unwedge_state;

	struct mutex mutex;		/* for state below and previous_mode */
	enum drm_connector_force force;	/* mutex-protected force state */
	struct drm_connector *curr_conn;/* current connector (only valid when !disabled) */
	bool disabled;			/* DRM has disabled our bridge */
	bool bridge_is_on;		/* indicates the bridge is on */
	bool rxsense;			/* rxsense state */
	u8 phy_mask;			/* desired phy int mask settings */
	u8 mc_clkdis;			/* clock disable register */

	spinlock_t audio_lock;
	struct mutex audio_mutex;
	unsigned int sample_non_pcm;
	unsigned int sample_width;
	unsigned int sample_rate;
	unsigned int channels;
	unsigned int audio_cts;
	unsigned int audio_n;
	bool audio_enable;

	unsigned int reg_shift;
	struct regmap *regm;
	void (*enable_audio)(struct dw_hdmi *hdmi);
	void (*disable_audio)(struct dw_hdmi *hdmi);

	struct mutex cec_notifier_mutex;
	struct cec_notifier *cec_notifier;

	hdmi_codec_plugged_cb plugged_cb;
	struct device *codec_dev;
	enum drm_connector_status last_connector_result;
};

void dw_handle_plugged_change(struct dw_hdmi *hdmi, bool plugged);
struct i2c_adapter *dw_hdmi_i2c_adapter(struct dw_hdmi *hdmi,
					const struct i2c_algorithm *algo);
bool dw_hdmi_support_scdc(struct dw_hdmi *hdmi,
			  const struct drm_display_info *display);
void dw_hdmi_prep_avi_infoframe(struct hdmi_avi_infoframe *frame,
				struct dw_hdmi *hdmi,
				const struct drm_connector *connector,
				const struct drm_display_mode *mode);
struct hdmi_vmode *dw_hdmi_prep_vmode(struct dw_hdmi *hdmi,
				      const struct drm_display_mode *mode);

enum drm_connector_status
dw_hdmi_connector_detect(struct drm_connector *connector, bool force);

int dw_hdmi_bridge_atomic_check(struct drm_bridge *bridge,
				struct drm_bridge_state *bridge_state,
				struct drm_crtc_state *crtc_state,
				struct drm_connector_state *conn_state);
void dw_hdmi_bridge_detach(struct drm_bridge *bridge);
void dw_hdmi_bridge_mode_set(struct drm_bridge *bridge,
			     const struct drm_display_mode *orig_mode,
			     const struct drm_display_mode *mode);
enum drm_connector_status dw_hdmi_bridge_detect(struct drm_bridge *bridge);
const struct drm_edid *
dw_hdmi_bridge_edid_read(struct drm_bridge *bridge,
			 struct drm_connector *connector);
#endif /* __DW_HDMI_COMMON_H__ */
