// SPDX-License-Identifier: GPL-2.0+

#include <linux/slab.h>
#include <linux/string_choices.h>

#include <drm/drm_blend.h>
#include <drm/drm_connector.h>
#include <drm/drm_mode.h>
#include <drm/drm_print.h>
#include <drm/drm_debugfs.h>
#include <kunit/visibility.h>

#include "vkms_config.h"

static const u32 vkms_supported_plane_formats[] = {
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_BGRA8888,
	DRM_FORMAT_RGBA8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_XRGB16161616,
	DRM_FORMAT_XBGR16161616,
	DRM_FORMAT_ARGB16161616,
	DRM_FORMAT_ABGR16161616,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_BGR565,
	DRM_FORMAT_NV12,
	DRM_FORMAT_NV16,
	DRM_FORMAT_NV24,
	DRM_FORMAT_NV21,
	DRM_FORMAT_NV61,
	DRM_FORMAT_NV42,
	DRM_FORMAT_YUV420,
	DRM_FORMAT_YUV422,
	DRM_FORMAT_YUV444,
	DRM_FORMAT_YVU420,
	DRM_FORMAT_YVU422,
	DRM_FORMAT_YVU444,
	DRM_FORMAT_P010,
	DRM_FORMAT_P012,
	DRM_FORMAT_P016,
	DRM_FORMAT_R1,
	DRM_FORMAT_R2,
	DRM_FORMAT_R4,
	DRM_FORMAT_R8,
};

struct vkms_config *vkms_config_create(const char *dev_name)
{
	struct vkms_config *config;

	config = kzalloc(sizeof(*config), GFP_KERNEL);
	if (!config)
		return ERR_PTR(-ENOMEM);

	config->dev_name = kstrdup_const(dev_name, GFP_KERNEL);
	if (!config->dev_name) {
		kfree(config);
		return ERR_PTR(-ENOMEM);
	}

	INIT_LIST_HEAD(&config->planes);
	INIT_LIST_HEAD(&config->crtcs);
	INIT_LIST_HEAD(&config->encoders);
	INIT_LIST_HEAD(&config->connectors);

	return config;
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_create);

struct vkms_config *vkms_config_default_create(bool enable_cursor,
					       bool enable_writeback,
					       bool enable_overlay)
{
	struct vkms_config *config;
	struct vkms_config_plane *plane_cfg;
	struct vkms_config_crtc *crtc_cfg;
	struct vkms_config_encoder *encoder_cfg;
	struct vkms_config_connector *connector_cfg;
	int n;

	config = vkms_config_create(DEFAULT_DEVICE_NAME);
	if (IS_ERR(config))
		return config;

	plane_cfg = vkms_config_create_plane(config);
	if (IS_ERR(plane_cfg))
		goto err_alloc;
	vkms_config_plane_set_type(plane_cfg, DRM_PLANE_TYPE_PRIMARY);

	crtc_cfg = vkms_config_create_crtc(config);
	if (IS_ERR(crtc_cfg))
		goto err_alloc;
	vkms_config_crtc_set_writeback(crtc_cfg, enable_writeback);

	if (vkms_config_plane_attach_crtc(plane_cfg, crtc_cfg))
		goto err_alloc;

	if (enable_overlay) {
		for (n = 0; n < NUM_OVERLAY_PLANES; n++) {
			plane_cfg = vkms_config_create_plane(config);
			if (IS_ERR(plane_cfg))
				goto err_alloc;

			vkms_config_plane_set_type(plane_cfg,
						   DRM_PLANE_TYPE_OVERLAY);

			if (vkms_config_plane_attach_crtc(plane_cfg, crtc_cfg))
				goto err_alloc;
		}
	}

	if (enable_cursor) {
		plane_cfg = vkms_config_create_plane(config);
		if (IS_ERR(plane_cfg))
			goto err_alloc;

		vkms_config_plane_set_type(plane_cfg, DRM_PLANE_TYPE_CURSOR);

		if (vkms_config_plane_attach_crtc(plane_cfg, crtc_cfg))
			goto err_alloc;
	}

	encoder_cfg = vkms_config_create_encoder(config);
	if (IS_ERR(encoder_cfg))
		goto err_alloc;

	if (vkms_config_encoder_attach_crtc(encoder_cfg, crtc_cfg))
		goto err_alloc;

	connector_cfg = vkms_config_create_connector(config);
	if (IS_ERR(connector_cfg))
		goto err_alloc;

	if (vkms_config_connector_attach_encoder(connector_cfg, encoder_cfg))
		goto err_alloc;

	return config;

err_alloc:
	vkms_config_destroy(config);
	return ERR_PTR(-ENOMEM);
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_default_create);

void vkms_config_destroy(struct vkms_config *config)
{
	struct vkms_config_plane *plane_cfg, *plane_tmp;
	struct vkms_config_crtc *crtc_cfg, *crtc_tmp;
	struct vkms_config_encoder *encoder_cfg, *encoder_tmp;
	struct vkms_config_connector *connector_cfg, *connector_tmp;

	list_for_each_entry_safe(plane_cfg, plane_tmp, &config->planes, link)
		vkms_config_destroy_plane(plane_cfg);

	list_for_each_entry_safe(crtc_cfg, crtc_tmp, &config->crtcs, link)
		vkms_config_destroy_crtc(config, crtc_cfg);

	list_for_each_entry_safe(encoder_cfg, encoder_tmp, &config->encoders, link)
		vkms_config_destroy_encoder(config, encoder_cfg);

	list_for_each_entry_safe(connector_cfg, connector_tmp, &config->connectors, link)
		vkms_config_destroy_connector(connector_cfg);

	kfree_const(config->dev_name);
	kfree(config);
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_destroy);

static bool valid_plane_number(const struct vkms_config *config)
{
	struct drm_device *dev = config->dev ? &config->dev->drm : NULL;
	size_t n_planes;

	n_planes = list_count_nodes((struct list_head *)&config->planes);
	if (n_planes <= 0 || n_planes >= 32) {
		drm_info(dev, "The number of planes must be between 1 and 31\n");
		return false;
	}

	return true;
}

VISIBLE_IF_KUNIT
bool vkms_config_valid_plane_rotation(const struct vkms_config *config,
				      const struct vkms_config_plane *plane_cfg)
{
	struct drm_device *dev = config->dev ? &config->dev->drm : NULL;

	if ((vkms_config_plane_get_default_rotation(plane_cfg) &
	     vkms_config_plane_get_supported_rotations(plane_cfg)) !=
	    vkms_config_plane_get_default_rotation(plane_cfg)) {
		drm_info(dev, "Configured default rotation is not supported by the plane\n");
		return false;
	}

	if ((vkms_config_plane_get_default_rotation(plane_cfg) & DRM_MODE_ROTATE_MASK) == 0) {
		drm_info(dev, "Configured default rotation must contains at least one possible rotation\n");
		return false;
	}

	if ((vkms_config_plane_get_supported_rotations(plane_cfg) & DRM_MODE_ROTATE_MASK) == 0) {
		drm_info(dev, "Configured supported rotations must contains at least one possible rotation\n");
		return false;
	}

	return true;
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_valid_plane_rotation);

VISIBLE_IF_KUNIT
bool vkms_config_valid_plane_color_encoding(const struct vkms_config *config,
					    const struct vkms_config_plane *plane_cfg)
{
	struct drm_device *dev = config->dev ? &config->dev->drm : NULL;

	if (vkms_config_plane_get_supported_color_encodings(plane_cfg)) {
		if ((BIT(vkms_config_plane_get_default_color_encoding(plane_cfg)) &
		     vkms_config_plane_get_supported_color_encodings(plane_cfg)) !=
		    BIT(vkms_config_plane_get_default_color_encoding(plane_cfg))) {
			drm_info(dev, "Configured default color encoding is not supported by the plane\n");
			return false;
		}
	}

	return true;
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_valid_plane_color_encoding);

VISIBLE_IF_KUNIT
bool vkms_config_valid_plane_color_range(const struct vkms_config *config,
					 const struct vkms_config_plane *plane_cfg)
{
	struct drm_device *dev = config->dev ? &config->dev->drm : NULL;

	if (vkms_config_plane_get_supported_color_ranges(plane_cfg)) {
		if ((BIT(vkms_config_plane_get_default_color_range(plane_cfg)) &
		     vkms_config_plane_get_supported_color_ranges(plane_cfg)) !=
		    BIT(vkms_config_plane_get_default_color_range(plane_cfg))) {
			drm_info(dev, "Configured default color range is not supported by the plane\n");
			return false;
		}
	}

	return true;
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_valid_plane_color_range);

static bool valid_planes_for_crtc(const struct vkms_config *config,
				  struct vkms_config_crtc *crtc_cfg)
{
	struct drm_device *dev = config->dev ? &config->dev->drm : NULL;
	struct vkms_config_plane *plane_cfg;
	bool has_primary_plane = false;
	bool has_cursor_plane = false;

	vkms_config_for_each_plane(config, plane_cfg) {
		struct vkms_config_crtc *possible_crtc;
		unsigned long idx = 0;
		enum drm_plane_type type;

		type = vkms_config_plane_get_type(plane_cfg);

		vkms_config_plane_for_each_possible_crtc(plane_cfg, idx, possible_crtc) {
			if (possible_crtc != crtc_cfg)
				continue;

			if (type == DRM_PLANE_TYPE_PRIMARY) {
				if (has_primary_plane) {
					drm_info(dev, "Multiple primary planes\n");
					return false;
				}

				has_primary_plane = true;
			} else if (type == DRM_PLANE_TYPE_CURSOR) {
				if (has_cursor_plane) {
					drm_info(dev, "Multiple cursor planes\n");
					return false;
				}

				has_cursor_plane = true;
			}
		}
	}

	if (!has_primary_plane) {
		drm_info(dev, "Primary plane not found\n");
		return false;
	}

	return true;
}

static bool valid_plane_possible_crtcs(const struct vkms_config *config)
{
	struct drm_device *dev = config->dev ? &config->dev->drm : NULL;
	struct vkms_config_plane *plane_cfg;

	vkms_config_for_each_plane(config, plane_cfg) {
		if (xa_empty(&plane_cfg->possible_crtcs)) {
			drm_info(dev, "All planes must have at least one possible CRTC\n");
			return false;
		}
	}

	return true;
}

static bool valid_crtc_number(const struct vkms_config *config)
{
	struct drm_device *dev = config->dev ? &config->dev->drm : NULL;
	size_t n_crtcs;

	n_crtcs = list_count_nodes((struct list_head *)&config->crtcs);
	if (n_crtcs <= 0 || n_crtcs >= 32) {
		drm_info(dev, "The number of CRTCs must be between 1 and 31\n");
		return false;
	}

	return true;
}

static bool valid_encoder_number(const struct vkms_config *config)
{
	struct drm_device *dev = config->dev ? &config->dev->drm : NULL;
	size_t n_encoders;

	n_encoders = list_count_nodes((struct list_head *)&config->encoders);
	if (n_encoders <= 0 || n_encoders >= 32) {
		drm_info(dev, "The number of encoders must be between 1 and 31\n");
		return false;
	}

	return true;
}

static bool valid_encoder_possible_crtcs(const struct vkms_config *config)
{
	struct drm_device *dev = config->dev ? &config->dev->drm : NULL;
	struct vkms_config_crtc *crtc_cfg;
	struct vkms_config_encoder *encoder_cfg;

	vkms_config_for_each_encoder(config, encoder_cfg) {
		if (xa_empty(&encoder_cfg->possible_crtcs)) {
			drm_info(dev, "All encoders must have at least one possible CRTC\n");
			return false;
		}
	}

	vkms_config_for_each_crtc(config, crtc_cfg) {
		bool crtc_has_encoder = false;

		vkms_config_for_each_encoder(config, encoder_cfg) {
			struct vkms_config_crtc *possible_crtc;
			unsigned long idx = 0;

			vkms_config_encoder_for_each_possible_crtc(encoder_cfg,
								   idx, possible_crtc) {
				if (possible_crtc == crtc_cfg)
					crtc_has_encoder = true;
			}
		}

		if (!crtc_has_encoder) {
			drm_info(dev, "All CRTCs must have at least one possible encoder\n");
			return false;
		}
	}

	return true;
}

static bool valid_connector_number(const struct vkms_config *config)
{
	struct drm_device *dev = config->dev ? &config->dev->drm : NULL;
	size_t n_connectors;

	n_connectors = list_count_nodes((struct list_head *)&config->connectors);
	if (n_connectors <= 0 || n_connectors >= 32) {
		drm_info(dev, "The number of connectors must be between 1 and 31\n");
		return false;
	}

	return true;
}

static bool valid_connector_possible_encoders(const struct vkms_config *config)
{
	struct drm_device *dev = config->dev ? &config->dev->drm : NULL;
	struct vkms_config_connector *connector_cfg;

	vkms_config_for_each_connector(config, connector_cfg) {
		if (xa_empty(&connector_cfg->possible_encoders)) {
			drm_info(dev,
				 "All connectors must have at least one possible encoder\n");
			return false;
		}
	}

	return true;
}

bool vkms_config_is_valid(const struct vkms_config *config)
{
	struct drm_device *dev = config->dev ? &config->dev->drm : NULL;
	struct vkms_config_crtc *crtc_cfg;
	struct vkms_config_plane *plane_cfg;

	vkms_config_for_each_plane(config, plane_cfg) {
		if (!vkms_config_valid_plane_rotation(config, plane_cfg))
			return false;

		if (!vkms_config_valid_plane_color_encoding(config, plane_cfg))
			return false;

		if (!vkms_config_valid_plane_color_range(config, plane_cfg))
			return false;

		if ((vkms_config_plane_get_supported_color_encodings(plane_cfg) == 0) !=
		    (vkms_config_plane_get_supported_color_ranges(plane_cfg) == 0)) {
			drm_info(dev,
				 "Both supported color encoding and color range must be set, or none\n");
			return false;
		}
	}

	if (!valid_plane_number(config))
		return false;

	if (!valid_crtc_number(config))
		return false;

	if (!valid_encoder_number(config))
		return false;

	if (!valid_connector_number(config))
		return false;

	if (!valid_plane_possible_crtcs(config))
		return false;

	vkms_config_for_each_crtc(config, crtc_cfg) {
		if (!valid_planes_for_crtc(config, crtc_cfg))
			return false;
	}

	if (!valid_encoder_possible_crtcs(config))
		return false;

	if (!valid_connector_possible_encoders(config))
		return false;

	return true;
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_is_valid);

static void show_bitfield(struct seq_file *m, uint32_t value, const char *callback(unsigned int))
{
	u32 offset = 0;
	bool first = true;

	while (value >= BIT(offset) && offset < 32) {
		if (value & BIT(offset)) {
			if (!first)
				seq_puts(m, ",");
			seq_printf(m, callback(offset));
			first = false;
		}
		offset += 1;
	}
}

static void show_formats(struct seq_file *m, u32 *formats, size_t formats_count)
{
	for (int i = 0; i < formats_count; i++) {
		seq_printf(m, "%p4cc", &formats[i]);
		if (i != formats_count - 1)
			seq_puts(m, ", ");
	}
}

static int vkms_config_show(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct drm_device *dev = entry->dev;
	struct vkms_device *vkmsdev = drm_device_to_vkms_device(dev);
	const char *dev_name;
	struct vkms_config_plane *plane_cfg;
	struct vkms_config_crtc *crtc_cfg;
	struct vkms_config_encoder *encoder_cfg;
	struct vkms_config_connector *connector_cfg;

	dev_name = vkms_config_get_device_name((struct vkms_config *)vkmsdev->config);
	seq_printf(m, "dev_name=%s\n", dev_name);

	vkms_config_for_each_plane(vkmsdev->config, plane_cfg) {
		seq_puts(m, "plane:\n");
		seq_printf(m, "\ttype=%s\n",
			   drm_get_plane_type_name(vkms_config_plane_get_type(plane_cfg)));
		seq_printf(m, "\tname=%s\n",
			   vkms_config_plane_get_name(plane_cfg));
		seq_puts(m, "\tsupported rotations=");
		show_bitfield(m, vkms_config_plane_get_supported_rotations(plane_cfg),
			      drm_get_rotation_name);
		seq_puts(m, "\n");
		seq_puts(m, "\tdefault rotation=");
		show_bitfield(m, vkms_config_plane_get_default_rotation(plane_cfg),
			      drm_get_rotation_name);
		seq_puts(m, "\n");
		seq_puts(m, "\tsupported color encoding=");
		show_bitfield(m, vkms_config_plane_get_supported_color_encodings(plane_cfg),
			      drm_get_color_encoding_name);
		seq_puts(m, "\n");
		seq_printf(m, "\tdefault color encoding=%s\n",
			   drm_get_color_encoding_name(vkms_config_plane_get_default_color_encoding(plane_cfg)));
		seq_puts(m, "\tsupported color range=");
		show_bitfield(m, vkms_config_plane_get_supported_color_ranges(plane_cfg),
			      drm_get_color_range_name);
		seq_puts(m, "\n");
		seq_printf(m, "\tdefault color range=%s\n",
			   drm_get_color_range_name(vkms_config_plane_get_default_color_range(plane_cfg)));
		seq_puts(m, "\tsupported formats=");
		show_formats(m, vkms_config_plane_get_supported_formats(plane_cfg),
			     vkms_config_plane_get_supported_formats_count(plane_cfg));
		seq_puts(m, "\n");
	}

	vkms_config_for_each_crtc(vkmsdev->config, crtc_cfg) {
		seq_puts(m, "crtc:\n");
		seq_printf(m, "\twriteback=%s\n",
			   str_enabled_disabled(vkms_config_crtc_get_writeback(crtc_cfg)));
	}

	vkms_config_for_each_encoder(vkmsdev->config, encoder_cfg)
		seq_puts(m, "encoder\n");

	vkms_config_for_each_connector(vkmsdev->config, connector_cfg) {
		seq_puts(m, "connector:\n");
		seq_printf(m, "\tstatus=%s\n",
			   drm_get_connector_status_name(vkms_config_connector_get_status(connector_cfg)));
	}

	return 0;
}

static const struct drm_debugfs_info vkms_config_debugfs_list[] = {
	{ "vkms_config", vkms_config_show, 0 },
};

void vkms_config_register_debugfs(struct vkms_device *vkms_device)
{
	drm_debugfs_add_files(&vkms_device->drm, vkms_config_debugfs_list,
			      ARRAY_SIZE(vkms_config_debugfs_list));
}

struct vkms_config_plane *vkms_config_create_plane(struct vkms_config *config)
{
	struct vkms_config_plane *plane_cfg;

	plane_cfg = kzalloc(sizeof(*plane_cfg), GFP_KERNEL);
	if (!plane_cfg)
		return ERR_PTR(-ENOMEM);

	if (vkms_config_plane_add_all_formats(plane_cfg)) {
		kfree(plane_cfg);
		return ERR_PTR(-ENOMEM);
	}

	plane_cfg->config = config;
	vkms_config_plane_set_type(plane_cfg, DRM_PLANE_TYPE_OVERLAY);
	vkms_config_plane_set_name(plane_cfg, NULL);
	vkms_config_plane_set_supported_rotations(plane_cfg,
						  DRM_MODE_ROTATE_MASK | DRM_MODE_REFLECT_MASK);
	vkms_config_plane_set_default_rotation(plane_cfg, DRM_MODE_ROTATE_0);
	vkms_config_plane_set_supported_color_encodings(plane_cfg, BIT(DRM_COLOR_YCBCR_BT601) |
							BIT(DRM_COLOR_YCBCR_BT709) |
							BIT(DRM_COLOR_YCBCR_BT2020));
	vkms_config_plane_set_default_color_encoding(plane_cfg, DRM_COLOR_YCBCR_BT601);
	vkms_config_plane_set_supported_color_ranges(plane_cfg, BIT(DRM_COLOR_YCBCR_LIMITED_RANGE) |
								BIT(DRM_COLOR_YCBCR_FULL_RANGE));
	vkms_config_plane_set_default_color_range(plane_cfg, DRM_COLOR_YCBCR_FULL_RANGE);

	xa_init_flags(&plane_cfg->possible_crtcs, XA_FLAGS_ALLOC);

	list_add_tail(&plane_cfg->link, &config->planes);

	return plane_cfg;
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_create_plane);

void vkms_config_destroy_plane(struct vkms_config_plane *plane_cfg)
{
	xa_destroy(&plane_cfg->possible_crtcs);
	list_del(&plane_cfg->link);
	kfree_const(plane_cfg->name);
	kfree(plane_cfg);
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_destroy_plane);

int __must_check vkms_config_plane_attach_crtc(struct vkms_config_plane *plane_cfg,
					       struct vkms_config_crtc *crtc_cfg)
{
	struct vkms_config_crtc *possible_crtc;
	unsigned long idx = 0;
	u32 crtc_idx = 0;

	if (plane_cfg->config != crtc_cfg->config)
		return -EINVAL;

	vkms_config_plane_for_each_possible_crtc(plane_cfg, idx, possible_crtc) {
		if (possible_crtc == crtc_cfg)
			return -EEXIST;
	}

	return xa_alloc(&plane_cfg->possible_crtcs, &crtc_idx, crtc_cfg,
			xa_limit_32b, GFP_KERNEL);
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_plane_attach_crtc);

void vkms_config_plane_detach_crtc(struct vkms_config_plane *plane_cfg,
				   struct vkms_config_crtc *crtc_cfg)
{
	struct vkms_config_crtc *possible_crtc;
	unsigned long idx = 0;

	vkms_config_plane_for_each_possible_crtc(plane_cfg, idx, possible_crtc) {
		if (possible_crtc == crtc_cfg)
			xa_erase(&plane_cfg->possible_crtcs, idx);
	}
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_plane_detach_crtc);

struct vkms_config_crtc *vkms_config_create_crtc(struct vkms_config *config)
{
	struct vkms_config_crtc *crtc_cfg;

	crtc_cfg = kzalloc(sizeof(*crtc_cfg), GFP_KERNEL);
	if (!crtc_cfg)
		return ERR_PTR(-ENOMEM);

	crtc_cfg->config = config;
	vkms_config_crtc_set_writeback(crtc_cfg, false);

	list_add_tail(&crtc_cfg->link, &config->crtcs);

	return crtc_cfg;
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_create_crtc);

void vkms_config_destroy_crtc(struct vkms_config *config,
			      struct vkms_config_crtc *crtc_cfg)
{
	struct vkms_config_plane *plane_cfg;
	struct vkms_config_encoder *encoder_cfg;

	vkms_config_for_each_plane(config, plane_cfg)
		vkms_config_plane_detach_crtc(plane_cfg, crtc_cfg);

	vkms_config_for_each_encoder(config, encoder_cfg)
		vkms_config_encoder_detach_crtc(encoder_cfg, crtc_cfg);

	list_del(&crtc_cfg->link);
	kfree(crtc_cfg);
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_destroy_crtc);

/**
 * vkms_config_crtc_get_plane() - Return the first attached plane to a CRTC with
 * the specific type
 * @config: Configuration containing the CRTC and the plane
 * @crtc_cfg: Only find planes attached to this CRTC
 * @type: Plane type to search
 *
 * Returns:
 * The first plane found attached to @crtc_cfg with the type @type.
 */
static struct vkms_config_plane *vkms_config_crtc_get_plane(const struct vkms_config *config,
							    struct vkms_config_crtc *crtc_cfg,
							    enum drm_plane_type type)
{
	struct vkms_config_plane *plane_cfg;
	struct vkms_config_crtc *possible_crtc;
	enum drm_plane_type current_type;
	unsigned long idx = 0;

	vkms_config_for_each_plane(config, plane_cfg) {
		current_type = vkms_config_plane_get_type(plane_cfg);

		vkms_config_plane_for_each_possible_crtc(plane_cfg, idx, possible_crtc) {
			if (possible_crtc == crtc_cfg && current_type == type)
				return plane_cfg;
		}
	}

	return NULL;
}

int __must_check vkms_config_plane_add_all_formats(struct vkms_config_plane *plane_cfg)
{
	u32 *ret = krealloc_array(plane_cfg->supported_formats,
				  ARRAY_SIZE(vkms_supported_plane_formats),
				  sizeof(uint32_t), GFP_KERNEL);
	if (!ret)
		return -ENOMEM;
	plane_cfg->supported_formats = ret;

	memcpy(plane_cfg->supported_formats, vkms_supported_plane_formats,
	       sizeof(vkms_supported_plane_formats));
	plane_cfg->supported_formats_count = ARRAY_SIZE(vkms_supported_plane_formats);
	return 0;
}

int __must_check vkms_config_plane_add_format(struct vkms_config_plane *plane_cfg, u32 drm_format)
{
	bool found = false;

	for (int i = 0; i < ARRAY_SIZE(vkms_supported_plane_formats); i++) {
		if (vkms_supported_plane_formats[i] == drm_format) {
			found = true;
			break;
		}
	}

	if (!found)
		return -EINVAL;
	for (unsigned int i = 0; i < plane_cfg->supported_formats_count; i++) {
		if (plane_cfg->supported_formats[i] == drm_format)
			return 0;
	}
	u32 *new_ptr = krealloc_array(plane_cfg->supported_formats,
				      plane_cfg->supported_formats_count + 1,
				      sizeof(*plane_cfg->supported_formats), GFP_KERNEL);
	if (!new_ptr)
		return -ENOMEM;

	plane_cfg->supported_formats = new_ptr;
	plane_cfg->supported_formats[plane_cfg->supported_formats_count] = drm_format;
	plane_cfg->supported_formats_count++;

	return 0;
}

void vkms_config_plane_remove_all_formats(struct vkms_config_plane *plane_cfg)
{
	plane_cfg->supported_formats_count = 0;
}

void vkms_config_plane_remove_format(struct vkms_config_plane *plane_cfg, u32 drm_format)
{
	for (unsigned int i = 0; i < plane_cfg->supported_formats_count; i++) {
		if (plane_cfg->supported_formats[i] == drm_format) {
			plane_cfg->supported_formats[i] = plane_cfg->supported_formats[plane_cfg->supported_formats_count - 1];
			plane_cfg->supported_formats_count--;
		}
	}
}

struct vkms_config_plane *vkms_config_crtc_primary_plane(const struct vkms_config *config,
							 struct vkms_config_crtc *crtc_cfg)
{
	return vkms_config_crtc_get_plane(config, crtc_cfg, DRM_PLANE_TYPE_PRIMARY);
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_crtc_primary_plane);

struct vkms_config_plane *vkms_config_crtc_cursor_plane(const struct vkms_config *config,
							struct vkms_config_crtc *crtc_cfg)
{
	return vkms_config_crtc_get_plane(config, crtc_cfg, DRM_PLANE_TYPE_CURSOR);
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_crtc_cursor_plane);

struct vkms_config_encoder *vkms_config_create_encoder(struct vkms_config *config)
{
	struct vkms_config_encoder *encoder_cfg;

	encoder_cfg = kzalloc(sizeof(*encoder_cfg), GFP_KERNEL);
	if (!encoder_cfg)
		return ERR_PTR(-ENOMEM);

	encoder_cfg->config = config;
	xa_init_flags(&encoder_cfg->possible_crtcs, XA_FLAGS_ALLOC);

	list_add_tail(&encoder_cfg->link, &config->encoders);

	return encoder_cfg;
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_create_encoder);

void vkms_config_destroy_encoder(struct vkms_config *config,
				 struct vkms_config_encoder *encoder_cfg)
{
	struct vkms_config_connector *connector_cfg;

	vkms_config_for_each_connector(config, connector_cfg)
		vkms_config_connector_detach_encoder(connector_cfg, encoder_cfg);

	xa_destroy(&encoder_cfg->possible_crtcs);
	list_del(&encoder_cfg->link);
	kfree(encoder_cfg);
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_destroy_encoder);

int __must_check vkms_config_encoder_attach_crtc(struct vkms_config_encoder *encoder_cfg,
						 struct vkms_config_crtc *crtc_cfg)
{
	struct vkms_config_crtc *possible_crtc;
	unsigned long idx = 0;
	u32 crtc_idx = 0;

	if (encoder_cfg->config != crtc_cfg->config)
		return -EINVAL;

	vkms_config_encoder_for_each_possible_crtc(encoder_cfg, idx, possible_crtc) {
		if (possible_crtc == crtc_cfg)
			return -EEXIST;
	}

	return xa_alloc(&encoder_cfg->possible_crtcs, &crtc_idx, crtc_cfg,
			xa_limit_32b, GFP_KERNEL);
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_encoder_attach_crtc);

void vkms_config_encoder_detach_crtc(struct vkms_config_encoder *encoder_cfg,
				     struct vkms_config_crtc *crtc_cfg)
{
	struct vkms_config_crtc *possible_crtc;
	unsigned long idx = 0;

	vkms_config_encoder_for_each_possible_crtc(encoder_cfg, idx, possible_crtc) {
		if (possible_crtc == crtc_cfg)
			xa_erase(&encoder_cfg->possible_crtcs, idx);
	}
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_encoder_detach_crtc);

struct vkms_config_connector *vkms_config_create_connector(struct vkms_config *config)
{
	struct vkms_config_connector *connector_cfg;

	connector_cfg = kzalloc(sizeof(*connector_cfg), GFP_KERNEL);
	if (!connector_cfg)
		return ERR_PTR(-ENOMEM);

	connector_cfg->config = config;
	connector_cfg->status = connector_status_connected;
	xa_init_flags(&connector_cfg->possible_encoders, XA_FLAGS_ALLOC);

	list_add_tail(&connector_cfg->link, &config->connectors);

	return connector_cfg;
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_create_connector);

void vkms_config_destroy_connector(struct vkms_config_connector *connector_cfg)
{
	xa_destroy(&connector_cfg->possible_encoders);
	list_del(&connector_cfg->link);
	kfree(connector_cfg);
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_destroy_connector);

int __must_check vkms_config_connector_attach_encoder(struct vkms_config_connector *connector_cfg,
						      struct vkms_config_encoder *encoder_cfg)
{
	struct vkms_config_encoder *possible_encoder;
	unsigned long idx = 0;
	u32 encoder_idx = 0;

	if (connector_cfg->config != encoder_cfg->config)
		return -EINVAL;

	vkms_config_connector_for_each_possible_encoder(connector_cfg, idx,
							possible_encoder) {
		if (possible_encoder == encoder_cfg)
			return -EEXIST;
	}

	return xa_alloc(&connector_cfg->possible_encoders, &encoder_idx,
			encoder_cfg, xa_limit_32b, GFP_KERNEL);
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_connector_attach_encoder);

void vkms_config_connector_detach_encoder(struct vkms_config_connector *connector_cfg,
					  struct vkms_config_encoder *encoder_cfg)
{
	struct vkms_config_encoder *possible_encoder;
	unsigned long idx = 0;

	vkms_config_connector_for_each_possible_encoder(connector_cfg, idx,
							possible_encoder) {
		if (possible_encoder == encoder_cfg)
			xa_erase(&connector_cfg->possible_encoders, idx);
	}
}
EXPORT_SYMBOL_IF_KUNIT(vkms_config_connector_detach_encoder);
