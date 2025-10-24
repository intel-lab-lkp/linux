/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 Collabora Ltd
 * 		      AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#ifndef __DRV_CLK_MTK_SPMI_H
#define __DRV_CLK_MTK_SPMI_H

struct mtk_clk_desc;
struct platform_device;

struct mtk_spmi_clk_desc {
	const struct mtk_clk_desc *desc;
	u16 max_register;
};

#ifdef CONFIG_COMMON_CLK_MEDIATEK_SPMI

int mtk_spmi_clk_simple_probe(struct platform_device *pdev);

#else

inline int mtk_spmi_clk_simple_probe(struct platform_device *pdev)
{
	return -ENXIO;
}

#endif /* CONFIG_COMMON_CLK_MEDIATEK_SPMI */

#endif /* __DRV_CLK_MTK_SPMI_H */
