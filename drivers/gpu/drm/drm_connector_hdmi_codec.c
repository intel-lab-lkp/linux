// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2024 Linaro Ltd
 */

#include <linux/mutex.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>

#include <drm/drm_connector.h>
#include <drm/drm_device.h>

#include <sound/hdmi-codec.h>

#include "drm_internal.h"

static int drm_connector_hdmi_codec_audio_startup(struct device *dev, void *data)
{
	struct drm_connector *connector = data;
	const struct drm_connector_hdmi_codec_funcs *funcs =
		connector->hdmi.funcs->codec_funcs;

	if (funcs->audio_startup)
		return funcs->audio_startup(connector);

	return 0;
}

static int drm_connector_hdmi_codec_prepare(struct device *dev, void *data,
					    struct hdmi_codec_daifmt *fmt,
					    struct hdmi_codec_params *hparms)
{
	struct drm_connector *connector = data;
	const struct drm_connector_hdmi_codec_funcs *funcs =
		connector->hdmi.funcs->codec_funcs;

	return funcs->prepare(connector, fmt, hparms);
}

static void drm_connector_hdmi_codec_audio_shutdown(struct device *dev, void *data)
{
	struct drm_connector *connector = data;
	const struct drm_connector_hdmi_codec_funcs *funcs =
		connector->hdmi.funcs->codec_funcs;

	return funcs->audio_shutdown(connector);
}

static int drm_connector_hdmi_codec_mute_stream(struct device *dev, void *data,
						bool enable, int direction)
{
	struct drm_connector *connector = data;
	const struct drm_connector_hdmi_codec_funcs *funcs =
		connector->hdmi.funcs->codec_funcs;

	if (funcs->mute_stream)
		return funcs->mute_stream(connector, enable, direction);

	return -ENOTSUPP;
}

static int drm_connector_hdmi_codec_get_dai_id(struct snd_soc_component *comment,
		  struct device_node *endpoint,
		  void *data)
{
	struct drm_connector *connector = data;
	struct of_endpoint of_ep;
	int ret;

	if (connector->hdmi_codec.sound_dai_port < 0)
		return -ENOTSUPP;

	ret = of_graph_parse_endpoint(endpoint, &of_ep);
	if (ret < 0)
		return ret;

	if (of_ep.port == connector->hdmi_codec.sound_dai_port)
		return 0;

	return -EINVAL;
}

static int drm_connector_hdmi_codec_get_eld(struct device *dev, void *data,
					    uint8_t *buf, size_t len)
{
	struct drm_connector *connector = data;

	//  FIXME: locking against drm_edid_to_eld ?
	memcpy(buf, connector->eld, min(sizeof(connector->eld), len));

	return 0;
}

static int drm_connector_hdmi_codec_hook_plugged_cb(struct device *dev,
						    void *data,
						    hdmi_codec_plugged_cb fn,
						    struct device *codec_dev)
{
	struct drm_connector *connector = data;

	mutex_lock(&connector->hdmi_codec.lock);

	connector->hdmi_codec.plugged_cb = fn;
	connector->hdmi_codec.plugged_cb_dev = codec_dev;

	fn(codec_dev, connector->hdmi_codec.last_state);

	mutex_unlock(&connector->hdmi_codec.lock);

	return 0;
}

void drm_connector_hdmi_codec_plugged_notify(struct drm_connector *connector,
					     bool plugged)
{
	mutex_lock(&connector->hdmi_codec.lock);

	connector->hdmi_codec.last_state = plugged;

	if (connector->hdmi_codec.plugged_cb &&
	    connector->hdmi_codec.plugged_cb_dev)
		connector->hdmi_codec.plugged_cb(connector->hdmi_codec.plugged_cb_dev,
						 connector->hdmi_codec.last_state);

	mutex_unlock(&connector->hdmi_codec.lock);
}
EXPORT_SYMBOL(drm_connector_hdmi_codec_plugged_notify);

static const struct hdmi_codec_ops drm_connector_hdmi_codec_ops = {
	.audio_startup = drm_connector_hdmi_codec_audio_startup,
	.prepare = drm_connector_hdmi_codec_prepare,
	.audio_shutdown = drm_connector_hdmi_codec_audio_shutdown,
	.mute_stream = drm_connector_hdmi_codec_mute_stream,
	.get_eld = drm_connector_hdmi_codec_get_eld,
	.get_dai_id = drm_connector_hdmi_codec_get_dai_id,
	.hook_plugged_cb = drm_connector_hdmi_codec_hook_plugged_cb,
};

/**
 * drm_connector_hdmi_codec_cleanup - Cleanup the HDMI Codec for the connector
 * @connector: A pointer to the connector to cleanup
 *
 * Cleanup the HDMI codec device created for the specified connector.
 * Can be called even if the codec wasn't allocated.
 */
void drm_connector_hdmi_codec_cleanup(struct drm_connector *connector)
{
	platform_device_unregister(connector->hdmi_codec.codec_pdev);
}

/**
 * drm_connector_hdmi_codec_init - Initialize HDMI Codec device for the DRM connector
 * @connector: A pointer to the connector to allocate codec for
 *
 * Create a HDMI codec device to be used with the specified connector.
 *
 * Returns:
 * Zero on success, error code on failure.
 */
int drm_connector_hdmi_codec_init(struct drm_connector *connector)
{
	struct hdmi_codec_pdata codec_pdata = {};
	struct platform_device *pdev;

	if (!connector->hdmi.funcs->codec_funcs->prepare ||
	    !connector->hdmi.funcs->codec_funcs->audio_shutdown ||
	    !connector->hdmi_codec.dev)
		return -EINVAL;

	codec_pdata.ops = &drm_connector_hdmi_codec_ops;
	codec_pdata.i2s = connector->hdmi_codec.i2s,
	codec_pdata.spdif = connector->hdmi_codec.spdif,
	codec_pdata.max_i2s_channels = connector->hdmi_codec.max_i2s_channels,
	codec_pdata.data = connector;

	pdev = platform_device_register_data(connector->hdmi_codec.dev,
					     HDMI_CODEC_DRV_NAME,
					     PLATFORM_DEVID_AUTO,
					     &codec_pdata, sizeof(codec_pdata));
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);

	connector->hdmi_codec.codec_pdev = pdev;

	return 0;
}
