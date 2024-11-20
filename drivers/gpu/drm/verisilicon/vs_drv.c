// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) VeriSilicon Holdings Co., Ltd.
 */
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/mfd/syscon.h>
#include <linux/of_clk.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include <drm/drm_aperture.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_client_setup.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_module.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "vs_drv.h"
#include "vs_crtc.h"
#include "vs_plane.h"
#include "vs_modeset.h"

#define DRV_NAME	"verisilicon"
#define DRV_DESC	"Verisilicon DRM driver"
#define DRV_DATE	"20230516"
#define DRV_MAJOR	1
#define DRV_MINOR	0

#define FRAC_16_16(mult, div)	 (((mult) << 16) / (div))

static const u32 primary_overlay_format[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_BGR565,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_RGBX8888,
	DRM_FORMAT_BGRX8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_RGBA8888,
	DRM_FORMAT_BGRA8888,
	DRM_FORMAT_XRGB4444,
	DRM_FORMAT_XBGR4444,
	DRM_FORMAT_RGBX4444,
	DRM_FORMAT_BGRX4444,
	DRM_FORMAT_ARGB4444,
	DRM_FORMAT_ABGR4444,
	DRM_FORMAT_RGBA4444,
	DRM_FORMAT_BGRA4444,
	DRM_FORMAT_XRGB1555,
	DRM_FORMAT_XBGR1555,
	DRM_FORMAT_RGBX5551,
	DRM_FORMAT_BGRX5551,
	DRM_FORMAT_ARGB1555,
	DRM_FORMAT_ABGR1555,
	DRM_FORMAT_RGBA5551,
	DRM_FORMAT_BGRA5551,
	DRM_FORMAT_ARGB2101010,
	DRM_FORMAT_ABGR2101010,
	DRM_FORMAT_RGBA1010102,
	DRM_FORMAT_BGRA1010102,
	DRM_FORMAT_YUYV,
	DRM_FORMAT_YVYU,
	DRM_FORMAT_UYVY,
	DRM_FORMAT_VYUY,
	DRM_FORMAT_YVU420,
	DRM_FORMAT_YUV420,
	DRM_FORMAT_NV12,
	DRM_FORMAT_NV21,
	DRM_FORMAT_NV16,
	DRM_FORMAT_NV61,
	DRM_FORMAT_P010,
};

static const u32 cursor_formats[] = {
	DRM_FORMAT_ARGB8888
};

static const u64 format_modifier[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static const u64 secondary_format_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static const struct vs_plane_data vs_plane_pri0 = {
		.num_formats	= ARRAY_SIZE(primary_overlay_format),
		.formats		= primary_overlay_format,
		.num_modifiers	= ARRAY_SIZE(format_modifier),
		.modifiers		= format_modifier,
		.min_width		= 0,
		.min_height		= 0,
		.max_width		= 4096,
		.max_height		= 4096,
		.min_scale		= FRAC_16_16(1, 3),
		.max_scale		= FRAC_16_16(10, 1),
		.rotation		= DRM_MODE_ROTATE_0 |
					  DRM_MODE_ROTATE_90 |
					  DRM_MODE_ROTATE_180 |
					  DRM_MODE_ROTATE_270 |
					  DRM_MODE_REFLECT_X |
					  DRM_MODE_REFLECT_Y,
		.color_encoding	= BIT(DRM_COLOR_YCBCR_BT709) | BIT(DRM_COLOR_YCBCR_BT2020),
		.zpos			= 0,
};

static const struct vs_plane_data vs_plane_pri1 = {
		.num_formats		= ARRAY_SIZE(primary_overlay_format),
		.formats		= primary_overlay_format,
		.num_modifiers		= ARRAY_SIZE(format_modifier),
		.modifiers		= format_modifier,
		.min_width		= 0,
		.min_height		= 0,
		.max_width		= 4096,
		.max_height		= 4096,
		.min_scale		= FRAC_16_16(1, 3),
		.max_scale		= FRAC_16_16(10, 1),
		.rotation		= DRM_MODE_ROTATE_0 |
					  DRM_MODE_ROTATE_90 |
					  DRM_MODE_ROTATE_180 |
					  DRM_MODE_ROTATE_270 |
					  DRM_MODE_REFLECT_X |
					  DRM_MODE_REFLECT_Y,
		.color_encoding		= BIT(DRM_COLOR_YCBCR_BT709) | BIT(DRM_COLOR_YCBCR_BT2020),
		.zpos			= 3,
};

static const struct vs_plane_data vs_plane_over0 = {
		.num_formats		= ARRAY_SIZE(primary_overlay_format),
		.formats		= primary_overlay_format,
		.num_modifiers		= ARRAY_SIZE(format_modifier),
		.modifiers		= format_modifier,
		.min_width		= 0,
		.min_height		= 0,
		.max_width		= 4096,
		.max_height		= 4096,
		.min_scale		= FRAC_16_16(1, 3),
		.max_scale		= FRAC_16_16(10, 1),
		.rotation		= DRM_MODE_ROTATE_0 |
					  DRM_MODE_ROTATE_90 |
					  DRM_MODE_ROTATE_180 |
					  DRM_MODE_ROTATE_270 |
					  DRM_MODE_REFLECT_X |
					  DRM_MODE_REFLECT_Y,
		.color_encoding		= BIT(DRM_COLOR_YCBCR_BT709) | BIT(DRM_COLOR_YCBCR_BT2020),
		.zpos			= 1,
};

static const struct vs_plane_data vs_plane_over1 = {
		.num_formats		= ARRAY_SIZE(primary_overlay_format),
		.formats		= primary_overlay_format,
		.num_modifiers		= ARRAY_SIZE(secondary_format_modifiers),
		.modifiers		= secondary_format_modifiers,
		.min_width		= 0,
		.min_height		= 0,
		.max_width		= 4096,
		.max_height		= 4096,
		.min_scale		= DRM_PLANE_NO_SCALING,
		.max_scale		= DRM_PLANE_NO_SCALING,
		.rotation		= 0,
		.color_encoding		= BIT(DRM_COLOR_YCBCR_BT709) | BIT(DRM_COLOR_YCBCR_BT2020),
		.zpos			= 2,
};

static const struct vs_plane_data vs_plane_over2 = {
	.num_formats		= ARRAY_SIZE(primary_overlay_format),
	.formats		= primary_overlay_format,
	.num_modifiers		= ARRAY_SIZE(format_modifier),
	.modifiers		= format_modifier,
	.min_width		= 0,
	.min_height		= 0,
	.max_width		= 4096,
	.max_height		= 4096,
	.min_scale		= FRAC_16_16(1, 3),
	.max_scale		= FRAC_16_16(10, 1),
	.rotation		= DRM_MODE_ROTATE_0 |
				  DRM_MODE_ROTATE_90 |
				  DRM_MODE_ROTATE_180 |
				  DRM_MODE_ROTATE_270 |
				  DRM_MODE_REFLECT_X |
				  DRM_MODE_REFLECT_Y,
	.color_encoding		= BIT(DRM_COLOR_YCBCR_BT709) | BIT(DRM_COLOR_YCBCR_BT2020),
	.zpos			= 4,
};

static const struct vs_plane_data vs_plane_over3 = {
		.num_formats	= ARRAY_SIZE(primary_overlay_format),
		.formats		= primary_overlay_format,
		.num_modifiers	= ARRAY_SIZE(format_modifier),
		.modifiers		= format_modifier,
		.min_width		= 0,
		.min_height		= 0,
		.max_width		= 4096,
		.max_height		= 4096,
		.min_scale		= FRAC_16_16(1, 3),
		.max_scale		= FRAC_16_16(10, 1),
		.rotation		= DRM_MODE_ROTATE_0 |
					  DRM_MODE_ROTATE_90 |
					  DRM_MODE_ROTATE_180 |
					  DRM_MODE_ROTATE_270 |
					  DRM_MODE_REFLECT_X |
					  DRM_MODE_REFLECT_Y,
		.color_encoding	= BIT(DRM_COLOR_YCBCR_BT709) | BIT(DRM_COLOR_YCBCR_BT2020),
		.zpos			= 5,
};

static const struct vs_plane_data vs_plane_cur0 = {
	.num_formats		= ARRAY_SIZE(cursor_formats),
	.formats		= cursor_formats,
	.min_width		= 32,
	.min_height		= 32,
	.max_width		= 64,
	.max_height		= 64,
	.min_scale		= DRM_PLANE_NO_SCALING,
	.max_scale		= DRM_PLANE_NO_SCALING,
	.zpos			= 255,
};

static const struct vs_plane_data vs_plane_cur1 = {
	.num_formats		= ARRAY_SIZE(cursor_formats),
	.formats		= cursor_formats,
	.min_width		= 32,
	.min_height		= 32,
	.max_width		= 64,
	.max_height		= 64,
	.zpos			= 255,
};

static const struct vs_plane_info info[] = {
	{.id = PRIMARY_PLANE_0, .data = &vs_plane_pri0,
	 .type = DRM_PLANE_TYPE_PRIMARY},
	{.id = OVERLAY_PLANE_0, .data = &vs_plane_over0,
	 .type = DRM_PLANE_TYPE_OVERLAY},
	{.id = OVERLAY_PLANE_1, .data = &vs_plane_over1,
	 .type = DRM_PLANE_TYPE_OVERLAY},
	{.id = PRIMARY_PLANE_1, .data = &vs_plane_pri1,
	 .type = DRM_PLANE_TYPE_PRIMARY},
	{.id = OVERLAY_PLANE_2, .data = &vs_plane_over2,
	 .type = DRM_PLANE_TYPE_OVERLAY},
	{.id = OVERLAY_PLANE_3, .data = &vs_plane_over3,
	 .type = DRM_PLANE_TYPE_OVERLAY},
	{.id = CURSOR_PLANE_0, .data = &vs_plane_cur0,
	 .type = DRM_PLANE_TYPE_CURSOR},
	{.id = CURSOR_PLANE_1, .data = &vs_plane_cur1,
	 .type = DRM_PLANE_TYPE_CURSOR},
};

static const struct vs_dc_info dc8200_info = {
	.name			= "DC8200",
	.plane_num      = ARRAY_SIZE(info),
	.panel_num		= 2,
	.info		= info,
	.layer_num		= 6,
	.gamma_size		= GAMMA_EX_SIZE,
	.gamma_bits		= 12,
	.pitch_alignment	= 128,
};

#define STARFIVE_SOC_CON8		0x08
# define STARFIVE_MIPI_SEL		BIT(3)

static int vs_gem_dumb_create(struct drm_file *file, struct drm_device *dev,
			      struct drm_mode_create_dumb *args)
{
	struct vs_drm_device *priv = to_vs_drm_private(dev);
	unsigned int pitch = DIV_ROUND_UP(args->width * args->bpp, 8);

	args->pitch = ALIGN(pitch, priv->pitch_alignment);
	return drm_gem_dma_dumb_create_internal(file, dev, args);
}

DEFINE_DRM_GEM_FOPS(vs_drm_fops);

static struct drm_driver vs_drm_driver = {
	.driver_features	= DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_GEM,

	DRM_GEM_DMA_DRIVER_OPS_WITH_DUMB_CREATE(vs_gem_dumb_create),

	.fops			= &vs_drm_fops,
	.name			= DRV_NAME,
	.desc			= DRV_DESC,
	.date			= DRV_DATE,
	.major			= DRV_MAJOR,
	.minor			= DRV_MINOR,
};

static irqreturn_t vs_dc_isr(int irq, void *data)
{
	struct vs_drm_device *priv = data;
	struct vs_dc *dc = &priv->dc;
	u8 status = 0;

	dc_hw_get_interrupt(&dc->hw, &status);

	if (status & BIT(0))
		drm_crtc_handle_vblank(&dc->crtc[0]->base);

	if (status & BIT(1))
		drm_crtc_handle_vblank(&dc->crtc[1]->base);

	return IRQ_HANDLED;
}

static int vs_drm_device_init_res(struct vs_drm_device *priv)
{
	struct device *dev = priv->base.dev;
	struct platform_device *pdev = to_platform_device(dev);
	int ret;
	struct vs_dc *dc;

	dc = &priv->dc;
	dc->hw.hi_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dc->hw.hi_base))
		return PTR_ERR(dc->hw.hi_base);

	dc->hw.reg_base = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(dc->hw.reg_base))
		return PTR_ERR(dc->hw.reg_base);

	dc->hw.info = (struct vs_dc_info *)of_device_get_match_data(dev);

	ret = devm_clk_bulk_get_all(dev, &priv->clks);
	if (ret < 0) {
		dev_err(dev, "can't get vout clock, ret=%d\n", ret);
		return ret;
	}
	priv->clk_count = ret;

	priv->rsts = devm_reset_control_array_get_shared(dev);
	if (IS_ERR(priv->rsts))
		return PTR_ERR(priv->rsts);

	priv->irq = platform_get_irq(pdev, 0);

	/* do not autoenable, will be enabled later */
	ret = devm_request_irq(dev, priv->irq, vs_dc_isr, IRQF_NO_AUTOEN, dev_name(dev), priv);
	if (ret < 0) {
		dev_err(dev, "Failed to install irq:%u.\n", priv->irq);
		return ret;
	}

	priv->dc_syscon_regmap = syscon_regmap_lookup_by_phandle(dev->of_node,
						"starfive,syscon");
	if (IS_ERR(priv->dc_syscon_regmap)) {
		dev_err(dev, "failed to get starfive,syscon property\n");
		return PTR_ERR(priv->dc_syscon_regmap);
	}

	return ret;
}

static u32 vs_get_addr_offset(u32 id)
{
	u32 offset = 0;

	switch (id) {
	case PRIMARY_PLANE_1:
	case OVERLAY_PLANE_1:
		offset = 0x04;
		break;
	case OVERLAY_PLANE_2:
		offset = 0x08;
		break;
	case OVERLAY_PLANE_3:
		offset = 0x0C;
		break;
	default:
		break;
	}

	return offset;
}

static u32 vs_map_possible_crtc(u32 id)
{
	switch (id) {
	case PRIMARY_PLANE_0:
	case CURSOR_PLANE_0:
		return 0x01;//crtc0
	case PRIMARY_PLANE_1:
	case CURSOR_PLANE_1:
		return 0x02;//crtc1
	default:
		return 0x03;//crtc0&crtc1
	}
}

static int vs_kms_init(struct vs_drm_device *priv)
{
	struct vs_dc *dc;
	struct drm_device *drm_dev;
	int i, ret;
	struct device_node *port;
	struct vs_crtc *crtc;
	struct vs_dc_info *dc_info;
	struct vs_plane_info *plane_info;
	struct vs_plane *plane;

	u32 max_width = 0, max_height = 0;
	u32 min_width = 0xffff, min_heigth = 0xffff;

	dc = &priv->dc;
	dc_info = dc->hw.info;
	drm_dev = &priv->base;

	for (i = 0; i < dc_info->panel_num; i++) {
		crtc = vs_crtc_create(drm_dev, dc_info);
		if (!crtc) {
			drm_err(drm_dev, "Failed to create CRTC.\n");
			ret = -ENOMEM;
			return ret;
		}
		crtc->dev = drm_dev->dev;
		crtc->index = i;

		port = of_graph_get_port_by_id(crtc->dev->of_node, i);
		if (!port) {
			drm_err(drm_dev, "no port node found for crtc_port%d\n", i);
			return -ENOENT;
		}

		crtc->base.port = port;
		dc->crtc[i] = crtc;

		of_node_put(port);
	}

	if (!dc->crtc[0]->base.port || !dc->crtc[1]->base.port) {
		drm_err(drm_dev, "no port no crtc mask, fail to create plane\n");
		return -ENOENT;
	}

	for (i = 0; i < dc_info->plane_num; i++) {
		plane_info = (struct vs_plane_info *)&dc_info->info[i];

		plane = vs_plane_create(drm_dev, plane_info, dc_info->layer_num,
					vs_map_possible_crtc(plane_info->id));

		plane->id = i;
		dc->planes[i].id = plane_info->id;
		dc->planes[i].offset = vs_get_addr_offset(plane_info->id);

		if (plane_info->type == DRM_PLANE_TYPE_PRIMARY) {
			if (plane_info->id == PRIMARY_PLANE_0)
				dc->crtc[0]->base.primary = &plane->base;
			else
				dc->crtc[1]->base.primary = &plane->base;
			min_width = min_t(u32, min_width, plane_info->data->min_width);
			min_heigth = min_t(u32, min_heigth, plane_info->data->min_height);
			/*
			 * Note: these values are used for multiple independent things:
			 * hw display mode filtering, plane buffer sizes ...
			 * Use the combined maximum values here to cover all use cases,
			 * and do more specific checking in the respective code paths.
			 */
			max_width = max_t(u32, max_width, plane_info->data->max_width);
			max_height = max_t(u32, max_height, plane_info->data->max_height);
		}

		if (plane_info->type == DRM_PLANE_TYPE_CURSOR) {
			if (plane_info->id == CURSOR_PLANE_0)
				dc->crtc[0]->base.cursor = &plane->base;
			else
				dc->crtc[1]->base.cursor = &plane->base;
			drm_dev->mode_config.cursor_width = plane_info->data->max_width;
			drm_dev->mode_config.cursor_height = plane_info->data->max_height;
		}
	}

	drm_dev->mode_config.min_width = min_width;
	drm_dev->mode_config.min_height = min_heigth;
	drm_dev->mode_config.max_width = max_width;
	drm_dev->mode_config.max_height = max_height;

	if (dc_info->pitch_alignment > priv->pitch_alignment)
		priv->pitch_alignment = dc_info->pitch_alignment;

	return 0;
}

static int vs_load(struct vs_drm_device *priv)
{
	int ret;

	ret = clk_bulk_prepare_enable(priv->clk_count, priv->clks);
	if (ret)
		return ret;

	reset_control_deassert(priv->rsts);

	ret = dc_hw_init(&priv->dc);
	if (ret) {
		DRM_ERROR("failed to init DC HW\n");
		return ret;
	}

	return 0;
}

static void vs_mipi_encoder_disable(struct drm_encoder *encoder)
{
	struct drm_device *dev = encoder->dev;
	struct vs_drm_device *priv = to_vs_drm_private(dev);
	int idx;

	if (!drm_dev_enter(dev, &idx))
		return;

	regmap_update_bits(priv->dc_syscon_regmap, STARFIVE_SOC_CON8, STARFIVE_MIPI_SEL, 0);

	drm_dev_exit(idx);
}

static void vs_mipi_encoder_enable(struct drm_encoder *encoder)
{
	struct drm_device *dev = encoder->dev;
	struct vs_drm_device *priv = to_vs_drm_private(dev);
	int idx;

	if (!drm_dev_enter(dev, &idx))
		return;

	regmap_update_bits(priv->dc_syscon_regmap, STARFIVE_SOC_CON8, STARFIVE_MIPI_SEL, BIT(3));

	drm_dev_exit(idx);
}

static const struct drm_encoder_helper_funcs vs_mipi_encoder_helper_funcs = {
	.disable = vs_mipi_encoder_disable,
	.enable = vs_mipi_encoder_enable,
};

static int vs_attach_mipi_bridge(struct vs_drm_device *priv)
{
	struct device *dev = priv->base.dev;
	struct drm_bridge *bridge;
	struct drm_encoder *encoder;
	int ret;

	bridge = devm_drm_of_get_bridge(dev, dev->of_node, 1, 1);
	if (IS_ERR(bridge)) {
		if (PTR_ERR(bridge) == -ENODEV) {
			bridge = NULL;
			return 0;
		}

		return PTR_ERR(bridge);
	}

	/* Create the encoder and attach the bridge. */
	encoder = devm_kzalloc(dev, sizeof(*encoder), GFP_KERNEL);
	if (!encoder)
		return -ENOMEM;

	encoder->possible_crtcs = drm_crtc_mask(&priv->dc.crtc[1]->base);

	ret = drmm_encoder_init(&priv->base, encoder, NULL, DRM_MODE_ENCODER_DSI, NULL);
	if (ret) {
		dev_err(dev, "Failed to initialize encoder\n");
		return ret;
	}

	drm_encoder_helper_add(encoder, &vs_mipi_encoder_helper_funcs);

	ret = drm_bridge_attach(encoder, bridge, NULL, 0);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to attach bridge\n");

	return 0;
}

static int vs_drm_bind(struct device *dev)
{
	struct vs_drm_device *priv;
	int ret;
	struct drm_device *drm_dev;

	priv = devm_drm_dev_alloc(dev, &vs_drm_driver, struct vs_drm_device, base);
	if (IS_ERR(priv))
		return PTR_ERR(priv);

	priv->pitch_alignment = 64;
	drm_dev = &priv->base;
	dev_set_drvdata(dev, drm_dev);

	ret = dma_set_coherent_mask(drm_dev->dev, DMA_BIT_MASK(40));
	if (ret)
		return ret;

	ret = vs_drm_device_init_res(priv);
	if (ret)
		return ret;

	vs_mode_config_init(drm_dev);

	/* Remove existing drivers that may own the framebuffer memory. */
	ret = drm_aperture_remove_framebuffers(&vs_drm_driver);
	if (ret)
		return ret;

	ret = vs_kms_init(priv);
	if (ret) {
		DRM_ERROR("Failed to initialize KMS pipeline\n");
		return ret;
	}

	ret = vs_load(priv);
	if (ret)
		return ret;

	/* Now try and bind all our sub-components */
	ret = component_bind_all(dev, drm_dev);
	if (ret) {
		ret = -EPROBE_DEFER;
		goto unload;
	}

	ret = vs_attach_mipi_bridge(priv);
	if (ret)
		goto err_unbind_all;

	ret = drm_vblank_init(drm_dev, drm_dev->mode_config.num_crtc);
	if (ret)
		goto err_unbind_all;

	drm_mode_config_reset(drm_dev);

	ret = drmm_kms_helper_poll_init(drm_dev);
	if (ret)
		goto err_unbind_all;

	ret = drm_dev_register(drm_dev, 0);
	if (ret)
		goto err_unbind_all;

	drm_client_setup(drm_dev, NULL);

	return 0;

err_unbind_all:
	component_unbind_all(drm_dev->dev, drm_dev);
unload:
	reset_control_assert(priv->rsts);
	clk_bulk_disable_unprepare(priv->clk_count, priv->clks);
	return ret;
}

static void vs_drm_unbind(struct device *dev)
{
	struct drm_device *drm_dev = dev_get_drvdata(dev);
	struct vs_drm_device *priv = to_vs_drm_private(drm_dev);

	reset_control_assert(priv->rsts);
	clk_bulk_disable_unprepare(priv->clk_count, priv->clks);

	drm_dev_unregister(drm_dev);
	drm_atomic_helper_shutdown(drm_dev);
	component_unbind_all(drm_dev->dev, drm_dev);
}

static const struct component_master_ops vs_drm_ops = {
	.bind = vs_drm_bind,
	.unbind = vs_drm_unbind,
};

static struct platform_driver *drm_sub_drivers[] = {
#ifdef CONFIG_DRM_INNO_STARFIVE_HDMI
	&starfive_hdmi_driver,
#endif
};

static struct component_match *vs_add_external_components(struct device *dev)
{
	struct component_match *match = NULL;
	struct device_node *node;

#ifdef CONFIG_DRM_INNO_STARFIVE_HDMI
	node = of_graph_get_remote_node(dev->of_node, 0, 0);
	drm_of_component_match_add(dev, &match, component_compare_of, node);
	of_node_put(node);
#endif

	return match ? match : ERR_PTR(-ENODEV);
}

static int vs_drm_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct component_match *match;

	/* all the planes and CRTC would be created in this platform device,
	 * so external components are encoder + connector
	 */
	match = vs_add_external_components(dev);
	if (IS_ERR(match))
		return PTR_ERR(match);

	return component_master_add_with_match(dev, &vs_drm_ops, match);
}

static void vs_drm_platform_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &vs_drm_ops);
}

#ifdef CONFIG_PM_SLEEP
static int vs_drm_suspend(struct device *dev)
{
	return drm_mode_config_helper_suspend(dev_get_drvdata(dev));
}

static int vs_drm_resume(struct device *dev)
{
	drm_mode_config_helper_resume(dev_get_drvdata(dev));

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(vs_drm_pm_ops, vs_drm_suspend, vs_drm_resume);

static const struct of_device_id vs_drm_dt_ids[] = {
	{ .compatible = "starfive,jh7110-dc8200", .data = &dc8200_info,},
	{ },
};

MODULE_DEVICE_TABLE(of, vs_drm_dt_ids);

static struct platform_driver vs_drm_platform_driver = {
	.probe = vs_drm_platform_probe,
	.remove = vs_drm_platform_remove,

	.driver = {
		.name = DRV_NAME,
		.of_match_table = vs_drm_dt_ids,
		.pm = &vs_drm_pm_ops,
	},
};

static int __init vs_drm_init(void)
{
	int ret;

	ret = platform_register_drivers(drm_sub_drivers, ARRAY_SIZE(drm_sub_drivers));
	if (ret)
		return ret;

	ret = drm_platform_driver_register(&vs_drm_platform_driver);
	if (ret)
		platform_unregister_drivers(drm_sub_drivers, ARRAY_SIZE(drm_sub_drivers));

	return ret;
}

static void __exit vs_drm_fini(void)
{
	platform_driver_unregister(&vs_drm_platform_driver);
	platform_unregister_drivers(drm_sub_drivers, ARRAY_SIZE(drm_sub_drivers));
}

module_init(vs_drm_init);
module_exit(vs_drm_fini);

MODULE_DESCRIPTION("VeriSilicon DRM Driver");
MODULE_LICENSE("GPL");
