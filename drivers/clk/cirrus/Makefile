# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_COMMON_CLK_CS2600)		+= clk-cs2600.o

clk-cs2600_kunit-y :=		clk-cs2600-kunit.o \
				kunit_clk_cs2600_assigned.dtbo.o \
				kunit_clk_cs2600_aux1_out_clk_in.dtbo.o \
				kunit_clk_cs2600_aux1_out_clk_in_missing.dtbo.o \
				kunit_clk_cs2600_aux1_out_freq_unlock.dtbo.o \
				kunit_clk_cs2600_aux1_out_phase_unlock.dtbo.o \
				kunit_clk_cs2600_aux1_out_ref_clk_in.dtbo.o \
				kunit_clk_cs2600_bclk_invert.dtbo.o \
				kunit_clk_cs2600_fsync_duty_1.dtbo.o \
				kunit_clk_cs2600_fsync_duty_2.dtbo.o \
				kunit_clk_cs2600_fsync_duty_32.dtbo.o \
				kunit_clk_cs2600_fsync_invert.dtbo.o \
				kunit_clk_cs2600_manual_intosc_clkin.dtbo.o \
				kunit_clk_cs2600_manual_intosc_only.dtbo.o \
				kunit_clk_cs2600_manual_refclk_clkin.dtbo.o \
				kunit_clk_cs2600_manual_refclk_only.dtbo.o \
				kunit_clk_cs2600_name_output_clocks.dtbo.o \
				kunit_clk_cs2600_smart_clkin_only_intosc.dtbo.o \
				kunit_clk_cs2600_smart_clkin_only_refclk.dtbo.o \
				kunit_clk_cs2600_smart_intosc.dtbo.o \
				kunit_clk_cs2600_smart_refclk.dtbo.o

obj-$(CONFIG_COMMON_CLK_CS2600_KUNIT_TEST)	+= clk-cs2600_kunit.o
