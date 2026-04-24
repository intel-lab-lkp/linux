/* SPDX-License-Identifier: ((GPL-2.0-or-later WITH Linux-syscall-note) OR MIT) */
/*
 * Rockchip ISP2 userspace API
 * Copyright (C) 2017 Rockchip Electronics Co., Ltd.
 * Copyright (C) 2026 Ideas on Board Oy.
 */

#ifndef _UAPI_RKISP2_CONFIG_H
#define _UAPI_RKISP2_CONFIG_H

/**
 * enum rkisp2_isp_version - ISP variants
 *
 * @RKISP3_V0: Used at least in RK3588
 */
enum rkisp2_isp_version {
	RKISP3_V0 = 30,
};

#endif /* _UAPI_RKISP2_CONFIG_H */
