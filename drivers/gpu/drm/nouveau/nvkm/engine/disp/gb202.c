// SPDX-License-Identifier: MIT
/*
 * Copyright 2026 Valve Corp.
 */
#include "priv.h"
#include "ior.h"

#include <subdev/timer.h>

/* GB20x (NVD5.0) reorganised the SF HDMI packet units. The AVI unit is
 * unchanged from GV100, but the legacy VSI unit is gone. Vendor infoframes
 * are sent through the shared generic infoframe units instead. Register
 * layout per NVIDIA's clc971.h/clca71.h, programming sequence per
 * nvhdmipkt_C971.c:programAdvancedInfoframeC971().
 */
void
gb202_sor_hdmi_infoframe_vsi(struct nvkm_ior *ior, int head, void *data, u32 size)
{
	struct nvkm_device *device = ior->disp->engine.subdev.device;
	const u32 hoff = head * 0x400;
	/* Generic infoframe unit 1, the slot NVIDIA's driver uses for the VSI. */
	const u32 ctrl = 0x6f0138 + hoff;
	u8 buf[36] = {};
	int i;

	/* Disable the unit and wait for it to go idle. */
	nvkm_mask(device, ctrl, 0x00000001, 0x00000000);
	if (nvkm_msec(device, 2000,
		if (!(nvkm_rd32(device, ctrl) & 0x00400000))
			break;
	) < 0)
		return;

	if (!size)
		return;

	/* Clear SENT status, and point the data FIFO at unit 1's slot. */
	nvkm_mask(device, ctrl, 0x00800000, 0x00800000);
	nvkm_wr32(device, 0x6f03f0 + hoff, 0x00000001);

	/* The FIFO takes the raw packet, except that a zero HB3 is inserted
	 * after the three header bytes. A unit sends 36 bytes.
	 */
	size = min_t(u32, size, 31);
	memcpy(buf, data, min_t(u32, size, 3));
	if (size > 3)
		memcpy(&buf[4], (u8 *)data + 3, size - 3);

	for (i = 0; i < 36; i += 4) {
		nvkm_wr32(device, 0x6f03f4 + hoff, buf[i + 0] | buf[i + 1] << 8 |
						   buf[i + 2] << 16 | buf[i + 3] << 24);
	}

	/* No flip ID or scanline matching. */
	nvkm_wr32(device, 0x6f013c + hoff, 0x00000000);

	/* ENABLE | RUN_MODE=ALWAYS | LOC=VBLANK | OFFSET=1 | SIZE=0. */
	nvkm_wr32(device, ctrl, 0x00000041);

	/* Audio priority low (the init value). */
	nvkm_wr32(device, 0x6f03f8 + hoff, 0x00000002);
}
