// SPDX-License-Identifier: GPL-2.0

/*
 * Kunit test for drm_atomic_state_helper functions
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_connector.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_kunit_helpers.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>

#include <drm/drm_print.h>
#include "../drm_crtc_internal.h"

#include <kunit/test.h>

#include "drm_kunit_edid.h"

struct drm_atomic_helper_connector_hdmi_priv {
	struct drm_device drm;
	struct drm_plane *plane;
	struct drm_crtc *crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;

	const char *current_edid;
	size_t current_edid_len;
};

#define connector_to_priv(c) \
	container_of_const(c, struct drm_atomic_helper_connector_hdmi_priv, connector)

static struct drm_display_mode *find_preferred_mode(struct drm_connector *connector)
{
	struct drm_device *drm = connector->dev;
	struct drm_display_mode *mode, *preferred;

	mutex_lock(&drm->mode_config.mutex);
	preferred = list_first_entry(&connector->modes, struct drm_display_mode, head);
	list_for_each_entry(mode, &connector->modes, head)
		if (mode->type & DRM_MODE_TYPE_PREFERRED)
			preferred = mode;
	mutex_unlock(&drm->mode_config.mutex);

	return preferred;
}

static int light_up_connector(struct kunit *test,
			      struct drm_device *drm,
			      struct drm_crtc *crtc,
			      struct drm_connector *connector,
			      struct drm_display_mode *mode,
			      struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_atomic_state *state;
	struct drm_connector_state *conn_state;
	struct drm_crtc_state *crtc_state;
	int ret;

	state = drm_kunit_helper_atomic_state_alloc(test, drm, ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	conn_state = drm_atomic_get_connector_state(state, connector);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	ret = drm_atomic_set_crtc_for_connector(conn_state, crtc);
	KUNIT_EXPECT_EQ(test, ret, 0);

	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);

	ret = drm_atomic_set_mode_for_crtc(crtc_state, mode);
	KUNIT_EXPECT_EQ(test, ret, 0);

	crtc_state->enable = true;
	crtc_state->active = true;

	ret = drm_atomic_commit(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	return 0;
}

static int set_connector_edid(struct kunit *test, struct drm_connector *connector,
			      const char *edid, size_t edid_len)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv =
		connector_to_priv(connector);
	struct drm_device *drm = connector->dev;
	int ret;

	priv->current_edid = edid;
	priv->current_edid_len = edid_len;

	mutex_lock(&drm->mode_config.mutex);
	ret = connector->funcs->fill_modes(connector, 4096, 4096);
	mutex_unlock(&drm->mode_config.mutex);
	KUNIT_ASSERT_GT(test, ret, 0);

	return 0;
}

static int dummy_connector_get_modes(struct drm_connector *connector)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv =
		connector_to_priv(connector);
	const struct drm_edid *edid;
	unsigned int num_modes;

	edid = drm_edid_alloc(priv->current_edid, priv->current_edid_len);
	if (!edid)
		return -EINVAL;

	drm_edid_connector_update(connector, edid);
	num_modes = drm_edid_connector_add_modes(connector);

	drm_edid_free(edid);

	return num_modes;
}

static const struct drm_connector_helper_funcs dummy_connector_helper_funcs = {
	.atomic_check	= drm_atomic_helper_connector_hdmi_check,
	.get_modes	= dummy_connector_get_modes,
};

static void dummy_hdmi_connector_reset(struct drm_connector *connector)
{
	drm_atomic_helper_connector_reset(connector);
	__drm_atomic_helper_connector_hdmi_reset(connector, connector->state);
}

static const struct drm_connector_funcs dummy_connector_funcs = {
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.reset			= dummy_hdmi_connector_reset,
};

static
struct drm_atomic_helper_connector_hdmi_priv *
drm_atomic_helper_connector_hdmi_init(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_connector *conn;
	struct drm_encoder *enc;
	struct drm_device *drm;
	struct device *dev;
	int ret;

	dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	priv = drm_kunit_helper_alloc_drm_device(test, dev,
						 struct drm_atomic_helper_connector_hdmi_priv, drm,
						 DRIVER_MODESET | DRIVER_ATOMIC);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv);
	test->priv = priv;

	drm = &priv->drm;
	priv->plane = drm_kunit_helper_create_primary_plane(test, drm,
							    NULL,
							    NULL,
							    NULL, 0,
							    NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->plane);

	priv->crtc = drm_kunit_helper_create_crtc(test, drm,
						  priv->plane, NULL,
						  NULL,
						  NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv->crtc);

	enc = &priv->encoder;
	ret = drmm_encoder_init(drm, enc, NULL, DRM_MODE_ENCODER_TMDS, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	enc->possible_crtcs = drm_crtc_mask(priv->crtc);

	conn = &priv->connector;
	ret = drmm_connector_hdmi_init(drm, conn,
				       &dummy_connector_funcs,
				       DRM_MODE_CONNECTOR_HDMIA,
				       NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	drm_connector_helper_add(conn, &dummy_connector_helper_funcs);
	drm_connector_attach_encoder(conn, enc);

	drm_mode_config_reset(drm);

	ret = set_connector_edid(test, conn,
				 test_edid_hdmi_1080p_rgb_max_200mhz,
				 ARRAY_SIZE(test_edid_hdmi_1080p_rgb_max_200mhz));
	KUNIT_ASSERT_EQ(test, ret, 0);

	return priv;
}

/*
 * Test that if we change the RGB quantization property to a different
 * value, we trigger a mode change on the connector's CRTC, which will
 * in turn disable/enable the connector.
 */
static void drm_test_check_broadcast_rgb_crtc_mode_changed(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx *ctx;
	struct drm_connector_state *old_conn_state;
	struct drm_connector_state *new_conn_state;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_state *state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_atomic_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	ctx = drm_kunit_helper_acquire_ctx_alloc(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);

	conn = &priv->connector;
	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	drm = &priv->drm;
	crtc = priv->crtc;
	ret = light_up_connector(test, drm, crtc, conn, preferred, ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	new_conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_conn_state);

	old_conn_state = drm_atomic_get_old_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, old_conn_state);

	new_conn_state->hdmi.broadcast_rgb = DRM_HDMI_BROADCAST_RGB_FULL;

	KUNIT_ASSERT_NE(test,
			old_conn_state->hdmi.broadcast_rgb,
			new_conn_state->hdmi.broadcast_rgb);

	ret = drm_atomic_check_only(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	new_conn_state = drm_atomic_get_new_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_conn_state);
	KUNIT_EXPECT_EQ(test, new_conn_state->hdmi.broadcast_rgb, DRM_HDMI_BROADCAST_RGB_FULL);

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);
	KUNIT_EXPECT_TRUE(test, crtc_state->mode_changed);
}

/*
 * Test that if we set the RGB quantization property to the same value,
 * we don't trigger a mode change on the connector's CRTC and leave the
 * connector unaffected.
 */
static void drm_test_check_broadcast_rgb_crtc_mode_not_changed(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx *ctx;
	struct drm_connector_state *old_conn_state;
	struct drm_connector_state *new_conn_state;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_state *state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_atomic_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	ctx = drm_kunit_helper_acquire_ctx_alloc(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);

	conn = &priv->connector;
	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);

	drm = &priv->drm;
	crtc = priv->crtc;
	ret = light_up_connector(test, drm, crtc, conn, preferred, ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	new_conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_conn_state);

	old_conn_state = drm_atomic_get_old_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, old_conn_state);

	new_conn_state->hdmi.broadcast_rgb = old_conn_state->hdmi.broadcast_rgb;

	ret = drm_atomic_check_only(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	old_conn_state = drm_atomic_get_old_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, old_conn_state);

	new_conn_state = drm_atomic_get_new_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, new_conn_state);

	KUNIT_EXPECT_EQ(test,
			old_conn_state->hdmi.broadcast_rgb,
			new_conn_state->hdmi.broadcast_rgb);

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, crtc_state);
	KUNIT_EXPECT_FALSE(test, crtc_state->mode_changed);
}

/*
 * Test that for an HDMI connector, with an HDMI monitor, if the
 * Broadcast RGB property is set to auto with a mode that isn't the
 * VIC-1 mode, we will get a limited RGB Quantization Range.
 */
static void drm_test_check_broadcast_rgb_auto_cea_mode(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx *ctx;
	struct drm_connector_state *conn_state;
	struct drm_atomic_state *state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_atomic_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	conn = &priv->connector;
	KUNIT_ASSERT_TRUE(test, conn->display_info.is_hdmi);

	ctx = drm_kunit_helper_acquire_ctx_alloc(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_ASSERT_NE(test, drm_match_cea_mode(preferred), 1);

	drm = &priv->drm;
	crtc = priv->crtc;
	ret = light_up_connector(test, drm, crtc, conn, preferred, ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	KUNIT_ASSERT_EQ(test,
			conn_state->hdmi.broadcast_rgb,
			DRM_HDMI_BROADCAST_RGB_AUTO);

	ret = drm_atomic_check_only(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	KUNIT_EXPECT_FALSE(test, conn_state->hdmi.is_full_range);
}

/*
 * Test that for an HDMI connector, with an HDMI monitor, if the
 * Broadcast RGB property is set to auto with a VIC-1 mode, we will get
 * a full RGB Quantization Range.
 */
static void drm_test_check_broadcast_rgb_auto_cea_mode_vic_1(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx *ctx;
	struct drm_connector_state *conn_state;
	struct drm_atomic_state *state;
	struct drm_display_mode *mode;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_atomic_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	conn = &priv->connector;
	KUNIT_ASSERT_TRUE(test, conn->display_info.is_hdmi);

	ctx = drm_kunit_helper_acquire_ctx_alloc(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);

	mode = drm_display_mode_from_cea_vic(drm, 1);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	drm = &priv->drm;
	crtc = priv->crtc;
	ret = light_up_connector(test, drm, crtc, conn, mode, ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	KUNIT_ASSERT_EQ(test,
			conn_state->hdmi.broadcast_rgb,
			DRM_HDMI_BROADCAST_RGB_AUTO);

	ret = drm_atomic_check_only(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	KUNIT_EXPECT_TRUE(test, conn_state->hdmi.is_full_range);
}

/*
 * Test that for an HDMI connector, with an HDMI monitor, if the
 * Broadcast RGB property is set to full with a mode that isn't the
 * VIC-1 mode, we will get a full RGB Quantization Range.
 */
static void drm_test_check_broadcast_rgb_full_cea_mode(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx *ctx;
	struct drm_connector_state *conn_state;
	struct drm_atomic_state *state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_atomic_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	conn = &priv->connector;
	KUNIT_ASSERT_TRUE(test, conn->display_info.is_hdmi);

	ctx = drm_kunit_helper_acquire_ctx_alloc(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_ASSERT_NE(test, drm_match_cea_mode(preferred), 1);

	drm = &priv->drm;
	crtc = priv->crtc;
	ret = light_up_connector(test, drm, crtc, conn, preferred, ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	conn_state->hdmi.broadcast_rgb = DRM_HDMI_BROADCAST_RGB_FULL;

	ret = drm_atomic_check_only(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	KUNIT_ASSERT_EQ(test,
			conn_state->hdmi.broadcast_rgb,
			DRM_HDMI_BROADCAST_RGB_FULL);

	KUNIT_EXPECT_TRUE(test, conn_state->hdmi.is_full_range);
}

/*
 * Test that for an HDMI connector, with an HDMI monitor, if the
 * Broadcast RGB property is set to full with a VIC-1 mode, we will get
 * a full RGB Quantization Range.
 */
static void drm_test_check_broadcast_rgb_full_cea_mode_vic_1(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx *ctx;
	struct drm_connector_state *conn_state;
	struct drm_atomic_state *state;
	struct drm_display_mode *mode;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_atomic_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	conn = &priv->connector;
	KUNIT_ASSERT_TRUE(test, conn->display_info.is_hdmi);

	ctx = drm_kunit_helper_acquire_ctx_alloc(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);

	mode = drm_display_mode_from_cea_vic(drm, 1);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	drm = &priv->drm;
	crtc = priv->crtc;
	ret = light_up_connector(test, drm, crtc, conn, mode, ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	conn_state->hdmi.broadcast_rgb = DRM_HDMI_BROADCAST_RGB_FULL;

	ret = drm_atomic_check_only(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	KUNIT_ASSERT_EQ(test,
			conn_state->hdmi.broadcast_rgb,
			DRM_HDMI_BROADCAST_RGB_FULL);

	KUNIT_EXPECT_TRUE(test, conn_state->hdmi.is_full_range);
}

/*
 * Test that for an HDMI connector, with an HDMI monitor, if the
 * Broadcast RGB property is set to limited with a mode that isn't the
 * VIC-1 mode, we will get a limited RGB Quantization Range.
 */
static void drm_test_check_broadcast_rgb_limited_cea_mode(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx *ctx;
	struct drm_connector_state *conn_state;
	struct drm_atomic_state *state;
	struct drm_display_mode *preferred;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_atomic_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	conn = &priv->connector;
	KUNIT_ASSERT_TRUE(test, conn->display_info.is_hdmi);

	ctx = drm_kunit_helper_acquire_ctx_alloc(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);

	preferred = find_preferred_mode(conn);
	KUNIT_ASSERT_NOT_NULL(test, preferred);
	KUNIT_ASSERT_NE(test, drm_match_cea_mode(preferred), 1);

	drm = &priv->drm;
	crtc = priv->crtc;
	ret = light_up_connector(test, drm, crtc, conn, preferred, ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	conn_state->hdmi.broadcast_rgb = DRM_HDMI_BROADCAST_RGB_LIMITED;

	ret = drm_atomic_check_only(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	KUNIT_ASSERT_EQ(test,
			conn_state->hdmi.broadcast_rgb,
			DRM_HDMI_BROADCAST_RGB_LIMITED);

	KUNIT_EXPECT_FALSE(test, conn_state->hdmi.is_full_range);
}

/*
 * Test that for an HDMI connector, with an HDMI monitor, if the
 * Broadcast RGB property is set to limited with a VIC-1 mode, we will
 * get a limited RGB Quantization Range.
 */
static void drm_test_check_broadcast_rgb_limited_cea_mode_vic_1(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_modeset_acquire_ctx *ctx;
	struct drm_connector_state *conn_state;
	struct drm_atomic_state *state;
	struct drm_display_mode *mode;
	struct drm_connector *conn;
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	priv = drm_atomic_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	conn = &priv->connector;
	KUNIT_ASSERT_TRUE(test, conn->display_info.is_hdmi);

	ctx = drm_kunit_helper_acquire_ctx_alloc(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);

	mode = drm_display_mode_from_cea_vic(drm, 1);
	KUNIT_ASSERT_NOT_NULL(test, mode);

	drm = &priv->drm;
	crtc = priv->crtc;
	ret = light_up_connector(test, drm, crtc, conn, mode, ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);

	state = drm_kunit_helper_atomic_state_alloc(test, drm, ctx);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, state);

	conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	conn_state->hdmi.broadcast_rgb = DRM_HDMI_BROADCAST_RGB_LIMITED;

	ret = drm_atomic_check_only(state);
	KUNIT_ASSERT_EQ(test, ret, 0);

	conn_state = drm_atomic_get_connector_state(state, conn);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, conn_state);

	KUNIT_ASSERT_EQ(test,
			conn_state->hdmi.broadcast_rgb,
			DRM_HDMI_BROADCAST_RGB_LIMITED);

	KUNIT_EXPECT_FALSE(test, conn_state->hdmi.is_full_range);
}

static struct kunit_case drm_atomic_helper_connector_hdmi_check_tests[] = {
	KUNIT_CASE(drm_test_check_broadcast_rgb_auto_cea_mode),
	KUNIT_CASE(drm_test_check_broadcast_rgb_auto_cea_mode_vic_1),
	KUNIT_CASE(drm_test_check_broadcast_rgb_full_cea_mode),
	KUNIT_CASE(drm_test_check_broadcast_rgb_full_cea_mode_vic_1),
	KUNIT_CASE(drm_test_check_broadcast_rgb_limited_cea_mode),
	KUNIT_CASE(drm_test_check_broadcast_rgb_limited_cea_mode_vic_1),
	KUNIT_CASE(drm_test_check_broadcast_rgb_crtc_mode_changed),
	KUNIT_CASE(drm_test_check_broadcast_rgb_crtc_mode_not_changed),
	{ }
};

static struct kunit_suite drm_atomic_helper_connector_hdmi_check_test_suite = {
	.name		= "drm_atomic_helper_connector_hdmi_check",
	.test_cases	= drm_atomic_helper_connector_hdmi_check_tests,
};

/*
 * Test that the value of the Broadcast RGB property out of reset is set
 * to auto.
 */
static void drm_test_check_broadcast_rgb_value(struct kunit *test)
{
	struct drm_atomic_helper_connector_hdmi_priv *priv;
	struct drm_connector_state *conn_state;
	struct drm_connector *conn;

	priv = drm_atomic_helper_connector_hdmi_init(test);
	KUNIT_ASSERT_NOT_NULL(test, priv);

	conn = &priv->connector;
	conn_state = conn->state;
	KUNIT_EXPECT_EQ(test, conn_state->hdmi.broadcast_rgb, DRM_HDMI_BROADCAST_RGB_AUTO);
}

static struct kunit_case drm_atomic_helper_connector_hdmi_reset_tests[] = {
	KUNIT_CASE(drm_test_check_broadcast_rgb_value),
	{ }
};

static struct kunit_suite drm_atomic_helper_connector_hdmi_reset_test_suite = {
	.name		= "drm_atomic_helper_connector_hdmi_reset",
	.test_cases 	= drm_atomic_helper_connector_hdmi_reset_tests,
};

kunit_test_suites(
	&drm_atomic_helper_connector_hdmi_check_test_suite,
	&drm_atomic_helper_connector_hdmi_reset_test_suite,
);

MODULE_AUTHOR("Maxime Ripard <mripard@kernel.org>");
MODULE_LICENSE("GPL");
