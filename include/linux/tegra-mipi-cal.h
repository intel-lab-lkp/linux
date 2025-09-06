/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __TEGRA_MIPI_CAL_H_
#define __TEGRA_MIPI_CAL_H_

#include <linux/tegra-csi.h>

struct tegra_mipi {
	const struct tegra_mipi_soc *soc;
	const struct tegra_mipi_ops *ops;
	struct device *dev;
	void __iomem *regs;
	struct mutex lock;
	struct clk *clk;

	unsigned long usage_count;
};

struct tegra_mipi_device {
	struct platform_device *pdev;
	struct tegra_mipi *mipi;
	struct tegra_csi *csi;
	struct device *device;
	unsigned long pads;
};

/**
 * Operations for Tegra MIPI calibration device
 */
struct tegra_mipi_ops {
	/**
	 * @tegra_mipi_enable:
	 *
	 * Enable MIPI calibration device
	 */
	int (*tegra_mipi_enable)(struct tegra_mipi_device *device);

	/**
	 * @tegra_mipi_disable:
	 *
	 * Disable MIPI calibration device
	 */
	int (*tegra_mipi_disable)(struct tegra_mipi_device *device);

	/**
	 * @tegra_mipi_start_calibration:
	 *
	 * Start MIPI calibration
	 */
	int (*tegra_mipi_start_calibration)(struct tegra_mipi_device *device);

	/**
	 * @tegra_mipi_finish_calibration:
	 *
	 * Finish MIPI calibration
	 */
	int (*tegra_mipi_finish_calibration)(struct tegra_mipi_device *device);
};

struct tegra_mipi_device *tegra_mipi_request(struct device *device,
					     struct device_node *np);

void tegra_mipi_free(struct tegra_mipi_device *device);

static inline int tegra_mipi_enable(struct tegra_mipi_device *device)
{
	/* Tegra114+ has a dedicated MIPI calibration block */
	if (device->mipi) {
		if (!device->mipi->ops->tegra_mipi_enable)
			return 0;

		return device->mipi->ops->tegra_mipi_enable(device);
	}

	/* Tegra20/Tegra30 have MIPI calibration logic inside CSI block */
	if (device->csi) {
		if (!device->csi->mipi_ops->tegra_mipi_enable)
			return 0;

		return device->csi->mipi_ops->tegra_mipi_enable(device);
	}

	return -ENOSYS;
}

static inline int tegra_mipi_disable(struct tegra_mipi_device *device)
{
	if (device->mipi) {
		if (!device->mipi->ops->tegra_mipi_disable)
			return 0;

		return device->mipi->ops->tegra_mipi_disable(device);
	}

	if (device->csi) {
		if (!device->csi->mipi_ops->tegra_mipi_disable)
			return 0;

		return device->csi->mipi_ops->tegra_mipi_disable(device);
	}

	return -ENOSYS;
}

static inline int tegra_mipi_start_calibration(struct tegra_mipi_device *device)
{
	if (device->mipi) {
		if (!device->mipi->ops->tegra_mipi_start_calibration)
			return 0;

		return device->mipi->ops->tegra_mipi_start_calibration(device);
	}

	if (device->csi) {
		if (!device->csi->mipi_ops->tegra_mipi_start_calibration)
			return 0;

		return device->csi->mipi_ops->tegra_mipi_start_calibration(device);
	}

	return -ENOSYS;
}

static inline int tegra_mipi_finish_calibration(struct tegra_mipi_device *device)
{
	if (device->mipi) {
		if (!device->mipi->ops->tegra_mipi_finish_calibration)
			return 0;

		return device->mipi->ops->tegra_mipi_finish_calibration(device);
	}

	if (device->csi) {
		if (!device->csi->mipi_ops->tegra_mipi_finish_calibration)
			return 0;

		return device->csi->mipi_ops->tegra_mipi_finish_calibration(device);
	}

	return -ENOSYS;
}

#endif /* __TEGRA_MIPI_CAL_H_ */
