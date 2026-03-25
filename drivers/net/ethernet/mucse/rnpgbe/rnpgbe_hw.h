/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2020 - 2025 Mucse Corporation. */

#ifndef _RNPGBE_HW_H
#define _RNPGBE_HW_H

#define MUCSE_N500_FWPF_CTRL_BASE      0x28b00
#define MUCSE_N500_FWPF_SHM_BASE       0x2d000
#define MUCSE_N500_RING_MSIX_BASE      0x28700
#define M_DEFAULT_N500_MHZ             125
#define MUCSE_GBE_PFFW_MBX_CTRL_OFFSET 0x5500
#define MUCSE_GBE_FWPF_MBX_MASK_OFFSET 0x5700
#define MUCSE_N210_FWPF_CTRL_BASE      0x29400
#define MUCSE_N210_FWPF_SHM_BASE       0x2d900
#define MUCSE_N210_RING_MSIX_BASE      0x29000
#define M_DEFAULT_N210_MHZ             62

#define TX_AXI_RW_EN                   0xc
#define RX_AXI_RW_EN                   0x03
/* mask all valid info */
#define M_ST_MASK                      0x0f000f11
/* 31:28 set 0xa to valid it is a driver set info */
#define M_DEFAULT_ST                   0xa0000000
/* driver setup this by own info */
/*bit:   27:24   |  11:8   |     4   | 0       */
/*fun:   pause   |  speed  |  duplex | up/down */
#define RNPGBE_LINK_ST                 0x000c
#define RNPGBE_DMA_AXI_EN              0x0010
#define RNPGBE_LEGACY_TIME             0xd000
#define RNPGBE_LEGACY_ENABLE           0xd004

#define MUCSE_GMAC_OFF(_n)             (0x20000 + (_n))
#define GMAC_CONTROL_RE                0x00000004
#define GMAC_CONTROL                   MUCSE_GMAC_OFF(0)
#define GMAC_FRAME_FILTER              MUCSE_GMAC_OFF(0x4)
#define RNPGBE_MAX_QUEUES 8
#endif /* _RNPGBE_HW_H */
