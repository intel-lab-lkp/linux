/*
 * Copyright 2023 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.

 * * Author: Shyam Sundar S K <Shyam-sundar.S-k@amd.com>
 */

#include <linux/backlight.h>
#include "amdgpu.h"

int amd_pmf_get_gfx_data(struct amd_gpu_pmf_data *pmf)
{
	struct drm_device *drm_dev = pci_get_drvdata(pmf->gpu_dev);
	struct drm_mode_config *mode_config = &drm_dev->mode_config;
	struct amdgpu_device *adev = drm_to_adev(drm_dev);
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;
	int i = 0;

	/* reset the count to zero */
	pmf->display_count = 0;
	if (!(adev->flags & AMD_IS_APU)) {
		DRM_ERROR("PMF-AMDGPU interface not supported\n");
		return -ENODEV;
	}

	mutex_lock(&mode_config->mutex);
	drm_connector_list_iter_begin(drm_dev, &iter);
	drm_for_each_connector_iter(connector, &iter) {
		if (connector->status == connector_status_connected) {
			pmf->con_status[i] = connector->status;
			pmf->connector_type[i] = connector->connector_type;
			pmf->display_count++;
		}
		i++;

		if (i > MAX_SUPPORTED)
			break;
	}
	drm_connector_list_iter_end(&iter);
	mutex_unlock(&mode_config->mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(amd_pmf_get_gfx_data);

static int amd_pmf_gpu_get_cur_state(struct thermal_cooling_device *cooling_dev,
				     unsigned long *state)
{
	struct backlight_device *bd;

	if (!acpi_video_backlight_use_native())
		return -ENODEV;

	bd = backlight_device_get_by_type(BACKLIGHT_RAW);
	if (!bd)
		return -ENODEV;

	*state = backlight_get_brightness(bd);

	return 0;
}

static int amd_pmf_gpu_get_max_state(struct thermal_cooling_device *cooling_dev,
				     unsigned long *state)
{
	struct backlight_device *bd;

	if (!acpi_video_backlight_use_native())
		return -ENODEV;

	bd = backlight_device_get_by_type(BACKLIGHT_RAW);
	if (!bd)
		return -ENODEV;

	if (backlight_is_blank(bd))
		*state = 0;
	else
		*state = bd->props.max_brightness;

	return 0;
}

static const struct thermal_cooling_device_ops bd_cooling_ops = {
	.get_max_state = amd_pmf_gpu_get_max_state,
	.get_cur_state = amd_pmf_gpu_get_cur_state,
};

int amd_pmf_gpu_init(struct amd_gpu_pmf_data *pmf)
{
	struct drm_device *drm_dev = pci_get_drvdata(pmf->gpu_dev);
	struct amdgpu_device *adev = drm_to_adev(drm_dev);

	if (!(adev->flags & AMD_IS_APU)) {
		DRM_ERROR("PMF-AMDGPU interface not supported\n");
		return -ENODEV;
	}

	if (!acpi_video_backlight_use_native())
		return -ENODEV;

	pmf->raw_bd = backlight_device_get_by_type(BACKLIGHT_RAW);
	if (!pmf->raw_bd)
		return -ENODEV;

	pmf->cooling_dev = thermal_cooling_device_register("pmf_gpu_bd",
							   pmf, &bd_cooling_ops);
	if (IS_ERR(pmf->cooling_dev))
		return -ENODEV;

	return 0;
}
EXPORT_SYMBOL_GPL(amd_pmf_gpu_init);

void amd_pmf_gpu_deinit(struct amd_gpu_pmf_data *pmf)
{
	thermal_cooling_device_unregister(pmf->cooling_dev);
}
EXPORT_SYMBOL_GPL(amd_pmf_gpu_deinit);
