/* SPDX-License-Identifier: MIT */
#ifndef __NVBIOS_XPIO_H__
#define __NVBIOS_XPIO_H__

u16 dcb_xpio_table(struct nvkm_bios *, u8 idx,
		   u8 *ver, u8 *hdr, u8 *cnt, u8 *len);
#endif
