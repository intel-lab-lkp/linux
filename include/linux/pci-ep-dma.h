/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_PCI_EP_DMA_H
#define __LINUX_PCI_EP_DMA_H

#include <linux/bits.h>

/*
 * BAR metadata format used by PCI endpoint functions that expose an
 * endpoint-integrated DMA controller to a PCI host.
 *
 * Offsets are relative to the beginning of the metadata blob. Multi-byte
 * fields are little-endian. The blob is normally placed at offset 0 of a
 * function BAR selected by the endpoint function and discovered by the host
 * driver using device-specific policy. Other data in the same BAR, such as a
 * standard MSI-X table or PBA, is outside this metadata format.
 *
 *          31                                                              0
 *          +---------------------------------------------------------------+
 * +0x000   | metadata magic                                                |
 *          +---------------------------------------------------------------+
 *          31                            16 15            8 7              0
 *          +-------------------------------+---------------+---------------+
 * +0x004   | metadata length               | reserved      | revision      |
 *          +-------------------------------+---------------+---------------+
 *          31 30 29     27 26          19 18          11 10           3 2  0
 *          +--+--+--------+--------------+--------------+--------------+---+
 * +0x008   |R |H |reserved| ch entry size| RD count     | WR count     |BAR|
 *          +--+--+--------+--------------+--------------+--------------+---+
 * +0x00c   | register window offset[31:0]                                  |
 *          +---------------------------------------------------------------+
 * +0x010   | register window offset[63:32]                                 |
 *          +---------------------------------------------------------------+
 *          31                            16 15            8 7              0
 *          +-------------------------------+---------------+---------------+
 * +0x014   | reserved                      | layout data   | layout        |
 *          +-------------------------------+---------------+---------------+
 * +0x018   | register window size                                          |
 *          +---------------------------------------------------------------+
 * +0x01c   | write table                                                   |
 *          | ( channel table entries #0 ~ #N )                             |
 *          +---------------------------------------------------------------+
 *          | read table                                                    |
 *          | ( channel table entries #0 ~ #N )                             |
 *          +---------------------------------------------------------------+
 *
 *          metadata magic: PCI_EP_DMA_METADATA_MAGIC.
 *          metadata length: byte size of the whole metadata blob.
 *          revision: metadata format revision.
 *          R: ready bit. Set when all BAR windows described by this metadata
 *             have been programmed and can be used by the host.
 *          H: host-request bit. Set by the host driver after it has found this
 *             metadata. The endpoint may use this as the trigger to program
 *             DMA window BAR subrange mappings.
 *          ch entry size: byte stride between consecutive channel table
 *                         entries. Revision 1 requires at least
 *                         PCI_EP_DMA_METADATA_CH_ENTRY_SIZE bytes.
 *          RD count: number of exposed RC-to-endpoint DMA read channels and
 *                    read channel-table entries.
 *          WR count: number of exposed endpoint-to-RC DMA write channels and
 *                    write channel-table entries.
 *          BAR: BAR that contains the DMA controller register window.
 *          register window offset: BAR-local byte offset of the DMA controller
 *                                  register window in BAR.
 *          register window size: DMA controller register window size in bytes.
 *          layout: DMA controller register layout identifier.
 *          layout data: layout-specific data. For
 *                       PCI_EP_DMA_METADATA_REG_LAYOUT_DW_EDMA this is the
 *                       DesignWare eDMA/HDMA map format.
 *          write table: starts at PCI_EP_DMA_METADATA_HDR_LEN if write channel
 *                       count is non-zero.
 *          read table: starts at PCI_EP_DMA_METADATA_HDR_LEN plus the write
 *                      table size if read channel count is non-zero.
 *
 *
 * Channel table entry:
 *
 *          31                 17 16 15 14   12 11 10     8 7                0
 *          +--------------------+--+--+-------+--+--------+----------------+
 * +0x000   | reserved           |A |rs|aux BAR|rs|desc BAR|hardware channel|
 *          +--------------------+--+--+-------+--+--------+----------------+
 * +0x004   | descriptor window BAR offset[31:0]                            |
 *          +---------------------------------------------------------------+
 * +0x008   | descriptor window BAR offset[63:32]                           |
 *          +---------------------------------------------------------------+
 * +0x00c   | descriptor window size                                        |
 *          +---------------------------------------------------------------+
 * +0x010   | descriptor DMA address[31:0]                                  |
 *          +---------------------------------------------------------------+
 * +0x014   | descriptor DMA address[63:32]                                 |
 *          +---------------------------------------------------------------+
 * +0x018   | auxiliary window BAR offset[31:0]                             |
 *          +---------------------------------------------------------------+
 * +0x01c   | auxiliary window BAR offset[63:32]                            |
 *          +---------------------------------------------------------------+
 * +0x020   | auxiliary window size                                         |
 *          +---------------------------------------------------------------+
 * +0x024   | auxiliary DMA address[31:0]                                   |
 *          +---------------------------------------------------------------+
 * +0x028   | auxiliary DMA address[63:32]                                  |
 *          +---------------------------------------------------------------+
 *
 *          A: auxiliary-window-valid bit. If clear, aux BAR and auxiliary
 *             window fields are ignored.
 *          aux BAR: BAR that contains the optional auxiliary window.
 *          desc BAR: BAR that contains the descriptor window.
 *          hardware channel: DMA controller's hardware channel number.
 *                            Revision 1 entries are currently consumed in dense
 *                            0-based order.
 *          descriptor window BAR offset: BAR-local byte offset of the
 *                                        descriptor window in desc BAR.
 *          descriptor window size: descriptor window size in bytes.
 *          descriptor DMA address: endpoint-local address used by the DMA
 *                                  controller to fetch descriptors.
 *          auxiliary window BAR offset: BAR-local byte offset of the auxiliary
 *                                       window in aux BAR.
 *          auxiliary window size: auxiliary window size in bytes.
 *          auxiliary DMA address: endpoint-local address corresponding to the
 *                                 auxiliary window.
 */
#define PCI_EP_DMA_METADATA_MAGIC		0x4d444550 /* "PEDM" */
#define PCI_EP_DMA_METADATA_REV			0x1

#define PCI_EP_DMA_METADATA_HDR_LEN		0x1c

#define PCI_EP_DMA_METADATA_HDR			0x04
#define  PCI_EP_DMA_METADATA_HDR_REV		GENMASK(7, 0)
#define  PCI_EP_DMA_METADATA_HDR_LEN_FIELD	GENMASK(31, 16)

#define PCI_EP_DMA_METADATA_CTRL		0x08
#define  PCI_EP_DMA_METADATA_CTRL_REG_BAR	GENMASK(2, 0)
#define  PCI_EP_DMA_METADATA_CTRL_WR_CH_COUNT	GENMASK(10, 3)
#define  PCI_EP_DMA_METADATA_CTRL_RD_CH_COUNT	GENMASK(18, 11)
#define  PCI_EP_DMA_METADATA_CTRL_CH_ENTRY_SIZE	GENMASK(26, 19)
#define  PCI_EP_DMA_METADATA_CTRL_HOST_REQ	BIT(30)
#define  PCI_EP_DMA_METADATA_CTRL_READY		BIT(31)

#define PCI_EP_DMA_METADATA_REG_OFF_LO		0x0c
#define PCI_EP_DMA_METADATA_REG_OFF_HI		0x10
#define PCI_EP_DMA_METADATA_REG_LAYOUT		0x14
#define  PCI_EP_DMA_METADATA_REG_LAYOUT_ID	GENMASK(7, 0)
#define  PCI_EP_DMA_METADATA_REG_LAYOUT_DATA	GENMASK(15, 8)
#define PCI_EP_DMA_METADATA_REG_SIZE		0x18

#define PCI_EP_DMA_METADATA_REG_LAYOUT_DW_EDMA	0x1

#define PCI_EP_DMA_METADATA_CH_ENTRY_SIZE	0x2c
#define PCI_EP_DMA_METADATA_CH_CTRL		0x00
#define  PCI_EP_DMA_METADATA_CH_CTRL_HW_CH	GENMASK(7, 0)
#define  PCI_EP_DMA_METADATA_CH_CTRL_DESC_BAR	GENMASK(10, 8)
#define  PCI_EP_DMA_METADATA_CH_CTRL_AUX_BAR	GENMASK(14, 12)
#define  PCI_EP_DMA_METADATA_CH_CTRL_AUX_VALID	BIT(16)
#define PCI_EP_DMA_METADATA_CH_DESC_OFF_LO	0x04
#define PCI_EP_DMA_METADATA_CH_DESC_OFF_HI	0x08
#define PCI_EP_DMA_METADATA_CH_DESC_SIZE		0x0c
#define PCI_EP_DMA_METADATA_CH_DESC_ADDR_LO	0x10
#define PCI_EP_DMA_METADATA_CH_DESC_ADDR_HI	0x14
#define PCI_EP_DMA_METADATA_CH_AUX_OFF_LO	0x18
#define PCI_EP_DMA_METADATA_CH_AUX_OFF_HI	0x1c
#define PCI_EP_DMA_METADATA_CH_AUX_SIZE		0x20
#define PCI_EP_DMA_METADATA_CH_AUX_ADDR_LO	0x24
#define PCI_EP_DMA_METADATA_CH_AUX_ADDR_HI	0x28

#endif /* __LINUX_PCI_EP_DMA_H */
