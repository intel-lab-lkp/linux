// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/firmware.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/soc/qcom/mdt_loader.h>

#include "iris_core.h"
#include "iris_firmware.h"

#define MAX_FIRMWARE_NAME_SIZE	128

static void iris_update_platform_data(struct iris_core *core)
{
	const char *marker = "QC_IMAGE_VERSION_STRING=";
	struct device_node *node = core->dev->of_node;
	const char *found = NULL;
	int major = 0, minor = 0;
	char version_buf[64];
	struct resource res;
	void *mem_virt;
	size_t i;

	if (!of_device_is_compatible(node, "qcom,sc7280-venus"))
		return;

	if (of_reserved_mem_region_to_resource(node, 0, &res)) {
		dev_err(core->dev, "Failed to get reserved memory for version check\n");
		return;
	}

	mem_virt = memremap(res.start, resource_size(&res), MEMREMAP_WC);
	if (!mem_virt) {
		dev_err(core->dev, "Failed to remap memory for version check\n");
		return;
	}

	for (i = 0; i < resource_size(&res) - strlen(marker); i++) {
		if (memcmp(mem_virt + i, marker, strlen(marker)) == 0) {
			found = (const char *)(mem_virt + i + strlen(marker));
			break;
		}
	}

	if (found) {
		strscpy(version_buf, found, sizeof(version_buf));

		/* Check for gen2 version string: "vfw..." OR "video-firmware.N..." (N>=2) */
		if (strncmp(version_buf, "vfw", 3) == 0 ||
		    (sscanf(version_buf, "video-firmware.%d.%d", &major, &minor) == 2 &&
			    major >= 2)) {
			dev_info(core->dev, "Gen2 FW Detected: %s\n", version_buf);
			core->iris_platform_data = &sc7280_gen2_data;
		}
	}

	memunmap(mem_virt);
}

static int iris_load_fw_to_memory(struct iris_core *core, const char *fw_name)
{
	u32 pas_id = core->iris_platform_data->pas_id;
	const struct firmware *firmware = NULL;
	struct device *dev = core->dev;
	struct resource res;
	phys_addr_t mem_phys;
	size_t res_size;
	ssize_t fw_size;
	void *mem_virt;
	int ret;

	if (strlen(fw_name) >= MAX_FIRMWARE_NAME_SIZE - 4)
		return -EINVAL;

	ret = of_reserved_mem_region_to_resource(dev->of_node, 0, &res);
	if (ret)
		return ret;

	mem_phys = res.start;
	res_size = resource_size(&res);

	ret = request_firmware(&firmware, fw_name, dev);
	if (ret)
		return ret;

	fw_size = qcom_mdt_get_size(firmware);
	if (fw_size < 0 || res_size < (size_t)fw_size) {
		ret = -EINVAL;
		goto err_release_fw;
	}

	mem_virt = memremap(mem_phys, res_size, MEMREMAP_WC);
	if (!mem_virt) {
		ret = -ENOMEM;
		goto err_release_fw;
	}

	ret = qcom_mdt_load(dev, firmware, fw_name,
			    pas_id, mem_virt, mem_phys, res_size, NULL);

	memunmap(mem_virt);
err_release_fw:
	release_firmware(firmware);

	return ret;
}

int iris_fw_load(struct iris_core *core)
{
	struct device_node *node = core->dev->of_node;
	const struct tz_cp_config *cp_config;
	const char *fwpath = NULL;
	int i, ret;

	ret = of_property_read_string_index(core->dev->of_node, "firmware-name", 0,
					    &fwpath);
	if (!ret) {
		ret = iris_load_fw_to_memory(core, fwpath);
	} else {
		bool fw_loaded = false;

		if (of_device_is_compatible(node, "qcom,sc7280-venus")) {
			ret = iris_load_fw_to_memory(core, "qcom/vpu/vpu20_p1_gen2_s6.mbn");
			if (!ret)
				fw_loaded = true;
		}

		if (!fw_loaded) {
			fwpath = core->iris_platform_data->fwname;
			dev_dbg(core->dev, "loading default fw: %s\n", fwpath);
			ret = iris_load_fw_to_memory(core, fwpath);
		}
	}

	if (ret) {
		dev_err(core->dev, "firmware download failed\n");
		return -ENOMEM;
	}

	iris_update_platform_data(core);

	ret = qcom_scm_pas_auth_and_reset(core->iris_platform_data->pas_id);
	if (ret)  {
		dev_err(core->dev, "auth and reset failed: %d\n", ret);
		return ret;
	}

	for (i = 0; i < core->iris_platform_data->tz_cp_config_data_size; i++) {
		cp_config = &core->iris_platform_data->tz_cp_config_data[i];
		ret = qcom_scm_mem_protect_video_var(cp_config->cp_start,
						     cp_config->cp_size,
						     cp_config->cp_nonpixel_start,
						     cp_config->cp_nonpixel_size);
		if (ret) {
			dev_err(core->dev, "qcom_scm_mem_protect_video_var failed: %d\n", ret);
			qcom_scm_pas_shutdown(core->iris_platform_data->pas_id);
			return ret;
		}
	}

	return ret;
}

int iris_fw_unload(struct iris_core *core)
{
	return qcom_scm_pas_shutdown(core->iris_platform_data->pas_id);
}

int iris_set_hw_state(struct iris_core *core, bool resume)
{
	return qcom_scm_set_remote_state(resume, 0);
}
