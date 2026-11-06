// SPDX-License-Identifier: GPL-2.0
/*
 * VGA output of the FL2000: the analog DAC of the chip, on an encoder and a connector of its own
 * next to the HDMI ones. All of them share the one CRTC, so a compositor can use either output
 * alone, and a dual-output adapter can show the same frame on both ("clone mode").
 */

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>

#include "fl2000.h"
#include "fl2000_it66121.h"

/*
 * Highest pixel clock offered on VGA, kHz. The DAC has no documented limit; 1920x1080 at 60 Hz is
 * what the vendor advertises for it.
 */
#define FL2000_VGA_MAX_CLOCK 165000

/* The first probes of a connection retry the EDID, in case the monitor is still waking up */
#define FL2000_VGA_EDID_TRIES 3

/* Offered as the preferred mode when there is no EDID: any VGA monitor takes it */
#define FL2000_VGA_FALLBACK_WIDTH  1024
#define FL2000_VGA_FALLBACK_HEIGHT 768

/* Largest mode listed from the standard timings when there is no EDID */
#define FL2000_VGA_NOEDID_WIDTH	 1920
#define FL2000_VGA_NOEDID_HEIGHT 1200

static inline struct fl2000_vga *to_fl2000_vga(struct drm_connector *connector)
{
	return container_of(connector, struct fl2000_vga, connector);
}

static int fl2000_vga_read_block(void *context, u8 *buf, unsigned int block, size_t len)
{
	struct fl2000_vga *vga = context;

	return fl2000_ddc_read_block(vga->adapter, buf, block, len);
}

/*
 * The chip has one monitor sense input, and on the adapters with both outputs it follows whichever
 * monitor is plugged in. The HDMI transmitter has a hot plug line of its own, so a sensed monitor
 * with no HDMI sink behind it is a VGA one. With both connected the HDMI sink wins and the VGA
 * output reports nothing: it can still be forced on (video=VGA-1:e) to mirror the picture.
 */
static enum drm_connector_status fl2000_vga_sense(struct fl2000_vga *vga)
{
	int ret = fl2000_intr_monitor_connected(vga->intr);

	if (ret < 0)
		return connector_status_unknown;
	if (!ret)
		return connector_status_disconnected;

	if (vga->hdmi_bridge) {
		/*
		 * Not knowing whether there is an HDMI sink is not the same as there being none:
		 * two connected outputs on the one CRTC can leave a compositor with neither.
		 */
		ret = it66121_hpd_connected(vga->hdmi_bridge);
		if (ret < 0)
			return connector_status_unknown;
		if (ret)
			return connector_status_disconnected;
	}

	return connector_status_connected;
}

static int fl2000_vga_detect_ctx(struct drm_connector *connector,
				 struct drm_modeset_acquire_ctx *ctx, bool force)
{
	struct fl2000_vga *vga = to_fl2000_vga(connector);
	enum drm_connector_status status;
	int idx;

	if (!drm_dev_enter(connector->dev, &idx))
		return connector_status_disconnected;

	status = fl2000_vga_sense(vga);
	if (status == connector_status_unknown)
		/* An unreadable status says nothing: keep what userspace was told */
		status = connector->status;
	else if (status != connector_status_connected)
		/* Whatever comes next may be another monitor: its EDID must be read again */
		WRITE_ONCE(vga->edid_stale, true);

	drm_dev_exit(idx);

	return status;
}

/*
 * The EDID is read through the FL2000 I2C engine. It is kept for as long as the monitor stays, and
 * only a few probes try for it: a read that finds nothing is quick, but old monitors often do not
 * answer DDC at all, and every probe runs with the mode config locked.
 */
static int fl2000_vga_get_modes(struct drm_connector *connector)
{
	struct fl2000_vga *vga = to_fl2000_vga(connector);
	int count = 0, idx;

	if (!drm_dev_enter(connector->dev, &idx))
		return 0;

	if (READ_ONCE(vga->edid_stale)) {
		WRITE_ONCE(vga->edid_stale, false);
		drm_edid_free(vga->edid);
		vga->edid = NULL;
		vga->edid_tries = 0;
	}

	if (!vga->edid && vga->edid_tries < FL2000_VGA_EDID_TRIES) {
		vga->edid_tries++;
		vga->edid = drm_edid_read_custom(connector, fl2000_vga_read_block, vga);
		drm_dbg_kms(connector->dev, "VGA EDID read %s (try %u)\n",
			    vga->edid ? "ok" : "failed", vga->edid_tries);
	}

	drm_edid_connector_update(connector, vga->edid);
	if (vga->edid)
		count = drm_edid_connector_add_modes(connector);

	if (!count) {
		/*
		 * No EDID, or nothing usable in it: offer the standard timings and let 1024x768
		 * be the default. Another mode can be chosen from the desktop, or forced on the
		 * kernel command line (video=VGA-1:1366x768@60e), and an EDID file can replace
		 * the missing one (drm.edid_firmware=VGA-1:edid/monitor.bin).
		 */
		count = drm_add_modes_noedid(connector, FL2000_VGA_NOEDID_WIDTH,
					     FL2000_VGA_NOEDID_HEIGHT);
		drm_set_preferred_mode(connector, FL2000_VGA_FALLBACK_WIDTH,
				       FL2000_VGA_FALLBACK_HEIGHT);
	}

	drm_dev_exit(idx);

	return count;
}

static const struct drm_connector_helper_funcs fl2000_vga_connector_helper_funcs = {
	.get_modes = fl2000_vga_get_modes,
	.detect_ctx = fl2000_vga_detect_ctx,
};

static const struct drm_connector_funcs fl2000_vga_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.atomic_create_state = drm_atomic_helper_connector_create_state,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/*
 * Applies to the VGA output only, both when probing and in atomic commits. The PLL limits are
 * checked by the CRTC.
 */
static enum drm_mode_status fl2000_vga_encoder_mode_valid(struct drm_encoder *encoder,
							  const struct drm_display_mode *mode)
{
	if (mode->clock > FL2000_VGA_MAX_CLOCK)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static const struct drm_encoder_helper_funcs fl2000_vga_encoder_helper_funcs = {
	.mode_valid = fl2000_vga_encoder_mode_valid,
};

/**
 * fl2000_vga_sink_event() - the chip reports that a monitor appeared, went away or changed EDID
 * @vga: VGA output
 *
 * Called from the interrupt work. The EDID is read again by the next probe.
 */
void fl2000_vga_sink_event(struct fl2000_vga *vga)
{
	WRITE_ONCE(vga->edid_stale, true);
}

static void fl2000_vga_release(struct drm_device *drm, void *res)
{
	struct fl2000_vga *vga = res;

	drm_edid_free(vga->edid);
	drm_bridge_put(vga->hdmi_bridge);
}

/**
 * fl2000_vga_init() - add the VGA encoder and connector
 * @drm: DRM device
 * @vga: VGA output to set up, part of the driver's device structure
 * @crtc: the CRTC, which every output shares
 * @hdmi_encoder: the encoder of the HDMI output, with its bridge already attached
 * @adapter: the FL2000 I2C adapter, which carries the monitor's DDC
 * @intr: interrupt context, to sense the monitor
 *
 * The HDMI bridge must be attached already, and nothing may be probed before this returns: the
 * two encoders are marked as clones of each other, in both directions as the core requires.
 *
 * Return: 0 on success, a negative error code otherwise
 */
int fl2000_vga_init(struct drm_device *drm, struct fl2000_vga *vga, struct drm_crtc *crtc,
		    struct drm_encoder *hdmi_encoder, struct i2c_adapter *adapter,
		    struct fl2000_intr *intr)
{
	u32 clones;
	int ret;

	vga->adapter = adapter;
	vga->intr = intr;
	vga->hdmi_bridge = drm_bridge_chain_get_first_bridge(hdmi_encoder);

	ret = drmm_add_action_or_reset(drm, fl2000_vga_release, vga);
	if (ret)
		return ret;

	ret = drmm_encoder_init(drm, &vga->encoder, NULL, DRM_MODE_ENCODER_DAC, NULL);
	if (ret)
		return ret;
	vga->encoder.possible_crtcs = drm_crtc_mask(crtc);
	drm_encoder_helper_add(&vga->encoder, &fl2000_vga_encoder_helper_funcs);

	/*
	 * No ddc adapter: its transfers carry a single byte, so the EDID is read with
	 * fl2000_ddc_read_block() instead of the DDC helpers.
	 */
	ret = drmm_connector_init(drm, &vga->connector, &fl2000_vga_connector_funcs,
				  DRM_MODE_CONNECTOR_VGA, NULL);
	if (ret)
		return ret;
	drm_connector_helper_add(&vga->connector, &fl2000_vga_connector_helper_funcs);

	/*
	 * The chip raises an event when the monitor comes or goes (fl2000_interrupt.c), but polling
	 * recovers from one that was missed, and it costs a status read every few seconds.
	 */
	vga->connector.polled = DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;

	ret = drm_connector_attach_encoder(&vga->connector, &vga->encoder);
	if (ret)
		return ret;

	clones = drm_encoder_mask(hdmi_encoder) | drm_encoder_mask(&vga->encoder);
	hdmi_encoder->possible_clones = clones;
	vga->encoder.possible_clones = clones;

	return 0;
}
