# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_RTK_CLK_COMMON) += clk-rtk.o

clk-rtk-y += common.o

clk-rtk-y += clk-pll.o
clk-rtk-y += clk-regmap-gate.o
clk-rtk-y += clk-regmap-mux.o
clk-rtk-y += freq_table.o
clk-rtk-y += reset.o

clk-rtk-$(CONFIG_RTK_CLK_PLL_MMC) += clk-pll-mmc.o
obj-$(CONFIG_COMMON_CLK_RTD1625) += clk-rtd1625-crt.o
