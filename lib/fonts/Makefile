# SPDX-License-Identifier: GPL-2.0
# Font handling

font-objs := fonts.o

font-objs-$(CONFIG_FONT_SUN8x16)   += font_sun8x16.o
font-objs-$(CONFIG_FONT_SUN12x22)  += font_sun12x22.o
font-objs-$(CONFIG_FONT_8x8)       += font_8x8.o
font-objs-$(CONFIG_FONT_8x16)      += font_8x16.o
font-objs-$(CONFIG_FONT_6x11)      += font_6x11.o
font-objs-$(CONFIG_FONT_7x14)      += font_7x14.o
font-objs-$(CONFIG_FONT_10x18)     += font_10x18.o
font-objs-$(CONFIG_FONT_PEARL_8x8) += font_pearl_8x8.o
font-objs-$(CONFIG_FONT_ACORN_8x8) += font_acorn_8x8.o
font-objs-$(CONFIG_FONT_MINI_4x6)  += font_mini_4x6.o
font-objs-$(CONFIG_FONT_6x10)      += font_6x10.o
font-objs-$(CONFIG_FONT_TER6x12N)  += font_ter-112n.o
font-objs-$(CONFIG_FONT_TER8x14B)  += font_ter-114b.o
font-objs-$(CONFIG_FONT_TER8x14N)  += font_ter-114n.o
font-objs-$(CONFIG_FONT_TER8x16B)  += font_ter-116b.o
font-objs-$(CONFIG_FONT_TER8x16N)  += font_ter-116n.o
font-objs-$(CONFIG_FONT_TER10x18B)  += font_ter-118b.o
font-objs-$(CONFIG_FONT_TER10x18N)  += font_ter-118n.o
font-objs-$(CONFIG_FONT_TER10x20B)  += font_ter-120b.o
font-objs-$(CONFIG_FONT_TER10x20N)  += font_ter-120n.o
font-objs-$(CONFIG_FONT_TER11x22B)  += font_ter-122b.o
font-objs-$(CONFIG_FONT_TER11x22N)  += font_ter-122n.o
font-objs-$(CONFIG_FONT_TER12x24B)  += font_ter-124b.o
font-objs-$(CONFIG_FONT_TER12x24N)  += font_ter-124n.o
font-objs-$(CONFIG_FONT_TER14x28B)  += font_ter-128b.o
font-objs-$(CONFIG_FONT_TER14x28N)  += font_ter-128n.o
font-objs-$(CONFIG_FONT_TER16x32B)  += font_ter-132b.o
font-objs-$(CONFIG_FONT_TER16x32N)  += font_ter-132n.o
font-objs-$(CONFIG_FONT_6x8)       += font_6x8.o

font-objs += $(font-objs-y)

obj-$(CONFIG_FONT_SUPPORT)         += font.o
