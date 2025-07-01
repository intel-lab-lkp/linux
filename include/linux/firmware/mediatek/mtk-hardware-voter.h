/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025 Collabora Ltd
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#ifndef MTK_HARDWARE_VOTER_H
#define MTK_HARDWARE_VOTER_H

#include <linux/device.h>
#include <linux/types.h>

struct mtk_hardware_voter_clk_regs {
	u16 set;
	u16 clear;
	u16 status;
};

struct mtk_hardware_voter_priv;

struct mtk_hardware_voter {
	struct device *dev;
	struct mtk_hardware_voter_priv *priv;
};

struct mtk_hardware_voter *mtk_hardware_voter_get_handle(struct device *dev);
struct mtk_hardware_voter *mtk_hardware_voter_get_by_phandle(struct device *dev);
int mtk_hardware_voter_clk_enable(struct mtk_hardware_voter *hwv,
				  const struct mtk_hardware_voter_clk_regs *clk_regs,
				  unsigned int index, bool enable);
int mtk_hardware_voter_pmdomain_enable(struct mtk_hardware_voter *hwv,
				       unsigned int index, bool enable);

#endif /* MTK_HARDWARE_VOTER_H */
