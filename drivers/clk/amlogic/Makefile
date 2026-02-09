# SPDX-License-Identifier: (GPL-2.0-only OR MIT)

obj-$(CONFIG_COMMON_CLK_AMLOGIC) += clk-amlogic.o

clk-amlogic-y += clk.o
clk-amlogic-y += clk-basic.o
clk-amlogic-y += clk-composite.o
clk-amlogic-y += clk-dualdiv.o
clk-amlogic-y += clk-noglitch.o
clk-amlogic-y += clk-pll.o
ifneq ($(CONFIG_COMMON_CLK_AMLOGIC_MODEL),)
clk-amlogic-y += a9-model-ccu.o
endif

ifneq ($(CONFIG_COMMON_CLK_AMLOGIC_PLL),)
clk-amlogic-y += a9-pll.o
endif
