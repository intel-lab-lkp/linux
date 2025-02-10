/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __DT_MESON_AIU_H
#define __DT_MESON_AIU_H

#define AIU_CPU			0
#define AIU_HDMI		1
#define AIU_ACODEC		2

#define CPU_I2S_FIFO		0
#define CPU_SPDIF_FIFO		1
#define CPU_I2S_ENCODER		2
#define CPU_SPDIF_ENCODER	3

#define CTRL_I2S		0
#define CTRL_PCM		1
#define CTRL_OUT		2

#define AIU_AOCLK_DIV_GATE	0
#define AIU_AOCLK_BASIC_DIV	1
#define AIU_AOCLK_MORE_DIV	2
#define AIU_LRCLK_DIV		3

#endif /* __DT_MESON_AIU_H */
