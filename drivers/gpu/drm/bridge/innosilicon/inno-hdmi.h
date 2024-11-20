/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) Fuzhou Rockchip Electronics Co.Ltd
 *    Zheng Yang <zhengyang@rock-chips.com>
 *    Yakir Yang <ykk@rock-chips.com>
 * Copyright (C) StarFive Technology Co., Ltd.
 */

#ifndef __INNO_H__
#define __INNO_H__

#define DDC_SEGMENT_ADDR		0x30

#define HDMI_SCL_RATE			(100 * 1000)
#define DDC_BUS_FREQ_L			0x4b
#define DDC_BUS_FREQ_H			0x4c

#define HDMI_EDID_SEGMENT_POINTER	0x4d
#define HDMI_EDID_WORD_ADDR		0x4e
#define HDMI_EDID_FIFO_OFFSET		0x4f
#define HDMI_EDID_FIFO_ADDR		0x50

#define HDMI_INTERRUPT_MASK1		0xc0
#define HDMI_INTERRUPT_STATUS1		0xc1
#define	m_INT_ACTIVE_VSYNC		(1 << 5)
#define m_INT_EDID_READY		(1 << 2)

#define HDMI_STATUS			0xc8
#define m_HOTPLUG			(1 << 7)
#define m_MASK_INT_HOTPLUG		(1 << 5)
#define m_INT_HOTPLUG			(1 << 1)
#define v_MASK_INT_HOTPLUG(n)		(((n) & 0x1) << 5)

#endif /* __INNO_H__ */
