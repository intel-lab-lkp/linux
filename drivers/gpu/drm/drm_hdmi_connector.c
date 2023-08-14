// SPDX-License-Identifier: GPL-2.0+

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_edid.h>
#include <drm/drm_managed.h>
#include <drm/drm_mode.h>
#include <drm/drm_print.h>
#include <drm/display/drm_hdmi_helper.h>

#include <linux/export.h>

/**
 * __drm_atomic_helper_hdmi_connector_reset() - Initializes all @drm_hdmi_connector_state resources
 * @hdmi_connector: the connector this state refers to
 * @new_hdmi_state: the HDMI connector state to initialize
 *
 * Initializes all relevant resources from a @drm_hdmi_connector_state
 * without actually allocating it. This is useful for drivers that
 * subclass @drm_hdmi_connector_state.
 */
void __drm_atomic_helper_hdmi_connector_reset(struct drm_hdmi_connector *hdmi_connector,
					      struct drm_hdmi_connector_state *new_hdmi_state)
{
	struct drm_connector *connector = &hdmi_connector->base;

	__drm_atomic_helper_connector_reset(connector, &new_hdmi_state->base);

	new_hdmi_state->base.max_bpc = 8;
	new_hdmi_state->base.max_requested_bpc = 8;
	new_hdmi_state->output_bpc = 8;
	new_hdmi_state->output_format = HDMI_COLORSPACE_RGB;
	new_hdmi_state->broadcast_rgb = DRM_HDMI_BROADCAST_RGB_AUTO;
}
EXPORT_SYMBOL(__drm_atomic_helper_hdmi_connector_reset);

/**
 * drm_atomic_helper_hdmi_connector_reset() - Create a @drm_hdmi_connector_state object
 * @connector: the parent connector
 *
 * This helper is meant to be the default &drm_connector_funcs.reset
 * implementation for @drm_hdmi_connector that don't subclass
 * @drm_hdmi_connector_state.
 */
void drm_atomic_helper_hdmi_connector_reset(struct drm_connector *connector)
{
	struct drm_hdmi_connector *hdmi_connector =
		connector_to_hdmi_connector(connector);
	struct drm_connector_state *old_state = connector->state;
	struct drm_hdmi_connector_state *old_hdmi_state =
		connector_state_to_hdmi_connector_state(old_state);
	struct drm_hdmi_connector_state *new_hdmi_state;

	if (old_state)
		__drm_atomic_helper_connector_destroy_state(old_state);

	kfree(old_hdmi_state);

	new_hdmi_state = kzalloc(sizeof(*new_hdmi_state), GFP_KERNEL);
	if (!new_hdmi_state)
		return;

	__drm_atomic_helper_hdmi_connector_reset(hdmi_connector, new_hdmi_state);
}
EXPORT_SYMBOL(drm_atomic_helper_hdmi_connector_reset);

/**
 * __drm_atomic_helper_hdmi_connector_duplicate_state() - Copies all @drm_hdmi_connector_state resources
 * @hdmi_connector: the connector this state refers to
 * @new_hdmi_state: the HDMI connector state to copy to
 *
 * Copies all relevant resources from a @drm_hdmi_connector_state to a
 * new one without actually allocating it. This is useful for drivers
 * that subclass @drm_hdmi_connector_state.
 */
void
__drm_atomic_helper_hdmi_connector_duplicate_state(struct drm_hdmi_connector *hdmi_connector,
						   struct drm_hdmi_connector_state *new_hdmi_state)
{
	struct drm_connector *connector = &hdmi_connector->base;
	struct drm_connector_state *old_state = connector->state;
	struct drm_hdmi_connector_state *old_hdmi_state =
		connector_state_to_hdmi_connector_state(old_state);

	new_hdmi_state->tmds_char_rate = old_hdmi_state->tmds_char_rate;
	new_hdmi_state->output_bpc = old_hdmi_state->output_bpc;
	new_hdmi_state->output_format = old_hdmi_state->output_format;
	new_hdmi_state->broadcast_rgb = old_hdmi_state->broadcast_rgb;
	__drm_atomic_helper_connector_duplicate_state(connector, &new_hdmi_state->base);
}
EXPORT_SYMBOL(__drm_atomic_helper_hdmi_connector_duplicate_state);

/**
 * drm_atomic_helper_hdmi_connector_duplicate_state() - Duplicate a @drm_hdmi_connector_state object
 * @connector: the parent connector this state refers to
 *
 * This helper is meant to be the default
 * &drm_connector_funcs.atomic_duplicate_state implementation for
 * @drm_hdmi_connector that don't subclass @drm_hdmi_connector_state.
 */
struct drm_connector_state *
drm_atomic_helper_hdmi_connector_duplicate_state(struct drm_connector *connector)
{
	struct drm_hdmi_connector *hdmi_connector =
		connector_to_hdmi_connector(connector);
	struct drm_hdmi_connector_state *new_hdmi_state;

	new_hdmi_state = kzalloc(sizeof(*new_hdmi_state), GFP_KERNEL);
	if (!new_hdmi_state)
		return NULL;

	__drm_atomic_helper_hdmi_connector_duplicate_state(hdmi_connector, new_hdmi_state);

	return &new_hdmi_state->base;
}
EXPORT_SYMBOL(drm_atomic_helper_hdmi_connector_duplicate_state);

/**
 * __drm_atomic_helper_hdmi_connector_destroy_state() - Releases all @drm_hdmi_connector_state resources
 * @hdmi_state: the HDMI connector state to release
 *
 * Release all resources stored in @drm_hdmi_connector_state without
 * actually freeing it. This is useful for drivers that subclass
 * @drm_hdmi_connector_state.
 */
void __drm_atomic_helper_hdmi_connector_destroy_state(struct drm_hdmi_connector_state *hdmi_state)
{
	__drm_atomic_helper_connector_destroy_state(&hdmi_state->base);
}
EXPORT_SYMBOL(__drm_atomic_helper_hdmi_connector_destroy_state);

/**
 * drm_atomic_helper_hdmi_connector_destroy_state() - Destroy a @drm_hdmi_connector_state object
 * @connector: the parent connector this state refers to
 * @state: the parent connector state to destroy
 *
 * Destroys an HDMI connector state previously created by
 * &drm_atomic_helper_hdmi_connector_reset() or
 * &drm_atomic_helper_hdmi_connector_duplicate_state().
 *
 * This helper is meant to be the default
 * &drm_connector_funcs.atomic_destroy_state implementation for
 * @drm_hdmi_connector that don't subclass @drm_hdmi_connector_state.
 */
void drm_atomic_helper_hdmi_connector_destroy_state(struct drm_connector *connector,
						    struct drm_connector_state *state)
{
	struct drm_hdmi_connector_state *hdmi_state =
		connector_state_to_hdmi_connector_state(state);

	__drm_atomic_helper_hdmi_connector_destroy_state(hdmi_state);
	kfree(hdmi_state);
}
EXPORT_SYMBOL(drm_atomic_helper_hdmi_connector_destroy_state);

/**
 * drm_atomic_helper_hdmi_connector_get_property() - Reads out HDMI connector properties
 * @connector: the parent connector this state refers to
 * @state: the parent connector state to destroy
 * @property: Property instance being queried
 * @val: Raw value of the property to read into
 *
 * Read out a @drm_connector_state property value.
 *
 * This helper is meant to be the default
 * &drm_connector_funcs.atomic_get_property implementation for
 * @drm_hdmi_connector.
 */
int drm_atomic_helper_hdmi_connector_get_property(struct drm_connector *connector,
						  const struct drm_connector_state *state,
						  struct drm_property *property,
						  uint64_t *val)
{
	const struct drm_hdmi_connector *hdmi_connector =
		connector_to_hdmi_connector(connector);
	const struct drm_hdmi_connector_state *hdmi_state =
		connector_state_to_hdmi_connector_state(state);
	struct drm_device *drm = connector->dev;

	if (property == hdmi_connector->broadcast_rgb_property) {
		*val = hdmi_state->broadcast_rgb;
	} else {
		drm_dbg(drm, "Unknown property [PROP:%d:%s]\n",
			property->base.id, property->name);
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(drm_atomic_helper_hdmi_connector_get_property);

/**
 * drm_atomic_helper_hdmi_connector_set_property() - Decodes HDMI connector properties
 * @connector: the parent connector this state refers to
 * @state: the parent connector state to destroy
 * @property: Property instance being queried
 * @val: Raw value of the property to decode
 *
 * Decodes a property into an @drm_connector_state.
 *
 * This helper is meant to be the default
 * &drm_connector_funcs.atomic_set_property implementation for
 * @drm_hdmi_connector.
 */
int drm_atomic_helper_hdmi_connector_set_property(struct drm_connector *connector,
						  struct drm_connector_state *state,
						  struct drm_property *property,
						  uint64_t val)
{
	const struct drm_hdmi_connector *hdmi_connector =
		connector_to_hdmi_connector(connector);
	struct drm_hdmi_connector_state *hdmi_state =
		connector_state_to_hdmi_connector_state(state);
	struct drm_device *drm = connector->dev;

	if (property == hdmi_connector->broadcast_rgb_property) {
		hdmi_state->broadcast_rgb = val;
	} else {
		drm_dbg(drm, "Unknown property [PROP:%d:%s]\n",
			property->base.id, property->name);
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(drm_atomic_helper_hdmi_connector_set_property);

static const char * const output_format_str[] = {
	[HDMI_COLORSPACE_RGB]		= "RGB",
	[HDMI_COLORSPACE_YUV420]	= "YUV 4:2:0",
	[HDMI_COLORSPACE_YUV422]	= "YUV 4:2:2",
	[HDMI_COLORSPACE_YUV444]	= "YUV 4:4:4",
};

/*
 * drm_hdmi_connector_get_output_format_name() - Return a string for HDMI connector output format
 * @fmt: Output format to compute name of
 *
 * Returns: the name of the output format, or NULL if the type is not
 * valid.
 */
const char *
drm_hdmi_connector_get_output_format_name(enum hdmi_colorspace fmt)
{
	if (fmt >= ARRAY_SIZE(output_format_str))
		return NULL;

	return output_format_str[fmt];
}
EXPORT_SYMBOL(drm_hdmi_connector_get_output_format_name);

static const struct drm_display_mode *
connector_state_get_adjusted_mode(const struct drm_connector_state *state)
{
	struct drm_crtc *crtc = state->crtc;
	struct drm_crtc_state *crtc_state = crtc->state;

	return &crtc_state->adjusted_mode;
}

static bool
sink_supports_format_bpc(const struct drm_hdmi_connector *hdmi_connector,
			 const struct drm_display_info *info,
			 const struct drm_display_mode *mode,
			 unsigned int format, unsigned int bpc)
{
	const struct drm_connector *connector = &hdmi_connector->base;
	struct drm_device *dev = connector->dev;
	u8 vic = drm_match_cea_mode(mode);

	if (vic == 1 && bpc != 8) {
		drm_dbg(dev, "VIC1 requires a bpc of 8, got %u\n", bpc);
		return false;
	}

	if (!info->is_hdmi &&
	    (format != HDMI_COLORSPACE_RGB || bpc != 8)) {
		drm_dbg(dev, "DVI Monitors require an RGB output at 8 bpc\n");
		return false;
	}

	switch (format) {
	case HDMI_COLORSPACE_RGB:
		drm_dbg(dev, "RGB Format, checking the constraints.\n");

		if (!(info->color_formats & DRM_COLOR_FORMAT_RGB444))
			return false;

		if (bpc == 10 && !(info->edid_hdmi_rgb444_dc_modes & DRM_EDID_HDMI_DC_30)) {
			drm_dbg(dev, "10 BPC but sink doesn't support Deep Color 30.\n");
			return false;
		}

		if (bpc == 12 && !(info->edid_hdmi_rgb444_dc_modes & DRM_EDID_HDMI_DC_36)) {
			drm_dbg(dev, "12 BPC but sink doesn't support Deep Color 36.\n");
			return false;
		}

		drm_dbg(dev, "RGB format supported in that configuration.\n");

		return true;

	case HDMI_COLORSPACE_YUV422:
		drm_dbg(dev, "YUV422 format, checking the constraints.\n");

		if (!(info->color_formats & DRM_COLOR_FORMAT_YCBCR422)) {
			drm_dbg(dev, "Sink doesn't support YUV422.\n");
			return false;
		}

		if (bpc != 12) {
			drm_dbg(dev, "YUV422 only supports 12 bpc.\n");
			return false;
		}

		drm_dbg(dev, "YUV422 format supported in that configuration.\n");

		return true;

	case HDMI_COLORSPACE_YUV444:
		drm_dbg(dev, "YUV444 format, checking the constraints.\n");

		if (!(info->color_formats & DRM_COLOR_FORMAT_YCBCR444)) {
			drm_dbg(dev, "Sink doesn't support YUV444.\n");
			return false;
		}

		if (bpc == 10 && !(info->edid_hdmi_ycbcr444_dc_modes & DRM_EDID_HDMI_DC_30)) {
			drm_dbg(dev, "10 BPC but sink doesn't support Deep Color 30.\n");
			return false;
		}

		if (bpc == 12 && !(info->edid_hdmi_ycbcr444_dc_modes & DRM_EDID_HDMI_DC_36)) {
			drm_dbg(dev, "12 BPC but sink doesn't support Deep Color 36.\n");
			return false;
		}

		drm_dbg(dev, "YUV444 format supported in that configuration.\n");

		return true;
	}

	return false;
}

static enum drm_mode_status
drm_hdmi_connector_clock_valid(const struct drm_hdmi_connector *hdmi_connector,
			       const struct drm_display_mode *mode,
			       unsigned long long clock)
{
	const struct drm_hdmi_connector_funcs *funcs = hdmi_connector->funcs;
	const struct drm_connector *connector = &hdmi_connector->base;
	const struct drm_display_info *info = &connector->display_info;

	if (info->max_tmds_clock && clock > info->max_tmds_clock * 1000)
		return MODE_CLOCK_HIGH;

	if (funcs && funcs->tmds_char_rate_valid)
		return funcs->tmds_char_rate_valid(hdmi_connector, mode, clock);

	return MODE_OK;
}

/**
 * drm_hdmi_connector_compute_mode_clock() - Computes the TMDS Character Rate
 * @mode: Display mode to compute the clock for
 * @bpc: Bits per character
 * @fmt: Output Pixel Format used
 *
 * Returns the TMDS Character Rate for a given mode, bpc count and output format.
 *
 * RETURNS:
 * The TMDS Character Rate, in Hertz
 */
unsigned long long
drm_hdmi_connector_compute_mode_clock(const struct drm_display_mode *mode,
				      unsigned int bpc,
				      enum hdmi_colorspace fmt)
{
	unsigned long long clock = mode->clock * 1000ULL;

	if (mode->flags & DRM_MODE_FLAG_DBLCLK)
		clock = clock * 2;

	if (fmt == HDMI_COLORSPACE_YUV422)
		bpc = 8;

	clock = clock * bpc;
	do_div(clock, 8);

	return clock;
}
EXPORT_SYMBOL(drm_hdmi_connector_compute_mode_clock);

static int
drm_hdmi_connector_compute_clock(const struct drm_hdmi_connector *hdmi_connector,
				 struct drm_hdmi_connector_state *hdmi_state,
				 const struct drm_display_mode *mode,
				 unsigned int bpc,
				 enum hdmi_colorspace fmt)
{
	unsigned long long clock;

	clock = drm_hdmi_connector_compute_mode_clock(mode, bpc, fmt);
	if (!drm_hdmi_connector_clock_valid(hdmi_connector, mode, clock) != MODE_OK)
		return -EINVAL;

	hdmi_state->tmds_char_rate = clock;

	return 0;
}

static bool
drm_hdmi_connector_try_format_bpc(const struct drm_hdmi_connector *hdmi_connector,
				  struct drm_hdmi_connector_state *hdmi_state,
				  const struct drm_display_mode *mode,
				  unsigned int bpc, enum hdmi_colorspace fmt)
{
	const struct drm_connector *connector = &hdmi_connector->base;
	const struct drm_display_info *info = &connector->display_info;
	struct drm_device *dev = connector->dev;
	int ret;

	drm_dbg(dev, "Trying output format %s\n",
		drm_hdmi_connector_get_output_format_name(fmt));

	if (!sink_supports_format_bpc(hdmi_connector, info, mode, fmt, bpc))
		return false;

	ret = drm_hdmi_connector_compute_clock(hdmi_connector, hdmi_state,
					       mode, bpc, fmt);
	if (ret)
		return false;

	return true;
}

static int
drm_hdmi_connector_compute_format(const struct drm_hdmi_connector *hdmi_connector,
				  struct drm_hdmi_connector_state *hdmi_state,
				  const struct drm_display_mode *mode,
				  unsigned int bpc)
{
	const struct drm_connector *connector = &hdmi_connector->base;
	struct drm_device *dev = connector->dev;

	if (drm_hdmi_connector_try_format_bpc(hdmi_connector, hdmi_state,
					      mode, bpc, HDMI_COLORSPACE_RGB)) {
		hdmi_state->output_format = HDMI_COLORSPACE_RGB;
		return 0;
	}

	if (drm_hdmi_connector_try_format_bpc(hdmi_connector, hdmi_state,
					      mode, bpc, HDMI_COLORSPACE_YUV422)) {
		hdmi_state->output_format = HDMI_COLORSPACE_YUV422;
		return 0;
	}

	drm_dbg(dev, "Failed. No Format Supported for that bpc count.\n");

	return -EINVAL;
}

static int
drm_hdmi_connector_compute_config(const struct drm_hdmi_connector *hdmi_connector,
				  struct drm_hdmi_connector_state *hdmi_state,
				  const struct drm_display_mode *mode)
{
	const struct drm_connector *connector = &hdmi_connector->base;
	struct drm_connector_state *conn_state = &hdmi_state->base;
	struct drm_device *dev = connector->dev;
	unsigned int max_bpc = clamp_t(unsigned int,
				       conn_state->max_bpc,
				       8, hdmi_connector->max_bpc);
	unsigned int bpc;
	int ret;

	for (bpc = max_bpc; bpc >= 8; bpc -= 2) {
		drm_dbg(dev, "Trying with a %d bpc output\n", bpc);

		ret = drm_hdmi_connector_compute_format(hdmi_connector,
							hdmi_state,
							mode, bpc);
		if (ret)
			continue;

		hdmi_state->output_bpc = bpc;

		drm_dbg(dev,
			"Mode %ux%u @ %uHz: Found configuration: bpc: %u, fmt: %s, clock: %llu\n",
			mode->hdisplay, mode->vdisplay, drm_mode_vrefresh(mode),
			hdmi_state->output_bpc,
			drm_hdmi_connector_get_output_format_name(hdmi_state->output_format),
			hdmi_state->tmds_char_rate);

		return 0;
	}

	return -EINVAL;
}

static int
drm_hdmi_connector_generate_avi_infoframe(const struct drm_hdmi_connector *hdmi_connector,
					  struct drm_hdmi_connector_state *hdmi_state)
{
	const struct drm_connector *connector = &hdmi_connector->base;
	const struct drm_connector_state *state = &hdmi_state->base;
	const struct drm_display_mode *mode =
		connector_state_get_adjusted_mode(state);
	struct hdmi_avi_infoframe *frame = &hdmi_state->infoframes.avi;
	bool is_lim_range =
		drm_atomic_helper_hdmi_connector_is_full_range(hdmi_connector,
							       hdmi_state);
	enum hdmi_quantization_range rgb_quant_range =
		is_lim_range ? HDMI_QUANTIZATION_RANGE_FULL : HDMI_QUANTIZATION_RANGE_LIMITED;
	int ret;

	ret = drm_hdmi_avi_infoframe_from_display_mode(frame, connector, mode);
	if (ret)
		return ret;

	frame->colorspace = hdmi_state->output_format;

	drm_hdmi_avi_infoframe_quant_range(frame, connector, mode, rgb_quant_range);
	drm_hdmi_avi_infoframe_colorimetry(frame, state);
	drm_hdmi_avi_infoframe_bars(frame, state);

	return 0;
}

static int
drm_hdmi_connector_generate_spd_infoframe(const struct drm_hdmi_connector *hdmi_connector,
					  struct drm_hdmi_connector_state *hdmi_state)
{
	struct hdmi_spd_infoframe *frame = &hdmi_state->infoframes.spd;
	int ret;

	ret = hdmi_spd_infoframe_init(frame,
				      hdmi_connector->vendor,
				      hdmi_connector->product);
	if (ret)
		return ret;

	frame->sdi = HDMI_SPD_SDI_PC;

	return 0;
}

static int
drm_hdmi_connector_generate_hdr_infoframe(const struct drm_hdmi_connector *hdmi_connector,
					  struct drm_hdmi_connector_state *hdmi_state)
{
	const struct drm_connector_state *state = &hdmi_state->base;
	struct hdmi_drm_infoframe *frame = &hdmi_state->infoframes.drm;
	int ret;

	if (hdmi_connector->max_bpc < 10)
		return 0;

	if (!state->hdr_output_metadata)
		return 0;

	ret = drm_hdmi_infoframe_set_hdr_metadata(frame, state);
	if (ret)
		return ret;

	return 0;
}

static int
drm_hdmi_connector_generate_vendor_infoframe(const struct drm_hdmi_connector *hdmi_connector,
					     struct drm_hdmi_connector_state *hdmi_state)
{
	const struct drm_connector *connector = &hdmi_connector->base;
	const struct drm_connector_state *state = &hdmi_state->base;
	const struct drm_display_mode *mode =
		connector_state_get_adjusted_mode(state);
	struct hdmi_vendor_infoframe *frame = &hdmi_state->infoframes.vendor;
	int ret;

	ret = drm_hdmi_vendor_infoframe_from_display_mode(frame, connector, mode);
	if (ret == -EINVAL)
		return 0;
	else
		return ret;

	return 0;
}

static int
drm_hdmi_connector_generate_infoframes(const struct drm_hdmi_connector *hdmi_connector,
				       struct drm_hdmi_connector_state *hdmi_state)
{
	const struct drm_connector *connector = &hdmi_connector->base;
	const struct drm_display_info *info = &connector->display_info;
	int ret;

	if (!info->is_hdmi)
		return 0;

	if (!info->has_hdmi_infoframe)
		return 0;

	ret = drm_hdmi_connector_generate_avi_infoframe(hdmi_connector, hdmi_state);
	if (ret)
		return ret;

	ret = drm_hdmi_connector_generate_spd_infoframe(hdmi_connector, hdmi_state);
	if (ret)
		return ret;

	/*
	 * Audio Infoframes will be generated by ALSA.
	 */

	ret = drm_hdmi_connector_generate_hdr_infoframe(hdmi_connector, hdmi_state);
	if (ret)
		return ret;

	ret = drm_hdmi_connector_generate_vendor_infoframe(hdmi_connector, hdmi_state);
	if (ret)
		return ret;

	return 0;
}

/**
 * drm_atomic_helper_hdmi_connector_atomic_check() - Helper to check HDMI connector atomic state
 * @connector: the parent connector this state refers to
 * @state: the parent connector state to check
 *
 * Provides a default connector state check handler for HDMI connectors.
 * Checks that a desired connector update is valid, and updates various
 * fields of derived state.
 *
 * Drivers that subclass @drm_hdmi_connector_state may still wish to
 * call this function to avoid duplication of error checking code.
 *
 * RETURNS:
 * Zero on success, or an errno code otherwise.
 */
int drm_atomic_helper_hdmi_connector_atomic_check(struct drm_connector *connector,
						  struct drm_atomic_state *state)
{
	struct drm_hdmi_connector *hdmi_connector =
		connector_to_hdmi_connector(connector);
	struct drm_connector_state *old_state =
		drm_atomic_get_old_connector_state(state, connector);
	struct drm_hdmi_connector_state *old_hdmi_state =
		connector_state_to_hdmi_connector_state(old_state);
	struct drm_connector_state *new_state =
		drm_atomic_get_new_connector_state(state, connector);
	struct drm_hdmi_connector_state *new_hdmi_state =
		connector_state_to_hdmi_connector_state(new_state);
	const struct drm_display_mode *mode =
		connector_state_get_adjusted_mode(new_state);
	int ret;

	ret = drm_hdmi_connector_compute_config(hdmi_connector, new_hdmi_state,
						mode);
	if (ret)
		return ret;

	ret = drm_hdmi_connector_generate_infoframes(hdmi_connector, new_hdmi_state);
	if (ret)
		return ret;

	if (old_hdmi_state->broadcast_rgb != new_hdmi_state->broadcast_rgb ||
	    old_hdmi_state->output_format != new_hdmi_state->output_format ||
	    old_hdmi_state->output_bpc != new_hdmi_state->output_bpc) {
		struct drm_crtc *crtc = new_state->crtc;
		struct drm_crtc_state *crtc_state;

		crtc_state = drm_atomic_get_crtc_state(state, crtc);
		if (IS_ERR(crtc_state))
			return PTR_ERR(crtc_state);

		crtc_state->mode_changed = true;
	}

	return 0;
}
EXPORT_SYMBOL(drm_atomic_helper_hdmi_connector_atomic_check);

#define HDMI_MAX_INFOFRAME_SIZE		29

static int write_infoframe(struct drm_hdmi_connector *hdmi_connector,
			   union hdmi_infoframe *frame)
{
	const struct drm_hdmi_connector_funcs *funcs = hdmi_connector->funcs;
	u8 buffer[HDMI_MAX_INFOFRAME_SIZE];
	int len;

	if (!funcs || !funcs->write_infoframe)
		return -ENOSYS;

	len = hdmi_infoframe_pack(frame, buffer, sizeof(buffer));
	if (len < 0)
		return len;

	return funcs->write_infoframe(hdmi_connector, frame->any.type, buffer, len);
}

static int update_infoframe(struct drm_hdmi_connector *hdmi_connector,
			    union hdmi_infoframe *frame)
{
	int ret;

	ret = write_infoframe(hdmi_connector, frame);
	if (ret)
		return ret;

	switch (frame->any.type) {
	case HDMI_INFOFRAME_TYPE_AVI:
		memcpy(&hdmi_connector->infoframes.avi, &frame->avi,
		       sizeof(hdmi_connector->infoframes.avi));
		break;
	case HDMI_INFOFRAME_TYPE_DRM:
		memcpy(&hdmi_connector->infoframes.drm, &frame->drm,
		       sizeof(hdmi_connector->infoframes.drm));
		break;
	case HDMI_INFOFRAME_TYPE_SPD:
		memcpy(&hdmi_connector->infoframes.spd, &frame->spd,
		       sizeof(hdmi_connector->infoframes.spd));
		break;
	case HDMI_INFOFRAME_TYPE_VENDOR:
		memcpy(&hdmi_connector->infoframes.vendor, &frame->vendor,
		       sizeof(hdmi_connector->infoframes.vendor));
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

#define UPDATE_INFOFRAME(c, i)					\
	update_infoframe(c, (union hdmi_infoframe *)&(c)->infoframes.i)

/**
 * drm_atomic_helper_hdmi_connector_update_infoframes - Update the Infoframes
 * @hdmi_connector: A pointer to the HDMI connector
 * @hdmi_state: The HDMI connector state to generate the infoframe from
 *
 * This function is meant for HDMI connector drivers to write their
 * infoframes. It will typically be used in a
 * @drm_connector_helper_funcs.atomic_enable implementation.
 *
 * Returns:
 * Zero on success, error code on failure.
 */
int drm_atomic_helper_hdmi_connector_update_infoframes(struct drm_hdmi_connector *hdmi_connector,
						       struct drm_hdmi_connector_state *hdmi_state)
{
	struct drm_connector *connector = &hdmi_connector->base;
	struct drm_display_info *info = &connector->display_info;
	int ret;

	if (!info->is_hdmi)
		return 0;

	if (!info->has_hdmi_infoframe)
		return 0;

	mutex_lock(&hdmi_connector->infoframes.lock);

	ret = UPDATE_INFOFRAME(hdmi_connector, avi);
	if (ret)
		goto out;

	ret = UPDATE_INFOFRAME(hdmi_connector, audio);
	if (ret)
		goto out;

	ret = UPDATE_INFOFRAME(hdmi_connector, drm);
	if (ret)
		goto out;

	ret = UPDATE_INFOFRAME(hdmi_connector, spd);
	if (ret)
		goto out;

	ret = UPDATE_INFOFRAME(hdmi_connector, vendor);
	if (ret)
		goto out;

out:
	mutex_unlock(&hdmi_connector->infoframes.lock);
	return ret;
}
EXPORT_SYMBOL(drm_atomic_helper_hdmi_connector_update_infoframes);

#undef UPDATE_INFOFRAME

/**
 * drm_atomic_helper_hdmi_connector_update_audio_infoframe - Update the Audio Infoframe
 * @hdmi_connector: A pointer to the HDMI connector
 * @frame: A pointer to the audio infoframe to write
 *
 * This function is meant for HDMI connector drivers to update their
 * audio infoframe. It will typically be used in one of the ALSA hooks
 * (most likely prepare).
 *
 * Returns:
 * Zero on success, error code on failure.
 */
int
drm_atomic_helper_hdmi_connector_update_audio_infoframe(struct drm_hdmi_connector *hdmi_connector,
							struct hdmi_audio_infoframe *frame)
{
	struct drm_connector *connector = &hdmi_connector->base;
	struct drm_display_info *info = &connector->display_info;
	int ret;

	if (!info->is_hdmi)
		return 0;

	if (!info->has_hdmi_infoframe)
		return 0;

	mutex_lock(&hdmi_connector->infoframes.lock);

	ret = update_infoframe(hdmi_connector, (union hdmi_infoframe *)frame);

	mutex_unlock(&hdmi_connector->infoframes.lock);

	return ret;
}
EXPORT_SYMBOL(drm_atomic_helper_hdmi_connector_update_audio_infoframe);

/**
 * drm_atomic_helper_hdmi_connector_is_full_range() - Checks whether a state uses Full-Range RGB
 * @hdmi_connector: the HDMI connector this state refers to
 * @hdmi_state: the HDMI connector state to check
 *
 * RETURNS:
 * True if @hdmi_state requires a Full range RGB output, False otherwise
 */
bool
drm_atomic_helper_hdmi_connector_is_full_range(const struct drm_hdmi_connector *hdmi_connector,
					       const struct drm_hdmi_connector_state *hdmi_state)
{
	const struct drm_connector *connector = &hdmi_connector->base;
	const struct drm_connector_state *conn_state = &hdmi_state->base;
	const struct drm_display_mode *mode =
		connector_state_get_adjusted_mode(conn_state);
	const struct drm_display_info *display = &connector->display_info;

	if (hdmi_state->broadcast_rgb == DRM_HDMI_BROADCAST_RGB_FULL)
		return true;

	if (hdmi_state->broadcast_rgb == DRM_HDMI_BROADCAST_RGB_LIMITED)
		return false;

	if (!display->is_hdmi)
		return true;

	return drm_default_rgb_quant_range(mode);
}
EXPORT_SYMBOL(drm_atomic_helper_hdmi_connector_is_full_range);

static const struct drm_prop_enum_list broadcast_rgb_names[] = {
	{ DRM_HDMI_BROADCAST_RGB_AUTO, "Automatic" },
	{ DRM_HDMI_BROADCAST_RGB_FULL, "Full" },
	{ DRM_HDMI_BROADCAST_RGB_LIMITED, "Limited 16:235" },
};

/*
 * drm_hdmi_connector_get_broadcast_rgb_name - Return a string for HDMI connector RGB broadcast selection
 * @broadcast_rgb: Broadcast RGB selection to compute name of
 *
 * Returns: the name of the Broadcast RGB selection, or NULL if the type
 * is not valid.
 */
const char *
drm_hdmi_connector_get_broadcast_rgb_name(enum drm_hdmi_broadcast_rgb broadcast_rgb)
{
	if (broadcast_rgb > DRM_HDMI_BROADCAST_RGB_LIMITED)
		return NULL;

	return broadcast_rgb_names[broadcast_rgb].name;
}
EXPORT_SYMBOL(drm_hdmi_connector_get_broadcast_rgb_name);

/**
 * drm_atomic_helper_hdmi_connector_print_state - Prints a @drm_hdmi_connector_state
 * @p: output printer
 * @state: Connector state to print
 *
 * Default implementation of @drm_connector_funcs.atomic_print_state for
 * a @drm_hdmi_connector_state.
 */
void drm_atomic_helper_hdmi_connector_print_state(struct drm_printer *p,
						  const struct drm_connector_state *state)
{
	const struct drm_hdmi_connector_state *hdmi_state =
		connector_state_to_hdmi_connector_state(state);

	drm_printf(p, "\tbroadcast_rgb=%s\n",
		   drm_hdmi_connector_get_broadcast_rgb_name(hdmi_state->broadcast_rgb));
	drm_printf(p, "\toutput_bpc=%u\n", hdmi_state->output_bpc);
	drm_printf(p, "\toutput_format=%s\n",
		   drm_hdmi_connector_get_output_format_name(hdmi_state->output_format));
	drm_printf(p, "\ttmds_char_rate=%llu\n", hdmi_state->tmds_char_rate);
}
EXPORT_SYMBOL(drm_atomic_helper_hdmi_connector_print_state);

struct debugfs_wrapper {
	struct drm_hdmi_connector *hdmi_connector;
	union hdmi_infoframe *frame;
};

static ssize_t
infoframe_read(struct file *filp, char __user *ubuf, size_t count, loff_t *ppos)
{
	const struct debugfs_wrapper *wrapper = filp->private_data;
	struct drm_hdmi_connector *hdmi_connector = wrapper->hdmi_connector;
	union hdmi_infoframe *frame = wrapper->frame;
	u8 buf[HDMI_MAX_INFOFRAME_SIZE];
	ssize_t len;

	len = hdmi_infoframe_pack(frame, buf, sizeof(buf));
	if (len < 0)
		return len;

	mutex_lock(&hdmi_connector->infoframes.lock);
	len = simple_read_from_buffer(ubuf, count, ppos, buf, len);
	mutex_unlock(&hdmi_connector->infoframes.lock);

	return len;
}

static const struct file_operations infoframe_fops = {
	.owner   = THIS_MODULE,
	.open    = simple_open,
	.read    = infoframe_read,
};

static int create_debugfs_infoframe_file(struct drm_hdmi_connector *hdmi_connector,
					 struct dentry *parent,
					 const char *filename,
					 union hdmi_infoframe *frame)
{
	struct drm_device *dev = hdmi_connector->base.dev;
	struct debugfs_wrapper *wrapper;
	struct dentry *file;

	wrapper = drmm_kzalloc(dev, sizeof(*wrapper), GFP_KERNEL);
	if (!wrapper)
		return -ENOMEM;

	wrapper->hdmi_connector = hdmi_connector;
	wrapper->frame = frame;

	file = debugfs_create_file(filename, 0400, parent, wrapper, &infoframe_fops);
	if (IS_ERR(file))
		return PTR_ERR(file);

	return 0;
}

#define CREATE_INFOFRAME_FILE(c, p, i)		\
	create_debugfs_infoframe_file(c, p, #i, (union hdmi_infoframe *)&(c)->infoframes.i)

static int create_debugfs_infoframe_files(struct drm_hdmi_connector *hdmi_connector,
					  struct dentry *parent)
{
	int ret;

	ret = CREATE_INFOFRAME_FILE(hdmi_connector, parent, audio);
	if (ret)
		return ret;

	ret = CREATE_INFOFRAME_FILE(hdmi_connector, parent, avi);
	if (ret)
		return ret;

	ret = CREATE_INFOFRAME_FILE(hdmi_connector, parent, drm);
	if (ret)
		return ret;

	ret = CREATE_INFOFRAME_FILE(hdmi_connector, parent, spd);
	if (ret)
		return ret;

	ret = CREATE_INFOFRAME_FILE(hdmi_connector, parent, vendor);
	if (ret)
		return ret;

	return 0;
}

#undef CREATE_INFOFRAME_FILE

static void remove_debugfs_dir(struct drm_device *dev, void *data)
{
	struct dentry *dir = data;

	debugfs_remove_recursive(dir);
}

/**
 * drm_helper_hdmi_connector_debugfs_init - DebugFS init for HDMI connectors
 * @connector: Parent Connector
 * @dentry: DebugFS root dentry
 *
 * Provides a default implementation for
 * @drm_connector_helper_funcs.debugfs_init that will create all the
 * files relevant for a @drm_hdmi_connector.
 */
void drm_helper_hdmi_connector_debugfs_init(struct drm_connector *connector,
					    struct dentry *root)
{
	struct drm_hdmi_connector *hdmi_connector =
		connector_to_hdmi_connector(connector);
	struct drm_device *dev = hdmi_connector->base.dev;
	struct dentry *dir;
	int ret;

	dir = debugfs_create_dir("infoframes", root);
	if (IS_ERR(dir))
		return;

	ret = drmm_add_action_or_reset(dev, remove_debugfs_dir, dir);
	if (ret)
		return;

	create_debugfs_infoframe_files(hdmi_connector, dir);
}
EXPORT_SYMBOL(drm_helper_hdmi_connector_debugfs_init);

/**
 * drmm_hdmi_connector_init - Init a preallocated HDMI connector
 * @dev: DRM device
 * @hdmi_connector: A pointer to the HDMI connector to init
 * @vendor: HDMI Controller Vendor name
 * @product: HDMI Controller Product name
 * @funcs: callbacks for this connector
 * @connector_type: user visible type of the connector
 * @ddc: optional pointer to the associated ddc adapter
 * @max_bpc: Maximum bits per char the HDMI connector supports
 *
 * Initialises a preallocated HDMI connector. Connectors can be
 * subclassed as part of driver connector objects.
 *
 * Cleanup is automatically handled with a call to
 * drm_connector_cleanup() in a DRM-managed action.
 *
 * The connector structure should be allocated with drmm_kzalloc().
 *
 * Returns:
 * Zero on success, error code on failure.
 */
int drmm_hdmi_connector_init(struct drm_device *dev,
			     struct drm_hdmi_connector *hdmi_connector,
			     const char *vendor, const char *product,
			     const struct drm_connector_funcs *funcs,
			     const struct drm_hdmi_connector_funcs *hdmi_funcs,
			     int connector_type,
			     struct i2c_adapter *ddc,
			     unsigned int max_bpc)
{
	struct drm_connector *connector = &hdmi_connector->base;
	struct drm_property *prop;
	int ret;

	if (connector_type != DRM_MODE_CONNECTOR_HDMIA ||
	    connector_type != DRM_MODE_CONNECTOR_HDMIB)
		return -EINVAL;

	ret = drmm_connector_init(dev, connector, funcs, connector_type, ddc);
	if (ret)
		return ret;

	strscpy(hdmi_connector->vendor, vendor, sizeof(hdmi_connector->vendor));
	strscpy(hdmi_connector->product, product, sizeof(hdmi_connector->product));

	ret = drmm_mutex_init(dev, &hdmi_connector->infoframes.lock);
	if (ret)
		return ret;

	prop = hdmi_connector->broadcast_rgb_property;
	if (!prop) {
		prop = drm_property_create_enum(dev, DRM_MODE_PROP_ENUM,
						"Broadcast RGB",
						broadcast_rgb_names,
						ARRAY_SIZE(broadcast_rgb_names));
		if (!prop)
			return -EINVAL;

		hdmi_connector->broadcast_rgb_property = prop;
	}

	drm_object_attach_property(&connector->base, prop,
				   DRM_HDMI_BROADCAST_RGB_AUTO);

	if (max_bpc) {
		if (!(max_bpc == 8 || max_bpc == 10 || max_bpc == 12))
			return -EINVAL;

		drm_connector_attach_hdr_output_metadata_property(connector);
		drm_connector_attach_max_bpc_property(connector, 8, max_bpc);
		hdmi_connector->max_bpc = max_bpc;
	}

	hdmi_connector->funcs = hdmi_funcs;

	return 0;
}
EXPORT_SYMBOL(drmm_hdmi_connector_init);
