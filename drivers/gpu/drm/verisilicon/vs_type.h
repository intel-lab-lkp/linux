/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) VeriSilicon Holdings Co., Ltd.
 */

#ifndef __VS_TYPE_H__
#define __VS_TYPE_H__

enum drm_plane_type;

struct vs_plane_data {
	unsigned int num_formats;
	const u32 *formats;
	u8 num_modifiers;
	const u64 *modifiers;
	unsigned int min_width;
	unsigned int min_height;
	unsigned int max_width;
	unsigned int max_height;
	unsigned int rotation;
	unsigned int blend_mode;
	unsigned int color_encoding;
	int min_scale; /* 16.16 fixed point */
	int max_scale; /* 16.16 fixed point */
	u8   zpos;
};

struct vs_plane_info {
	u32 id;
	const struct vs_plane_data *data;
	enum drm_plane_type type;
};

struct vs_dc_info {
	const char *name;

	u8 panel_num;

	/* planes */
	u8 plane_num;

	u8 layer_num;
	u8 primary_num;
	u8 overlay_num;
	u8 cursor_num;
	const struct vs_plane_info *info;
	/* 0 means no gamma LUT */
	u16 gamma_size;
	u8 gamma_bits;

	u16 pitch_alignment;
};

#endif /* __VS_TYPE_H__ */
