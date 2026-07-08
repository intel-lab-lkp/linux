/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_OVERLAY_H
#define _UAPI_LINUX_OVERLAY_H

#include <linux/ioctl.h>
#include <linux/types.h>

/**
 * struct ovl_layers_info - overlay layer configuration summary
 * @numlower:     number of lower (metadata) layers
 * @numlowerdata: number of data-only lower layers
 * @has_upper:    1 if an upper layer is configured, 0 otherwise
 */
struct ovl_layers_info {
	__u32 numlower;
	__u32 numlowerdata;
	__u32 has_upper;
};

/*
 * Return an O_PATH fd to the root of the specified overlay layer.
 * arg == 0: upper layer (returns -ENOENT if no upper is configured)
 * arg >= 1: lower layers (returns -ENOENT if index is out of range)
 */
#define OVL_IOC_OPEN_LAYER	_IO('O', 1)

/* Retrieve overlay layer configuration into struct ovl_layers_info. */
#define OVL_IOC_GET_LAYERS_INFO	_IOR('O', 2, struct ovl_layers_info)

#endif /* _UAPI_LINUX_OVERLAY_H */
