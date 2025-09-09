// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Durai Manickam KR <durai.manickamkr@microchip.com>
 *
 * Microchip SAMA7D65 I3C HCI Master driver
 */

#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/i3c/master.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>

/*
 * Microchip Host Controller Capabilities and Operation Registers
 */

#define MCHP_HCI_VERSION			0x00	/* HCI Version (in BCD) */

#define MCHP_HC_CONTROL				0x04
#define MCHP_HC_CONTROL_BUS_ENABLE		BIT(31)
#define MCHP_HC_CONTROL_RESUME			BIT(30)
#define MCHP_HC_CONTROL_ABORT			BIT(29)
#define MCHP_HC_CONTROL_HOT_JOIN_CTRL		BIT(8)	/* Hot-Join ACK/NACK Control */
#define MCHP_HC_CONTROL_I2C_TARGET_PRESENT	BIT(7)
#define MCHP_HC_CONTROL_IBA_INCLUDE		BIT(0)	/* Include I3C Broadcast Address */

#define MCHP_MASTER_DEVICE_ADDR			0x08	/* Master Device Address */
#define MCHP_MASTER_DYNAMIC_ADDR_VALID		BIT(31)	/* Dynamic Address is Valid */
#define MCHP_MASTER_DYNAMIC_ADDR(v)		FIELD_PREP(GENMASK(22, 16), v)

#define MCHP_HC_CAPABILITIES			0x0c
#define MCHP_HC_CAP_HDR_TS_EN			BIT(7)
#define MCHP_HC_CAP_HDR_DDR_EN			BIT(6)
#define MCHP_HC_CAP_NON_CURRENT_MASTER_CAP	BIT(5)	/* master handoff capable */
#define MCHP_HC_CAP_AUTO_COMMAND		BIT(3)
#define MCHP_HC_CAP_COMBO_COMMAND		BIT(2)

#define MCHP_RESET_CONTROL			0x10
#define MCHP_IBI_QUEUE_RST			BIT(5)
#define MCHP_RX_FIFO_RST			BIT(4)
#define MCHP_TX_FIFO_RST			BIT(3)
#define MCHP_RESP_QUEUE_RST			BIT(2)
#define MCHP_CMD_QUEUE_RST			BIT(1)
#define MCHP_SOFT_RST				BIT(0)	/* Core Reset */

#define MCHP_PRESENT_STATE			0x14
#define MCHp_STATE_CURRENT_MASTER		BIT(2)

#define MCHP_INTR_STATUS			0x20
#define MCHP_INTR_STATUS_ENABLE			0x24
#define MCHP_INTR_SIGNAL_ENABLE			0x28
#define MCHP_INTR_FORCE				0x2c
#define MCHP_INTR_HC_INTERNAL_ERR		BIT(10)	/* HC Internal Error */

#define MCHP_DAT_SECTION			0x30	/* Device Address Table */
#define MCHP_DAT_TABLE_SIZE			GENMASK(17, 12)
#define MCHP_DAT_TABLE_OFFSET			GENMASK(11, 0)

#define MCHP_DCT_SECTION			0x34	/* Device Characteristics Table */
#define MCHP_DCT_TABLE_INDEX			GENMASK(21, 19)
#define MCHP_DCT_TABLE_SIZE			GENMASK(18, 12)
#define MCHP_DCT_TABLE_OFFSET			GENMASK(11, 0)

#define MCHP_RING_HEADERS_SECTION		0x38
#define MCHP_RING_HEADERS_OFFSET		GENMASK(15, 0)

#define MCHP_PIO_SECTION			0x3c
#define MCHP_PIO_REGS_OFFSET			GENMASK(15, 0)	/* PIO Offset */

#define MCHP_EXT_CAPS_SECTION			0x40
#define MCHP_EXT_CAPS_OFFSET			GENMASK(15, 0)

#define MCHP_IBI_NOTIFY_CTRL			0x58	/* IBI Notify Control */
#define MCHP_IBI_NOTIFY_SIR_REJECTED		BIT(3)	/* Rejected Target Interrupt Request */
#define MCHP_IBI_NOTIFY_MR_REJECTED		BIT(1)	/* Rejected Master Request Control */
#define MCHP_IBI_NOTIFY_HJ_REJECTED		BIT(0)	/* Rejected Hot-Join Control */

#define DEV_CTX_BASE_LO			0x60
#define DEV_CTX_BASE_HI			0x64

/*
 * PIO Access Area
 */

#define pio_reg_read(r)         readl(hci->PIO_regs + (PIO_##r))
#define pio_reg_write(r, v)     writel(v, hci->PIO_regs + (PIO_##r))

#define PIO_MCHP_COMMAND_QUEUE_PORT          0x00
#define PIO_MCHP_RESPONSE_QUEUE_PORT         0x04
#define PIO_MCHP_XFER_DATA_PORT              0x08
#define PIO_MCHP_IBI_PORT                            0x0c

#define PIO_MCHP_QUEUE_THLD_CTRL             0x10
#define MCHP_QUEUE_IBI_STATUS_THLD           GENMASK(31, 24)
#define MCHP_QUEUE_IBI_DATA_THLD             GENMASK(23, 16)
#define MCHP_QUEUE_RESP_BUF_THLD             GENMASK(15, 8)
#define MCHP_QUEUE_CMD_EMPTY_BUF_THLD        GENMASK(7, 0)

#define PIO_MCHP_DATA_BUFFER_THLD_CTRL       0x14
#define MCHP_DATA_RX_START_THLD              GENMASK(26, 24)
#define MCHP_DATA_TX_START_THLD              GENMASK(18, 16)
#define MCHP_DATA_RX_BUF_THLD                GENMASK(10, 8)
#define MCHP_DATA_TX_BUF_THLD                GENMASK(2, 0)

#define PIO_MCHP_QUEUE_SIZE                  0x18
#define MCHP_TX_DATA_BUFFER_SIZE             GENMASK(31, 24)
#define MCHP_RX_DATA_BUFFER_SIZE             GENMASK(23, 16)
#define MCHP_IBI_STATUS_SIZE                 GENMASK(15, 8)
#define MCHP_CR_QUEUE_SIZE                   GENMASK(7, 0)

#define PIO_MCHP_INTR_STATUS                 0x20
#define PIO_MCHP_INTR_STATUS_ENABLE          0x24
#define PIO_MCHP_INTR_SIGNAL_ENABLE          0x28
#define PIO_MCHP_INTR_FORCE                  0x2c
#define STAT_TRANSFER_BLOCKED                BIT(25)
#define STAT_PERR_RESP_UFLOW                 BIT(24)
#define STAT_PERR_CMD_OFLOW                  BIT(23)
#define STAT_PERR_IBI_UFLOW                  BIT(22)
#define STAT_PERR_RX_UFLOW                   BIT(21)
#define STAT_PERR_TX_OFLOW                   BIT(20)
#define STAT_ERR_RESP_QUEUE_FULL             BIT(19)
#define STAT_WARN_RESP_QUEUE_FULL            BIT(18)
#define STAT_ERR_IBI_QUEUE_FULL              BIT(17)
#define STAT_WARN_IBI_QUEUE_FULL             BIT(16)
#define STAT_ERR_RX_DATA_FULL                BIT(15)
#define STAT_WARN_RX_DATA_FULL               BIT(14)
#define STAT_ERR_TX_DATA_EMPTY               BIT(13)
#define STAT_WARN_TX_DATA_EMPTY              BIT(12)
#define STAT_TRANSFER_ERR                    BIT(9)
#define STAT_WARN_INS_STOP_MODE              BIT(7)
#define STAT_TRANSFER_ABORT                  BIT(5)
#define STAT_RESP_READY                      BIT(4)
#define STAT_CMD_QUEUE_READY                 BIT(3)
#define STAT_IBI_STATUS_THLD                 BIT(2)
#define STAT_RX_THLD                         BIT(1)
#define STAT_TX_THLD                         BIT(0)

#define PIO_MCHP_QUEUE_CUR_STATUS            0x38
#define MCHP_CUR_IBI_Q_LEVEL                 GENMASK(23, 16)
#define MCHP_CUR_RESP_Q_LEVEL                GENMASK(15, 8)
#define MCHP_CUR_CMD_Q_EMPTY_LEVEL           GENMASK(7, 0)

#define PIO_MCHP_DATA_BUFFER_CUR_STATUS      0x3c
#define MCHP_CUR_RX_BUF_LVL                  GENMASK(15, 8)
#define MCHP_CUR_TX_BUF_LVL                  GENMASK(7, 0)

/*
 * Handy status bit combinations
 */

#define STAT_LATENCY_WARNINGS           (STAT_WARN_RESP_QUEUE_FULL | \
                                         STAT_WARN_IBI_QUEUE_FULL | \
                                         STAT_WARN_RX_DATA_FULL | \
                                         STAT_WARN_TX_DATA_EMPTY | \
                                         STAT_WARN_INS_STOP_MODE)

#define STAT_LATENCY_ERRORS             (STAT_ERR_RESP_QUEUE_FULL | \
                                         STAT_ERR_IBI_QUEUE_FULL | \
                                         STAT_ERR_RX_DATA_FULL | \
                                         STAT_ERR_TX_DATA_EMPTY)

#define STAT_PROG_ERRORS                (STAT_TRANSFER_BLOCKED | \
                                         STAT_PERR_RESP_UFLOW | \
                                         STAT_PERR_CMD_OFLOW | \
                                         STAT_PERR_IBI_UFLOW | \
                                         STAT_PERR_RX_UFLOW | \
                                         STAT_PERR_TX_OFLOW)

#define STAT_ALL_ERRORS                 (STAT_TRANSFER_ABORT | \
                                         STAT_TRANSFER_ERR | \
                                         STAT_LATENCY_ERRORS | \
                                         STAT_PROG_ERRORS)

/*
 * Address Assignment Command
 */

#define CMD_0_ATTR_A                    FIELD_PREP(CMD_0_ATTR, 0x2)

#define CMD_A0_TOC                                 W0_BIT_(31)
#define CMD_A0_ROC                                 W0_BIT_(30)
#define CMD_A0_DEV_COUNT(v)             FIELD_PREP(W0_MASK(29, 26), v)
#define CMD_A0_DEV_INDEX(v)             FIELD_PREP(W0_MASK(20, 16), v)
#define CMD_A0_CMD(v)                   FIELD_PREP(W0_MASK(14,  7), v)
#define CMD_A0_TID(v)                   FIELD_PREP(W0_MASK( 6,  3), v)

/*
 * Immediate Data Transfer Command
 */

#define CMD_0_ATTR_I                    FIELD_PREP(CMD_0_ATTR, 0x1)

#define CMD_I1_DATA_BYTE_4(v)           FIELD_PREP(W1_MASK(63, 56), v)
#define CMD_I1_DATA_BYTE_3(v)           FIELD_PREP(W1_MASK(55, 48), v)
#define CMD_I1_DATA_BYTE_2(v)           FIELD_PREP(W1_MASK(47, 40), v)
#define CMD_I1_DATA_BYTE_1(v)           FIELD_PREP(W1_MASK(39, 32), v)
#define CMD_I1_DEF_BYTE(v)              FIELD_PREP(W1_MASK(39, 32), v)
#define CMD_I0_TOC                                 W0_BIT_(31)
#define CMD_I0_ROC                                 W0_BIT_(30)
#define CMD_I0_RNW                                 W0_BIT_(29)
#define CMD_I0_MODE(v)                  FIELD_PREP(W0_MASK(28, 26), v)
#define CMD_I0_DTT(v)                   FIELD_PREP(W0_MASK(25, 23), v)
#define CMD_I0_DEV_INDEX(v)             FIELD_PREP(W0_MASK(20, 16), v)
#define CMD_I0_CP                                  W0_BIT_(15)
#define CMD_I0_CMD(v)                   FIELD_PREP(W0_MASK(14,  7), v)
#define CMD_I0_TID(v)                   FIELD_PREP(W0_MASK( 6,  3), v)

/*
 * Regular Data Transfer Command
 */

#define CMD_0_ATTR_R                    FIELD_PREP(CMD_0_ATTR, 0x0)

#define CMD_R1_DATA_LENGTH(v)           FIELD_PREP(W1_MASK(63, 48), v)
#define CMD_R1_DEF_BYTE(v)              FIELD_PREP(W1_MASK(39, 32), v)
#define CMD_R0_TOC                                 W0_BIT_(31)
#define CMD_R0_ROC                                 W0_BIT_(30)
#define CMD_R0_RNW                                 W0_BIT_(29)
#define CMD_R0_MODE(v)                  FIELD_PREP(W0_MASK(28, 26), v)
#define CMD_R0_DBP                                 W0_BIT_(25)
#define CMD_R0_DEV_INDEX(v)             FIELD_PREP(W0_MASK(20, 16), v)
#define CMD_R0_CP                                  W0_BIT_(15)
#define CMD_R0_CMD(v)                   FIELD_PREP(W0_MASK(14,  7), v)
#define CMD_R0_TID(v)                   FIELD_PREP(W0_MASK( 6,  3), v)

/*
 * Combo Transfer (Write + Write/Read) Command
 */

#define CMD_0_ATTR_C                    FIELD_PREP(CMD_0_ATTR, 0x3)

#define CMD_C1_DATA_LENGTH(v)           FIELD_PREP(W1_MASK(63, 48), v)
#define CMD_C1_OFFSET(v)                FIELD_PREP(W1_MASK(47, 32), v)
#define CMD_C0_TOC                                 W0_BIT_(31)
#define CMD_C0_ROC                                 W0_BIT_(30)
#define CMD_C0_RNW                                 W0_BIT_(29)
#define CMD_C0_MODE(v)                  FIELD_PREP(W0_MASK(28, 26), v)
#define CMD_C0_16_BIT_SUBOFFSET                    W0_BIT_(25)
#define CMD_C0_FIRST_PHASE_MODE                    W0_BIT_(24)
#define CMD_C0_DATA_LENGTH_POSITION(v)  FIELD_PREP(W0_MASK(23, 22), v)
#define CMD_C0_DEV_INDEX(v)             FIELD_PREP(W0_MASK(20, 16), v)
#define CMD_C0_CP                                  W0_BIT_(15)
#define CMD_C0_CMD(v)                   FIELD_PREP(W0_MASK(14,  7), v)
#define CMD_C0_TID(v)                   FIELD_PREP(W0_MASK( 6,  3), v)

/*
 * Internal Control Command
 */

#define CMD_0_ATTR_M                    FIELD_PREP(CMD_0_ATTR, 0x7)

#define CMD_M1_VENDOR_SPECIFIC                     W1_MASK(63, 32)
#define CMD_M0_MIPI_RESERVED                       W0_MASK(31, 12)
#define CMD_M0_MIPI_CMD                            W0_MASK(11,  8)
#define CMD_M0_VENDOR_INFO_PRESENT                 W0_BIT_( 7)
#define CMD_M0_TID(v)                   FIELD_PREP(W0_MASK( 6,  3), v)

/*
 * Device Address Table Structure
 */

#define DAT_1_AUTOCMD_HDR_CODE          W1_MASK(58, 51)
#define DAT_1_AUTOCMD_MODE              W1_MASK(50, 48)
#define DAT_1_AUTOCMD_VALUE             W1_MASK(47, 40)
#define DAT_1_AUTOCMD_MASK              W1_MASK(39, 32)
/*      DAT_0_I2C_DEVICE                W0_BIT_(31) */
#define DAT_0_DEV_NACK_RETRY_CNT        W0_MASK(30, 29)
#define DAT_0_RING_ID                   W0_MASK(28, 26)
#define DAT_0_DYNADDR_PARITY            W0_BIT_(23)
#define DAT_0_DYNAMIC_ADDRESS           W0_MASK(22, 16)
#define DAT_0_TS                        W0_BIT_(15)
#define DAT_0_MR_REJECT                 W0_BIT_(14)
/*      DAT_0_SIR_REJECT                W0_BIT_(13) */
/*      DAT_0_IBI_PAYLOAD               W0_BIT_(12) */
#define DAT_0_STATIC_ADDRESS            W0_MASK(6, 0)

#define dat_w0_read(i)          readl(hci->DAT_regs + (i) * 8)
#define dat_w1_read(i)          readl(hci->DAT_regs + (i) * 8 + 4)
#define dat_w0_write(i, v)      writel(v, hci->DAT_regs + (i) * 8)
#define dat_w1_write(i, v)      writel(v, hci->DAT_regs + (i) * 8 + 4)

/* Global DAT flags */
#define DAT_0_I2C_DEVICE                W0_BIT_(31)
#define DAT_0_SIR_REJECT                W0_BIT_(13)
#define DAT_0_IBI_PAYLOAD               W0_BIT_(12)

/* 32-bit word aware bit and mask macros */
#define W0_MASK(h, l)  GENMASK((h) - 0,  (l) - 0)
#define W1_MASK(h, l)  GENMASK((h) - 32, (l) - 32)
#define W2_MASK(h, l)  GENMASK((h) - 64, (l) - 64)
#define W3_MASK(h, l)  GENMASK((h) - 96, (l) - 96)

/* Same for single bit macros (trailing _ to align with W*_MASK width) */
#define W0_BIT_(x)  BIT((x) - 0)
#define W1_BIT_(x)  BIT((x) - 32)
#define W2_BIT_(x)  BIT((x) - 64)
#define W3_BIT_(x)  BIT((x) - 96)

#define reg_read(r)             readl(hci->base_regs + (r))
#define reg_write(r, v)         writel(v, hci->base_regs + (r))
#define reg_set(r, v)           reg_write(r, reg_read(r) | (v))
#define reg_clear(r, v)         reg_write(r, reg_read(r) & ~(v))

/*
 * Those bits are common to all descriptor formats and
 * may be manipulated by the core code.
 */
#define CMD_0_TOC                       W0_BIT_(31)
#define CMD_0_ROC                       W0_BIT_(30)
#define CMD_0_ATTR                      W0_MASK(2, 0)

/*
 * Response Descriptor Structure
 */
#define RESP_STATUS(resp)               FIELD_GET(GENMASK(31, 28), resp)
#define RESP_TID(resp)                  FIELD_GET(GENMASK(27, 24), resp)
#define RESP_DATA_LENGTH(resp)          FIELD_GET(GENMASK(15,  0), resp) /* Response Data length as per Microchip IP */
#define RESP_ERR_FIELD                  GENMASK(31, 28)

/*
 * IBI Status Descriptor bits
 */
#define MCHP_IBI_STS                         BIT(31)
#define MCHP_IBI_ERROR                       BIT(30)
#define MCHP_IBI_STATUS_TYPE                 BIT(29)
#define MCHP_IBI_HW_CONTEXT                  GENMASK(28, 26)
#define MCHP_IBI_TS                          BIT(25)
#define MCHP_IBI_LAST_STATUS                 BIT(24)
#define MCHP_IBI_CHUNKS                      GENMASK(23, 16)
#define MCHP_IBI_ID                          GENMASK(15, 8)
#define MCHP_IBI_TARGET_ADDR                 GENMASK(15, 9)
#define MCHP_IBI_TARGET_RNW                  BIT(8)
#define MCHP_IBI_DATA_LENGTH                 GENMASK(7, 0)

/*
 * Master Data Transfer Rate Table Mode ID values.
 */
#define XFERRATE_MODE_I3C               0x00
#define XFERRATE_MODE_I2C               0x08

/*
 * Master Data Transfer Rate Table Entry Bits Definitions
 */
#define XFERRATE_MODE_ID                GENMASK(31, 28)
#define XFERRATE_RATE_ID                GENMASK(22, 20)
#define XFERRATE_ACTUAL_RATE_KHZ        GENMASK(19, 0)

/* Extended Capability Header */
#define CAP_HEADER_LENGTH               GENMASK(23, 8)
#define CAP_HEADER_ID                   GENMASK(7, 0)
/*ext_caps.c END */

/* list of quirks */
#define HCI_QUIRK_RAW_CCC               BIT(1)  /* CCC framing must be explicit */
#define MCHP_HCI_QUIRK_PIO_MODE         BIT(2)  /* Set PIO mode for Microchip platforms */
#define MCHP_HCI_QUIRK_OD_PP_TIMING     BIT(3)  /* Set OD and PP timings for Microchip platforms */
#define MCHP_HCI_QUIRK_RESP_BUF_THLD    BIT(4)  /* Set resp buf thld to 0 for Microchip platforms */

/* Timing registers */
#define MCHP_HCI_SCL_I3C_OD_TIMING          0x214
#define MCHP_HCI_SCL_I3C_PP_TIMING          0x218
#define MCHP_HCI_SDA_HOLD_SWITCH_DLY_TIMING 0x230

/* Timing values to configure 9MHz frequency */
#define MCHP_SCL_I3C_OD_TIMING          0x00cf00cf /* check and update correct values later */
#define MCHP_SCL_I3C_PP_TIMING          0x00160016

#define MCHP_QUEUE_THLD_CTRL                0xD0

/* TID generation (4 bits wide in all cases) */
#define mchp_hci_get_tid(bits) \
        (atomic_inc_return_relaxed(&hci->next_cmd_tid) % (1U << 4))

#define EXT_CAP(_id, _highest_mandatory_reg_offset, _parser) \
        { .id = (_id), .parser = (_parser), \
          .min_length = (_highest_mandatory_reg_offset)/4 + 1 }

/* Our main structure */
struct mchp_i3c_hci {
        struct i3c_master_controller master;
        struct clk *pclk;
        struct clk *gclk;
        void __iomem *base_regs;
        void __iomem *DAT_regs;
        void __iomem *DCT_regs;
        void __iomem *RHS_regs;
        void __iomem *PIO_regs;
        void __iomem *EXTCAPS_regs;
        void __iomem *AUTOCMD_regs;
        void __iomem *DEBUG_regs;
        const struct mchp_hci_io_ops *io;
        void *io_data;
        const struct mchp_hci_cmd_ops *cmd;
        atomic_t next_cmd_tid;
        u32 caps;
        unsigned int quirks;
        unsigned int DAT_entries;
        unsigned int DAT_entry_size;
        void *DAT_data;
        unsigned int DCT_entries;
        unsigned int DCT_entry_size;
        u8 version_major;
        u8 version_minor;
        u8 revision;
        u32 vendor_mipi_id;
        u32 vendor_version_id;
        u32 vendor_product_id;
        void *vendor_data;
};

/*
 * Structure to represent a master initiated transfer.
 * The rnw, data and data_len fields must be initialized before calling any
 * hci->cmd->*() method. The cmd method will initialize cmd_desc[] and
 * possibly modify (clear) the data field. Then xfer->cmd_desc[0] can
 * be augmented with CMD_0_ROC and/or CMD_0_TOC.
 * The completion field needs to be initialized before queueing with
 * hci->io->queue_xfer(), and requires CMD_0_ROC to be set.
 */
struct mchp_hci_xfer {
        u32 cmd_desc[4];
        u32 response;
        bool rnw;
        void *data;
        unsigned int data_len;
        unsigned int cmd_tid;
        struct completion *completion;
        union {
                struct {
                        /* PIO specific */
                        struct mchp_hci_xfer *next_xfer;
                        struct mchp_hci_xfer *next_data;
                        struct mchp_hci_xfer *next_resp;
                        unsigned int data_left;
                        u32 data_word_before_partial;
                };
                struct {
                        /* DMA specific */
                        dma_addr_t data_dma;
                        void *bounce_buf;
                        int ring_number;
                        int ring_entry;
                };
        };
};

struct mchp_hci_dat_ops {
        int (*init)(struct mchp_i3c_hci *hci);
        void (*cleanup)(struct mchp_i3c_hci *hci);
        int (*alloc_entry)(struct mchp_i3c_hci *hci);
        void (*free_entry)(struct mchp_i3c_hci *hci, unsigned int dat_idx);
        void (*set_dynamic_addr)(struct mchp_i3c_hci *hci, unsigned int dat_idx, u8 addr);
        void (*set_static_addr)(struct mchp_i3c_hci *hci, unsigned int dat_idx, u8 addr);
        void (*set_flags)(struct mchp_i3c_hci *hci, unsigned int dat_idx, u32 w0, u32 w1);
        void (*clear_flags)(struct mchp_i3c_hci *hci, unsigned int dat_idx, u32 w0, u32 w1);
        int (*get_index)(struct mchp_i3c_hci *hci, u8 address);
};

/* This abstracts PIO vs DMA operations */
struct mchp_hci_io_ops {
        bool (*irq_handler)(struct mchp_i3c_hci *hci, unsigned int mask);
        int (*queue_xfer)(struct mchp_i3c_hci *hci, struct mchp_hci_xfer *xfer, int n);
        bool (*dequeue_xfer)(struct mchp_i3c_hci *hci, struct mchp_hci_xfer *xfer, int n);
        int (*request_ibi)(struct mchp_i3c_hci *hci, struct i3c_dev_desc *dev,
                           const struct i3c_ibi_setup *req);
        void (*free_ibi)(struct mchp_i3c_hci *hci, struct i3c_dev_desc *dev);
        void (*recycle_ibi_slot)(struct mchp_i3c_hci *hci, struct i3c_dev_desc *dev,
                                struct i3c_ibi_slot *slot);
        int (*init)(struct mchp_i3c_hci *hci);
        void (*cleanup)(struct mchp_i3c_hci *hci);
};

/* Our per device master private data */
struct mchp_i3c_hci_dev_data {
        int dat_idx;
        void *ibi_data;
};

/* This abstracts operations with our command descriptor formats */
struct mchp_hci_cmd_ops {
        int (*prep_ccc)(struct mchp_i3c_hci *hci, struct mchp_hci_xfer *xfer,
                        u8 ccc_addr, u8 ccc_cmd, bool raw);
        void (*prep_i3c_xfer)(struct mchp_i3c_hci *hci, struct i3c_dev_desc *dev,
                              struct mchp_hci_xfer *xfer);
        void (*prep_i2c_xfer)(struct mchp_i3c_hci *hci, struct i2c_dev_desc *dev,
                              struct mchp_hci_xfer *xfer);
        int (*perform_daa)(struct mchp_i3c_hci *hci);
};

struct hci_ext_caps {
        u8  id;
        u16 min_length;
        int (*parser)(struct mchp_i3c_hci *hci, void __iomem *base);
};

struct hci_pio_dev_ibi_data {
        struct i3c_generic_ibi_pool *pool;
        unsigned int max_len;
};

struct hci_pio_ibi_data {
        struct i3c_ibi_slot *slot;
        void *data_ptr;
        unsigned int addr;
        unsigned int seg_len, seg_cnt;
        unsigned int max_len;
        bool last_seg;
};

struct mchp_hci_pio_data {
        spinlock_t lock;
        struct mchp_hci_xfer *curr_xfer, *xfer_queue;
        struct mchp_hci_xfer *curr_rx, *rx_queue;
        struct mchp_hci_xfer *curr_tx, *tx_queue;
        struct mchp_hci_xfer *curr_resp, *resp_queue;
        struct hci_pio_ibi_data ibi;
        unsigned int rx_thresh_size, tx_thresh_size;
        unsigned int max_ibi_thresh;
        u32 reg_queue_thresh;
        u32 enabled_irqs;
};

/* global functions */
static int i3c_hci_parse_ext_caps(struct mchp_i3c_hci *hci);
void mchp_i3c_hci_dct_get_val(struct mchp_i3c_hci *hci, unsigned int dct_idx,
                         u64 *pid, unsigned int *dcr, unsigned int *bcr);
static void mchp_hci_pio_err(struct mchp_i3c_hci *hci, struct mchp_hci_pio_data *pio,
                        u32 status);
void mchp_mipi_i3c_hci_resume(struct mchp_i3c_hci *hci);
void mchp_mipi_i3c_hci_pio_reset(struct mchp_i3c_hci *hci);
void mchp_mipi_i3c_hci_dct_index_reset(struct mchp_i3c_hci *hci);
void microchip_set_od_pp_timing(struct mchp_i3c_hci *hci);
void microchip_set_resp_buf_thld(struct mchp_i3c_hci *hci);

static inline struct mchp_hci_xfer *mchp_hci_alloc_xfer(unsigned int n)
{
        return kcalloc(n, sizeof(struct mchp_hci_xfer), GFP_KERNEL);
}

static inline void mchp_hci_free_xfer(struct mchp_hci_xfer *xfer, unsigned int n)
{
        kfree(xfer);
}

/* Data Transfer Speed and Mode */
enum mchp_hci_cmd_mode {
        MODE_I3C_SDR0           = 0x0,
        MODE_I3C_SDR1           = 0x1,
        MODE_I3C_SDR2           = 0x2,
        MODE_I3C_SDR3           = 0x3,
        MODE_I3C_SDR4           = 0x4,
        MODE_I3C_HDR_TSx        = 0x5,
        MODE_I3C_HDR_DDR        = 0x6,
        MODE_I3C_HDR_BT         = 0x7,
        MODE_I3C_Fm_FmP         = 0x8,
        MODE_I2C_Fm             = 0x0,
        MODE_I2C_FmP            = 0x1,
        MODE_I2C_UD1            = 0x2,
        MODE_I2C_UD2            = 0x3,
        MODE_I2C_UD3            = 0x4,
};

enum mchp_hci_resp_err {
        RESP_SUCCESS                    = 0x0,
        RESP_ERR_CRC                    = 0x1,
        RESP_ERR_PARITY                 = 0x2,
        RESP_ERR_FRAME                  = 0x3,
        RESP_ERR_ADDR_HEADER            = 0x4,
        RESP_ERR_BCAST_NACK_7E          = 0x4,
        RESP_ERR_NACK                   = 0x5,
        RESP_ERR_OVL                    = 0x6,
        RESP_ERR_I3C_SHORT_READ         = 0x7,
        RESP_ERR_HC_TERMINATED          = 0x8,
        RESP_ERR_I2C_WR_DATA_NACK       = 0x9,
        RESP_ERR_BUS_XFER_ABORTED       = 0x9,
        RESP_ERR_NOT_SUPPORTED          = 0xa,
        RESP_ERR_ABORTED_WITH_CRC       = 0xb,
        /* 0xc to 0xf are reserved for transfer specific errors */
};

/* Our various instances */
/*  handy helpers */
static inline struct i3c_dev_desc *
mchp_i3c_hci_addr_to_dev(struct mchp_i3c_hci *hci, unsigned int addr)
{
        struct i3c_bus *bus = i3c_master_get_bus(&hci->master);
        struct i3c_dev_desc *dev;

        i3c_bus_for_each_i3cdev(bus, dev) {
                if (dev->info.dyn_addr == addr)
                        return dev;
        }
        return NULL;
}

void microchip_set_od_pp_timing(struct mchp_i3c_hci *hci)
{
        u32 data;

        reg_write(MCHP_HCI_SCL_I3C_OD_TIMING, MCHP_SCL_I3C_OD_TIMING);
        reg_write(MCHP_HCI_SCL_I3C_PP_TIMING, MCHP_SCL_I3C_PP_TIMING);
        data = reg_read(MCHP_HCI_SDA_HOLD_SWITCH_DLY_TIMING);
        /* Configure maximum TX hold time */
        data |= W0_MASK(18, 16);
        reg_write(MCHP_HCI_SDA_HOLD_SWITCH_DLY_TIMING, data);
}

void microchip_set_resp_buf_thld(struct mchp_i3c_hci *hci)
{
        u32 data;

        data = reg_read(MCHP_QUEUE_THLD_CTRL);
        data = data & ~W0_MASK(15, 8);
        reg_write(MCHP_QUEUE_THLD_CTRL, data);
}

static int hci_extcap_hardware_id(struct mchp_i3c_hci *hci, void __iomem *base)
{
        hci->vendor_mipi_id     = readl(base + 0x04);
        hci->vendor_version_id  = readl(base + 0x08);
        hci->vendor_product_id  = readl(base + 0x0c);

        dev_info(&hci->master.dev, "vendor MIPI ID: %#x\n", hci->vendor_mipi_id);
        dev_info(&hci->master.dev, "vendor version ID: %#x\n", hci->vendor_version_id);
        dev_info(&hci->master.dev, "vendor product ID: %#x\n", hci->vendor_product_id);

        return 0;
}

static int hci_extcap_master_config(struct mchp_i3c_hci *hci, void __iomem *base)
{
        u32 master_config = readl(base + 0x04);
        unsigned int operation_mode = FIELD_GET(GENMASK(5, 4), master_config);
        static const char * const functionality[] = {
                "(unknown)", "master only", "target only",
                "primary/secondary master" };
        dev_info(&hci->master.dev, "operation mode: %s\n", functionality[operation_mode]);
        if (operation_mode & 0x1)
                return 0;
        dev_err(&hci->master.dev, "only master mode is currently supported\n");
        return -EOPNOTSUPP;
}

static int hci_extcap_debug(struct mchp_i3c_hci *hci, void __iomem *base)
{
        dev_info(&hci->master.dev, "debug registers present\n");
        hci->DEBUG_regs = base;
        return 0;
}

static const struct hci_ext_caps ext_capabilities[] = {
        EXT_CAP(0x01, 0x0c, hci_extcap_hardware_id),
        EXT_CAP(0x02, 0x04, hci_extcap_master_config),
        EXT_CAP(0x0c, 0x10, hci_extcap_debug),
};

static int i3c_hci_parse_ext_caps(struct mchp_i3c_hci *hci)
{
        void __iomem *curr_cap = hci->EXTCAPS_regs;
        void __iomem *end = curr_cap + 0x1000; /* some arbitrary limit */
        u32 cap_header, cap_id, cap_length;
        const struct hci_ext_caps *cap_entry;
        int i, err = 0;

        if (!curr_cap)
                return 0;

        for (; !err && curr_cap < end; curr_cap += cap_length * 4) {
                cap_header = readl(curr_cap);
                cap_id = FIELD_GET(CAP_HEADER_ID, cap_header);
                cap_length = FIELD_GET(CAP_HEADER_LENGTH, cap_header);
                dev_dbg(&hci->master.dev,"id=0x%02x length=%d", cap_id, cap_length);
		if (!cap_length)
                        break;
                if (curr_cap + cap_length * 4 >= end) {
                        dev_err(&hci->master.dev,
                                "ext_cap 0x%02x has size %d (too big)\n",
                                cap_id, cap_length);
                        err = -EINVAL;
                        break;
                }
                cap_entry = NULL;
                for (i = 0; i < ARRAY_SIZE(ext_capabilities); i++) {
                        if (ext_capabilities[i].id == cap_id) {
                                cap_entry = &ext_capabilities[i];
                                break;
                        }
                }
                if (!cap_entry) {
                        dev_notice(&hci->master.dev,
                                   "unknown ext_cap 0x%02x\n", cap_id);
                } else if (cap_length < cap_entry->min_length) {
                        dev_err(&hci->master.dev,
                                "ext_cap 0x%02x has size %d (expecting >= %d)\n",
                                cap_id, cap_length, cap_entry->min_length);
                        err = -EINVAL;
                } else {
                        err = cap_entry->parser(hci, curr_cap);
                }
	}
        return err;
}

static inline bool dynaddr_parity(unsigned int addr)
{
        addr |= 1 << 7;
        addr += addr >> 4;
        addr += addr >> 2;
        addr += addr >> 1;
        return (addr & 1);
}

static int mchp_hci_dat_v1_init(struct mchp_i3c_hci *hci)
{
        unsigned int dat_idx;

        if (!hci->DAT_regs) {
                dev_err(&hci->master.dev,
                        "only DAT in register space is supported at the moment\n");
                return -EOPNOTSUPP;
        }
        if (hci->DAT_entry_size != 8) {
                dev_err(&hci->master.dev,
                        "only 8-bytes DAT entries are supported at the moment\n");
                return -EOPNOTSUPP;
        }

        if (!hci->DAT_data) {
                /* use a bitmap for faster free slot search */
                hci->DAT_data = bitmap_zalloc(hci->DAT_entries, GFP_KERNEL);
                if (!hci->DAT_data)
                        return -ENOMEM;

                /* clear them */
                for (dat_idx = 0; dat_idx < hci->DAT_entries; dat_idx++) {
                        dat_w0_write(dat_idx, 0);
                        dat_w1_write(dat_idx, 0);
                }
        }

        return 0;
}

static void mchp_hci_dat_v1_cleanup(struct mchp_i3c_hci *hci)
{
        bitmap_free(hci->DAT_data);
        hci->DAT_data = NULL;
}

static int mchp_hci_dat_v1_alloc_entry(struct mchp_i3c_hci *hci)
{
        unsigned int dat_idx;
        int ret;

        if (!hci->DAT_data) {
                ret = mchp_hci_dat_v1_init(hci);
                if (ret)
                        return ret;
        }
        dat_idx = find_first_zero_bit(hci->DAT_data, hci->DAT_entries);
        if (dat_idx >= hci->DAT_entries)
                return -ENOENT;
        __set_bit(dat_idx, hci->DAT_data);

        /* default flags */
        dat_w0_write(dat_idx, DAT_0_SIR_REJECT | DAT_0_MR_REJECT);

        return dat_idx;
}

static void mchp_hci_dat_v1_free_entry(struct mchp_i3c_hci *hci, unsigned int dat_idx)
{
        dat_w0_write(dat_idx, 0);
        dat_w1_write(dat_idx, 0);
        if (hci->DAT_data)
                __clear_bit(dat_idx, hci->DAT_data);
}

static void mchp_hci_dat_v1_set_dynamic_addr(struct mchp_i3c_hci *hci,
                                        unsigned int dat_idx, u8 address)
{
        u32 dat_w0;

        dat_w0 = dat_w0_read(dat_idx);
        dat_w0 &= ~(DAT_0_DYNAMIC_ADDRESS | DAT_0_DYNADDR_PARITY);
        dat_w0 |= FIELD_PREP(DAT_0_DYNAMIC_ADDRESS, address) |
                  (dynaddr_parity(address) ? DAT_0_DYNADDR_PARITY : 0);
        dat_w0_write(dat_idx, dat_w0);
}

static void mchp_hci_dat_v1_set_static_addr(struct mchp_i3c_hci *hci,
                                       unsigned int dat_idx, u8 address)
{
        u32 dat_w0;

        dat_w0 = dat_w0_read(dat_idx);
        dat_w0 &= ~DAT_0_STATIC_ADDRESS;
        dat_w0 |= FIELD_PREP(DAT_0_STATIC_ADDRESS, address);
        dat_w0_write(dat_idx, dat_w0);
}

static void mchp_hci_dat_v1_set_flags(struct mchp_i3c_hci *hci, unsigned int dat_idx,
                                 u32 w0_flags, u32 w1_flags)
{
        u32 dat_w0, dat_w1;

        dat_w0 = dat_w0_read(dat_idx);
        dat_w1 = dat_w1_read(dat_idx);
        dat_w0 |= w0_flags;
        dat_w1 |= w1_flags;
        dat_w0_write(dat_idx, dat_w0);
        dat_w1_write(dat_idx, dat_w1);
}

static void mchp_hci_dat_v1_clear_flags(struct mchp_i3c_hci *hci, unsigned int dat_idx,
                                   u32 w0_flags, u32 w1_flags)
{
        u32 dat_w0, dat_w1;

        dat_w0 = dat_w0_read(dat_idx);
        dat_w1 = dat_w1_read(dat_idx);
        dat_w0 &= ~w0_flags;
        dat_w1 &= ~w1_flags;
        dat_w0_write(dat_idx, dat_w0);
        dat_w1_write(dat_idx, dat_w1);
}

static int mchp_hci_dat_v1_get_index(struct mchp_i3c_hci *hci, u8 dev_addr)
{
        unsigned int dat_idx;
        u32 dat_w0;

        for_each_set_bit(dat_idx, hci->DAT_data, hci->DAT_entries) {
                dat_w0 = dat_w0_read(dat_idx);
                if (FIELD_GET(DAT_0_DYNAMIC_ADDRESS, dat_w0) == dev_addr)
                        return dat_idx;
        }

        return -ENODEV;
}

const struct mchp_hci_dat_ops mchp_mipi_i3c_hci_dat_v1 = {
        .init                   = mchp_hci_dat_v1_init,
        .cleanup                = mchp_hci_dat_v1_cleanup,
        .alloc_entry            = mchp_hci_dat_v1_alloc_entry,
        .free_entry             = mchp_hci_dat_v1_free_entry,
        .set_dynamic_addr       = mchp_hci_dat_v1_set_dynamic_addr,
        .set_static_addr        = mchp_hci_dat_v1_set_static_addr,
        .set_flags              = mchp_hci_dat_v1_set_flags,
        .clear_flags            = mchp_hci_dat_v1_clear_flags,
        .get_index              = mchp_hci_dat_v1_get_index,
};

/*
 * Device Characteristic Table
 */

void mchp_i3c_hci_dct_get_val(struct mchp_i3c_hci *hci, unsigned int dct_idx,
                         u64 *pid, unsigned int *dcr, unsigned int *bcr)
{
        void __iomem *reg = hci->DCT_regs + dct_idx * 4 * 4;
        u32 dct_entry_data[4];
        unsigned int i;

        for (i = 0; i < 4; i++) {
                dct_entry_data[i] = readl(reg);
                reg += 4;
        }

        *pid = ((u64)dct_entry_data[0]) << (47 - 32 + 1) |
               FIELD_GET(W1_MASK(47, 32), dct_entry_data[1]);
        *dcr = FIELD_GET(W2_MASK(71, 64), dct_entry_data[2]);
        *bcr = FIELD_GET(W2_MASK(79, 72), dct_entry_data[2]);
}

static enum mchp_hci_cmd_mode mchp_get_i3c_mode(struct mchp_i3c_hci *hci)
{
        struct i3c_bus *bus = i3c_master_get_bus(&hci->master);

        if (bus->scl_rate.i3c > 8000000)
                return MODE_I3C_SDR0;
        if (bus->scl_rate.i3c > 6000000)
                return MODE_I3C_SDR1;
        if (bus->scl_rate.i3c > 4000000)
                return MODE_I3C_SDR2;
        if (bus->scl_rate.i3c > 2000000)
                return MODE_I3C_SDR3;
        return MODE_I3C_SDR4;
}

static enum mchp_hci_cmd_mode mchp_get_i2c_mode(struct mchp_i3c_hci *hci)
{
        struct i3c_bus *bus = i3c_master_get_bus(&hci->master);

        if (bus->scl_rate.i2c >= 1000000)
                return MODE_I2C_FmP;
        return MODE_I2C_Fm;
}

static void mchp_fill_data_bytes(struct mchp_hci_xfer *xfer, u8 *data,
                            unsigned int data_len)
{
        xfer->cmd_desc[1] = 0;
        switch (data_len) {
        case 4:
                xfer->cmd_desc[1] |= CMD_I1_DATA_BYTE_4(data[3]);
                fallthrough;
        case 3:
                xfer->cmd_desc[1] |= CMD_I1_DATA_BYTE_3(data[2]);
                fallthrough;
        case 2:
                xfer->cmd_desc[1] |= CMD_I1_DATA_BYTE_2(data[1]);
                fallthrough;
        case 1:
                xfer->cmd_desc[1] |= CMD_I1_DATA_BYTE_1(data[0]);
                fallthrough;
        case 0:
                break;
        }
        /* we consumed all the data with the cmd descriptor */
        xfer->data = NULL;
}

static int mchp_hci_cmd_v1_prep_ccc(struct mchp_i3c_hci *hci,
                               struct mchp_hci_xfer *xfer,
                               u8 ccc_addr, u8 ccc_cmd, bool raw)
{
        unsigned int dat_idx = 0;
        enum mchp_hci_cmd_mode mode = mchp_get_i3c_mode(hci);
        u8 *data = xfer->data;
        unsigned int data_len = xfer->data_len;
        bool rnw = xfer->rnw;
        int ret;

        /* this should never happen */
        if (WARN_ON(raw))
                return -EINVAL;

        if (ccc_addr != I3C_BROADCAST_ADDR) {
                ret = mchp_mipi_i3c_hci_dat_v1.get_index(hci, ccc_addr);
                if (ret < 0)
                        return ret;
                dat_idx = ret;
        }

        xfer->cmd_tid = mchp_hci_get_tid();

        if (!rnw && data_len <= 4) {
                /* we use an Immediate Data Transfer Command */
                xfer->cmd_desc[0] =
                        CMD_0_ATTR_I |
                        CMD_I0_TID(xfer->cmd_tid) |
                        CMD_I0_CMD(ccc_cmd) | CMD_I0_CP |
                        CMD_I0_DEV_INDEX(dat_idx) |
                        CMD_I0_DTT(data_len) |
                        CMD_I0_MODE(mode);
                mchp_fill_data_bytes(xfer, data, data_len);
        } else {
                /* we use a Regular Data Transfer Command */
                xfer->cmd_desc[0] =
                        CMD_0_ATTR_R |
                        CMD_R0_TID(xfer->cmd_tid) |
                        CMD_R0_CMD(ccc_cmd) | CMD_R0_CP |
                        CMD_R0_DEV_INDEX(dat_idx) |
                        CMD_R0_MODE(mode) |
                        (rnw ? CMD_R0_RNW : 0);
                xfer->cmd_desc[1] =
                        CMD_R1_DATA_LENGTH(data_len);
        }

        return 0;
}

static void mchp_hci_cmd_v1_prep_i3c_xfer(struct mchp_i3c_hci *hci,
                                     struct i3c_dev_desc *dev,
                                     struct mchp_hci_xfer *xfer)
{
        struct mchp_i3c_hci_dev_data *dev_data = i3c_dev_get_master_data(dev);
        unsigned int dat_idx = dev_data->dat_idx;
        enum mchp_hci_cmd_mode mode = mchp_get_i3c_mode(hci);
        u8 *data = xfer->data;
        unsigned int data_len = xfer->data_len;
        bool rnw = xfer->rnw;

        xfer->cmd_tid = mchp_hci_get_tid();

        if (!rnw && data_len <= 4) {
                /* we use an Immediate Data Transfer Command */
                xfer->cmd_desc[0] =
                        CMD_0_ATTR_I |
                        CMD_I0_TID(xfer->cmd_tid) |
                        CMD_I0_DEV_INDEX(dat_idx) |
                        CMD_I0_DTT(data_len) |
                        CMD_I0_MODE(mode);
                mchp_fill_data_bytes(xfer, data, data_len);
        } else {
                /* we use a Regular Data Transfer Command */
                xfer->cmd_desc[0] =
                        CMD_0_ATTR_R |
                        CMD_R0_TID(xfer->cmd_tid) |
                        CMD_R0_DEV_INDEX(dat_idx) |
                        CMD_R0_MODE(mode) |
                        (rnw ? CMD_R0_RNW : 0);
                xfer->cmd_desc[1] =
                        CMD_R1_DATA_LENGTH(data_len);
        }
}

static void mchp_hci_cmd_v1_prep_i2c_xfer(struct mchp_i3c_hci *hci,
                                     struct i2c_dev_desc *dev,
                                     struct mchp_hci_xfer *xfer)
{
        struct mchp_i3c_hci_dev_data *dev_data = i2c_dev_get_master_data(dev);
        unsigned int dat_idx = dev_data->dat_idx;
        enum mchp_hci_cmd_mode mode = mchp_get_i2c_mode(hci);
        u8 *data = xfer->data;
        unsigned int data_len = xfer->data_len;
        bool rnw = xfer->rnw;

        xfer->cmd_tid = mchp_hci_get_tid();

        if (!rnw && data_len <= 4) {
                /* we use an Immediate Data Transfer Command */
                xfer->cmd_desc[0] =
                        CMD_0_ATTR_I |
                        CMD_I0_TID(xfer->cmd_tid) |
                        CMD_I0_DEV_INDEX(dat_idx) |
                        CMD_I0_DTT(data_len) |
                        CMD_I0_MODE(mode);
                mchp_fill_data_bytes(xfer, data, data_len);
        } else {
                /* we use a Regular Data Transfer Command */
                xfer->cmd_desc[0] =
                        CMD_0_ATTR_R |
                        CMD_R0_TID(xfer->cmd_tid) |
                        CMD_R0_DEV_INDEX(dat_idx) |
                        CMD_R0_MODE(mode) |
                        (rnw ? CMD_R0_RNW : 0);
                xfer->cmd_desc[1] =
                        CMD_R1_DATA_LENGTH(data_len);
        }
}

static int mchp_hci_cmd_v1_daa(struct mchp_i3c_hci *hci)
{
        struct mchp_hci_xfer *xfer;
        int ret, dat_idx = -1;
        u8 next_addr = 0;
        u64 pid;
        unsigned int dcr, bcr;
        DECLARE_COMPLETION_ONSTACK(done);

        xfer = mchp_hci_alloc_xfer(1);
        if (!xfer)
                return -ENOMEM;

        /*
         * Simple for now: we allocate a temporary DAT entry, do a single
         * DAA, register the device which will allocate its own DAT entry
         * via the core callback, then free the temporary DAT entry.
         * Loop until there is no more devices to assign an address to.
         * Yes, there is room for improvements.
         */
        for (;;) {
                ret = mchp_mipi_i3c_hci_dat_v1.alloc_entry(hci);
                if (ret < 0)
                        break;
                dat_idx = ret;
                ret = i3c_master_get_free_addr(&hci->master, next_addr);
                if (ret < 0)
                        break;
                next_addr = ret;

                dev_dbg(&hci->master.dev,"next_addr = 0x%02x, DAA using DAT %d", next_addr, dat_idx);
                mchp_mipi_i3c_hci_dat_v1.set_dynamic_addr(hci, dat_idx, next_addr);
                mchp_mipi_i3c_hci_dct_index_reset(hci);

                xfer->cmd_tid = mchp_hci_get_tid();
                xfer->cmd_desc[0] =
                        CMD_0_ATTR_A |
                        CMD_A0_TID(xfer->cmd_tid) |
                        CMD_A0_CMD(I3C_CCC_ENTDAA) |
                        CMD_A0_DEV_INDEX(dat_idx) |
                        CMD_A0_DEV_COUNT(1) |
                        CMD_A0_ROC | CMD_A0_TOC;
                xfer->cmd_desc[1] = 0;
                xfer->completion = &done;
                hci->io->queue_xfer(hci, xfer, 1);
                if (!wait_for_completion_timeout(&done, HZ) &&
                    hci->io->dequeue_xfer(hci, xfer, 1)) {
                        ret = -ETIME;
                        break;
                }
		if ((RESP_STATUS(xfer->response) == RESP_ERR_ADDR_HEADER ||
                     RESP_STATUS(xfer->response) == RESP_ERR_NACK) &&
                    RESP_DATA_LENGTH(xfer->response) == 1) {
                        ret = 0;  /* no more devices to be assigned */
                        break;
                }
		if (RESP_STATUS(xfer->response) != RESP_SUCCESS) {
                        ret = -EIO;
                        break;
                }

                mchp_i3c_hci_dct_get_val(hci, 0, &pid, &dcr, &bcr);
                dev_dbg(&hci->master.dev,"assigned address %#x to device PID=0x%llx DCR=%#x BCR=%#x",
                    next_addr, pid, dcr, bcr);

                mchp_mipi_i3c_hci_dat_v1.free_entry(hci, dat_idx);
                dat_idx = -1;

                /*
                 * TODO: Extend the subsystem layer to allow for registering
                 * new device and provide BCR/DCR/PID at the same time.
                 */
                ret = i3c_master_add_i3c_dev_locked(&hci->master, next_addr);
                if (ret)
                        break;
        }

        if (dat_idx >= 0)
                mchp_mipi_i3c_hci_dat_v1.free_entry(hci, dat_idx);
        mchp_hci_free_xfer(xfer, 1);
        return ret;
}

const struct mchp_hci_cmd_ops mchp_mipi_i3c_hci_cmd_v1 = {
        .prep_ccc               = mchp_hci_cmd_v1_prep_ccc,
        .prep_i3c_xfer          = mchp_hci_cmd_v1_prep_i3c_xfer,
        .prep_i2c_xfer          = mchp_hci_cmd_v1_prep_i2c_xfer,
        .perform_daa            = mchp_hci_cmd_v1_daa,
};

static int mchp_hci_pio_init(struct mchp_i3c_hci *hci)
{
        struct mchp_hci_pio_data *pio;
        u32 val, size_val, rx_thresh, tx_thresh, ibi_val;

        pio = kzalloc(sizeof(*pio), GFP_KERNEL);
        if (!pio)
                return -ENOMEM;

        hci->io_data = pio;
        spin_lock_init(&pio->lock);

        size_val = pio_reg_read(MCHP_QUEUE_SIZE);
        dev_info(&hci->master.dev, "CMD/RESP FIFO = %ld entries\n",
                 FIELD_GET(MCHP_CR_QUEUE_SIZE, size_val));
        dev_info(&hci->master.dev, "IBI FIFO = %ld bytes\n",
                 4 * FIELD_GET(MCHP_IBI_STATUS_SIZE, size_val));
        dev_info(&hci->master.dev, "RX data FIFO = %d bytes\n",
                 4 * (2 << FIELD_GET(MCHP_RX_DATA_BUFFER_SIZE, size_val)));
        dev_info(&hci->master.dev, "TX data FIFO = %d bytes\n",
                 4 * (2 << FIELD_GET(MCHP_TX_DATA_BUFFER_SIZE, size_val)));

        /*
         * Let's initialize data thresholds to half of the actual FIFO size.
         * The start thresholds aren't used (set to 0) as the FIFO is always
         * serviced before the corresponding command is queued.
         */
        rx_thresh = FIELD_GET(MCHP_RX_DATA_BUFFER_SIZE, size_val);
        tx_thresh = FIELD_GET(MCHP_TX_DATA_BUFFER_SIZE, size_val);
        if (hci->version_major == 1) {
                /* those are expressed as 2^[n+1), so just sub 1 if not 0 */
                if (rx_thresh)
                        rx_thresh -= 1;
                if (tx_thresh)
                        tx_thresh -= 1;
                pio->rx_thresh_size = 2 << rx_thresh;
                pio->tx_thresh_size = 2 << tx_thresh;
        } else {
                /* size is 2^(n+1) and threshold is 2^n i.e. already halved */
                pio->rx_thresh_size = 1 << rx_thresh;
                pio->tx_thresh_size = 1 << tx_thresh;
        }
        val = FIELD_PREP(MCHP_DATA_RX_BUF_THLD,   rx_thresh) |
              FIELD_PREP(MCHP_DATA_TX_BUF_THLD,   tx_thresh);
        pio_reg_write(MCHP_DATA_BUFFER_THLD_CTRL, val);
	
	/*
         * Let's raise an interrupt as soon as there is one free cmd slot
         * or one available response or IBI. For IBI data let's use half the
         * IBI queue size within allowed bounds.
         */
        ibi_val = FIELD_GET(MCHP_IBI_STATUS_SIZE, size_val);
        pio->max_ibi_thresh = clamp_val(ibi_val/2, 1, 63);
        val = FIELD_PREP(MCHP_QUEUE_IBI_STATUS_THLD, 1) |
              FIELD_PREP(MCHP_QUEUE_IBI_DATA_THLD, pio->max_ibi_thresh) |
              FIELD_PREP(MCHP_QUEUE_RESP_BUF_THLD, 1) |
              FIELD_PREP(MCHP_QUEUE_CMD_EMPTY_BUF_THLD, 1);
        pio_reg_write(MCHP_QUEUE_THLD_CTRL, val);
        pio->reg_queue_thresh = val;

        /* Disable all IRQs but allow all status bits */
        pio_reg_write(MCHP_INTR_SIGNAL_ENABLE, 0x0);
        pio_reg_write(MCHP_INTR_STATUS_ENABLE, 0xffffffff);

        /* Always accept error interrupts (will be activated on first xfer) */
        pio->enabled_irqs = STAT_ALL_ERRORS;

        return 0;
}

static void mchp_hci_pio_cleanup(struct mchp_i3c_hci *hci)
{
        struct mchp_hci_pio_data *pio = hci->io_data;

        pio_reg_write(MCHP_INTR_SIGNAL_ENABLE, 0x0);

        if (pio) {
                dev_dbg(&hci->master.dev,"status = %#x/%#x",
                    pio_reg_read(MCHP_INTR_STATUS), pio_reg_read(MCHP_INTR_SIGNAL_ENABLE));
                BUG_ON(pio->curr_xfer);
                BUG_ON(pio->curr_rx);
                BUG_ON(pio->curr_tx);
                BUG_ON(pio->curr_resp);
                kfree(pio);
                hci->io_data = NULL;
        }
}

static void mchp_hci_pio_write_cmd(struct mchp_i3c_hci *hci, struct mchp_hci_xfer *xfer)
{
        dev_dbg(&hci->master.dev,"cmd_desc[%d] = 0x%08x", 0, xfer->cmd_desc[0]);
        dev_dbg(&hci->master.dev,"cmd_desc[%d] = 0x%08x", 1, xfer->cmd_desc[1]);
        pio_reg_write(MCHP_COMMAND_QUEUE_PORT, xfer->cmd_desc[0]);
        pio_reg_write(MCHP_COMMAND_QUEUE_PORT, xfer->cmd_desc[1]);
}

static bool mchp_hci_pio_do_rx(struct mchp_i3c_hci *hci, struct mchp_hci_pio_data *pio)
{
        struct mchp_hci_xfer *xfer = pio->curr_rx;
        unsigned int nr_words;
        u32 *p;

        p = xfer->data;
        p += (xfer->data_len - xfer->data_left) / 4;

        while (xfer->data_left >= 4) {
                /* bail out if FIFO hasn't reached the threshold value yet */
                if (!(pio_reg_read(MCHP_INTR_STATUS) & STAT_RX_THLD))
                        return false;
                nr_words = min(xfer->data_left / 4, pio->rx_thresh_size);
                /* extract data from FIFO */
                xfer->data_left -= nr_words * 4;
                dev_dbg(&hci->master.dev,"now %d left %d", nr_words * 4, xfer->data_left);
                while (nr_words--)
                        *p++ = pio_reg_read(MCHP_XFER_DATA_PORT);
        }

        /* trailing data is retrieved upon response reception */
        return !xfer->data_left;
}

static void mchp_hci_pio_do_trailing_rx(struct mchp_i3c_hci *hci,
                                   struct mchp_hci_pio_data *pio, unsigned int count)
{
        struct mchp_hci_xfer *xfer = pio->curr_rx;
        u32 *p;

        dev_dbg(&hci->master.dev,"%d remaining", count);

        p = xfer->data;
        p += (xfer->data_len - xfer->data_left) / 4;

        if (count >= 4) {
                unsigned int nr_words = count / 4;
                /* extract data from FIFO */
                xfer->data_left -= nr_words * 4;
                dev_dbg(&hci->master.dev,"now %d left %d", nr_words * 4, xfer->data_left);
                while (nr_words--)
                        *p++ = pio_reg_read(MCHP_XFER_DATA_PORT);
        }

        count &= 3;
        if (count) {
                /*
                 * There are trailing bytes in the last word.
                 * Fetch it and extract bytes in an endian independent way.
                 * Unlike the TX case, we must not write memory past the
                 * end of the destination buffer.
                 */
                u8 *p_byte = (u8 *)p;
                u32 data = pio_reg_read(MCHP_XFER_DATA_PORT);

                xfer->data_word_before_partial = data;
                xfer->data_left -= count;
                data = (__force u32) cpu_to_le32(data);
                while (count--) {
                        *p_byte++ = data;
                        data >>= 8;
                }
        }
}

static bool mchp_hci_pio_do_tx(struct mchp_i3c_hci *hci, struct mchp_hci_pio_data *pio)
{
        struct mchp_hci_xfer *xfer = pio->curr_tx;
        unsigned int nr_words;
        u32 *p;

        p = xfer->data;
        p += (xfer->data_len - xfer->data_left) / 4;

        while (xfer->data_left >= 4) {
                /* bail out if FIFO free space is below set threshold */
                if (!(pio_reg_read(MCHP_INTR_STATUS) & STAT_TX_THLD))
                        return false;
                /* we can fill up to that TX threshold */
                nr_words = min(xfer->data_left / 4, pio->tx_thresh_size);
                /* push data into the FIFO */
                xfer->data_left -= nr_words * 4;
                dev_dbg(&hci->master.dev,"now %d left %d", nr_words * 4, xfer->data_left);
                while (nr_words--)
                        pio_reg_write(MCHP_XFER_DATA_PORT, *p++);
        }

        if (xfer->data_left) {
                /*
                 * There are trailing bytes to send. We can simply load
                 * them from memory as a word which will keep those bytes
                 * in their proper place even on a BE system. This will
                 * also get some bytes past the actual buffer but no one
                 * should care as they won't be sent out.
                 */
                if (!(pio_reg_read(MCHP_INTR_STATUS) & STAT_TX_THLD))
                        return false;
                dev_dbg(&hci->master.dev,"trailing %d", xfer->data_left);
                pio_reg_write(MCHP_XFER_DATA_PORT, *p);
                xfer->data_left = 0;
        }

        return true;
}

static bool mchp_hci_pio_process_rx(struct mchp_i3c_hci *hci, struct mchp_hci_pio_data *pio)
{
        while (pio->curr_rx && mchp_hci_pio_do_rx(hci, pio))
                pio->curr_rx = pio->curr_rx->next_data;
        return !pio->curr_rx;
}

static bool mchp_hci_pio_process_tx(struct mchp_i3c_hci *hci, struct mchp_hci_pio_data *pio)
{
        while (pio->curr_tx && mchp_hci_pio_do_tx(hci, pio))
                pio->curr_tx = pio->curr_tx->next_data;
        return !pio->curr_tx;
}

static void mchp_hci_pio_queue_data(struct mchp_i3c_hci *hci, struct mchp_hci_pio_data *pio)
{
        struct mchp_hci_xfer *xfer = pio->curr_xfer;
        struct mchp_hci_xfer *prev_queue_tail;

        if (!xfer->data) {
                xfer->data_len = xfer->data_left = 0;
                return;
        }

        if (xfer->rnw) {
                prev_queue_tail = pio->rx_queue;
                pio->rx_queue = xfer;
                if (pio->curr_rx) {
                        prev_queue_tail->next_data = xfer;
                } else {
                        pio->curr_rx = xfer;
                        if (!mchp_hci_pio_process_rx(hci, pio))
                                pio->enabled_irqs |= STAT_RX_THLD;
                }
        } else {
                prev_queue_tail = pio->tx_queue;
                pio->tx_queue = xfer;
                if (pio->curr_tx) {
                        prev_queue_tail->next_data = xfer;
                } else {
                        pio->curr_tx = xfer;
                        if (!mchp_hci_pio_process_tx(hci, pio))
                                pio->enabled_irqs |= STAT_TX_THLD;
                }
        }
}

static void mchp_hci_pio_push_to_next_rx(struct mchp_i3c_hci *hci, struct mchp_hci_xfer *xfer,
                                    unsigned int words_to_keep)
{
        u32 *from = xfer->data;
        u32 from_last;
        unsigned int received, count;

        received = (xfer->data_len - xfer->data_left) / 4;
        if ((xfer->data_len - xfer->data_left) & 3) {
                from_last = xfer->data_word_before_partial;
                received += 1;
        } else {
                from_last = from[received];
        }
        from += words_to_keep;
        count = received - words_to_keep;

        while (count) {
                unsigned int room, left, chunk, bytes_to_move;
                u32 last_word;

                xfer = xfer->next_data;
                if (!xfer) {
                        dev_err(&hci->master.dev, "pushing RX data to unexistent xfer\n");
                        return;
                }

                room = DIV_ROUND_UP(xfer->data_len, 4);
                left = DIV_ROUND_UP(xfer->data_left, 4);
                chunk = min(count, room);
                if (chunk > left) {
                        mchp_hci_pio_push_to_next_rx(hci, xfer, chunk - left);
                        left = chunk;
                        xfer->data_left = left * 4;
                }

                bytes_to_move = xfer->data_len - xfer->data_left;
                if (bytes_to_move & 3) {
                        /* preserve word  to become partial */
                        u32 *p = xfer->data;

                        xfer->data_word_before_partial = p[bytes_to_move / 4];
                }
                memmove(xfer->data + chunk, xfer->data, bytes_to_move);
		
		/* treat last word specially because of partial word issues */
                chunk -= 1;

                memcpy(xfer->data, from, chunk * 4);
                xfer->data_left -= chunk * 4;
                from += chunk;
                count -= chunk;

                last_word = (count == 1) ? from_last : *from++;
                if (xfer->data_left < 4) {
                        /*
                         * Like in hci_pio_do_trailing_rx(), preserve original
                         * word to be stored partially then store bytes it
                         * in an endian independent way.
                         */
                        u8 *p_byte = xfer->data;

                        p_byte += chunk * 4;
                        xfer->data_word_before_partial = last_word;
                        last_word = (__force u32) cpu_to_le32(last_word);
                        while (xfer->data_left--) {
                                *p_byte++ = last_word;
                                last_word >>= 8;
                        }
                } else {
                        u32 *p = xfer->data;

                        p[chunk] = last_word;
                        xfer->data_left -= 4;
                }
                count--;
        }
}

static bool mchp_hci_pio_process_resp(struct mchp_i3c_hci *hci, struct mchp_hci_pio_data *pio)
{
        while (pio->curr_resp &&
               (pio_reg_read(MCHP_INTR_STATUS) & STAT_RESP_READY)) {
                struct mchp_hci_xfer *xfer = pio->curr_resp;
                u32 resp = pio_reg_read(MCHP_RESPONSE_QUEUE_PORT);
                unsigned int tid = RESP_TID(resp);

                dev_dbg(&hci->master.dev,"resp = 0x%08x", resp);
                if (tid != xfer->cmd_tid) {
                        dev_err(&hci->master.dev,
                                "response tid=%d when expecting %d\n",
                                tid, xfer->cmd_tid);
                        /* let's pretend it is a prog error... any of them  */
                        mchp_hci_pio_err(hci, pio, STAT_PROG_ERRORS);
                        return false;
                }
                xfer->response = resp;

                if (pio->curr_rx == xfer) {
                        /*
                         * Response availability implies RX completion.
                         * Retrieve trailing RX data if any.
                         * Note that short reads are possible.
                         */
                        unsigned int received, expected, to_keep;

                        received = xfer->data_len - xfer->data_left;
                        expected = RESP_DATA_LENGTH(xfer->response);
                        if (expected > received) {
                                mchp_hci_pio_do_trailing_rx(hci, pio,
                                                       expected - received);
                        } else if (received > expected) {
                                /* we consumed data meant for next xfer */
                                to_keep = DIV_ROUND_UP(expected, 4);
                                mchp_hci_pio_push_to_next_rx(hci, xfer, to_keep);
                        }

                        /* then process the RX list pointer */
                        if (mchp_hci_pio_process_rx(hci, pio))
                                pio->enabled_irqs &= ~STAT_RX_THLD;
                }

		/*
                 * We're about to give back ownership of the xfer structure
                 * to the waiting instance. Make sure no reference to it
                 * still exists.
                 */
                if (pio->curr_rx == xfer) {
                        dev_dbg(&hci->master.dev,"short RX ?");
                        pio->curr_rx = pio->curr_rx->next_data;
                } else if (pio->curr_tx == xfer) {
                        dev_dbg(&hci->master.dev,"short TX ?");
                        pio->curr_tx = pio->curr_tx->next_data;
                } else if (xfer->data_left) {
                        dev_dbg(&hci->master.dev,"PIO xfer count = %d after response",
                            xfer->data_left);
                }

                pio->curr_resp = xfer->next_resp;
                if (xfer->completion)
                        complete(xfer->completion);
        }
        return !pio->curr_resp;
}

static void mchp_hci_pio_queue_resp(struct mchp_i3c_hci *hci, struct mchp_hci_pio_data *pio)
{
        struct mchp_hci_xfer *xfer = pio->curr_xfer;
        struct mchp_hci_xfer *prev_queue_tail;

        if (!(xfer->cmd_desc[0] & CMD_0_ROC))
                return;

        prev_queue_tail = pio->resp_queue;
        pio->resp_queue = xfer;
        if (pio->curr_resp) {
                prev_queue_tail->next_resp = xfer;
        } else {
                pio->curr_resp = xfer;
                if (!mchp_hci_pio_process_resp(hci, pio))
                        pio->enabled_irqs |= STAT_RESP_READY;
        }
}

static bool mchp_hci_pio_process_cmd(struct mchp_i3c_hci *hci, struct mchp_hci_pio_data *pio)
{
        while (pio->curr_xfer &&
               (pio_reg_read(MCHP_INTR_STATUS) & STAT_CMD_QUEUE_READY)) {
                /*
                 * Always process the data FIFO before sending the command
                 * so needed TX data or RX space is available upfront.
                 */
                mchp_hci_pio_queue_data(hci, pio);
                /*
                 * Then queue our response request. This will also process
                 * the response FIFO in case it got suddenly filled up
                 * with results from previous commands.
                 */
                mchp_hci_pio_queue_resp(hci, pio);
                /*
                 * Finally send the command.
                 */
                mchp_hci_pio_write_cmd(hci, pio->curr_xfer);
                /*
                 * And move on.
                 */
                pio->curr_xfer = pio->curr_xfer->next_xfer;
        }
        return !pio->curr_xfer;
}

static int mchp_hci_pio_queue_xfer(struct mchp_i3c_hci *hci, struct mchp_hci_xfer *xfer, int n)
{
        struct mchp_hci_pio_data *pio = hci->io_data;
        struct mchp_hci_xfer *prev_queue_tail;
        int i;

        dev_dbg(&hci->master.dev,"n = %d", n);

        /* link xfer instances together and initialize data count */
        for (i = 0; i < n; i++) {
                xfer[i].next_xfer = (i + 1 < n) ? &xfer[i + 1] : NULL;
                xfer[i].next_data = NULL;
                xfer[i].next_resp = NULL;
                xfer[i].data_left = xfer[i].data_len;
        }

        spin_lock_irq(&pio->lock);
        prev_queue_tail = pio->xfer_queue;
        pio->xfer_queue = &xfer[n - 1];
        if (pio->curr_xfer) {
                prev_queue_tail->next_xfer = xfer;
        } else {
                pio->curr_xfer = xfer;
                if (!mchp_hci_pio_process_cmd(hci, pio))
                        pio->enabled_irqs |= STAT_CMD_QUEUE_READY;
                pio_reg_write(MCHP_INTR_SIGNAL_ENABLE, pio->enabled_irqs);
                dev_dbg(&hci->master.dev,"status = %#x/%#x",
                    pio_reg_read(MCHP_INTR_STATUS), pio_reg_read(MCHP_INTR_SIGNAL_ENABLE));
        }
        spin_unlock_irq(&pio->lock);
        return 0;
}

static bool mchp_hci_pio_dequeue_xfer_common(struct mchp_i3c_hci *hci,
                                        struct mchp_hci_pio_data *pio,
                                        struct mchp_hci_xfer *xfer, int n)
{
        struct mchp_hci_xfer *p, **p_prev_next;
        int i;

        /*
         * To safely dequeue a transfer request, it must be either entirely
         * processed, or not yet processed at all. If our request tail is
         * reachable from either the data or resp list that means the command
         * was submitted and not yet completed.
         */
        for (p = pio->curr_resp; p; p = p->next_resp)
                for (i = 0; i < n; i++)
                        if (p == &xfer[i])
                                goto pio_screwed;
        for (p = pio->curr_rx; p; p = p->next_data)
                for (i = 0; i < n; i++)
                        if (p == &xfer[i])
                                goto pio_screwed;
        for (p = pio->curr_tx; p; p = p->next_data)
                for (i = 0; i < n; i++)
                        if (p == &xfer[i])
                                goto pio_screwed;

        /*
         * The command was completed, or wasn't yet submitted.
         * Unlink it from the que if the later.
         */
        p_prev_next = &pio->curr_xfer;
        for (p = pio->curr_xfer; p; p = p->next_xfer) {
                if (p == &xfer[0]) {
                        *p_prev_next = xfer[n - 1].next_xfer;
                        break;
                }
                p_prev_next = &p->next_xfer;
        }

        /* return true if we actually unqueued something */
        return !!p;

pio_screwed:
        /*
         * Life is tough. We must invalidate the hardware state and
         * discard everything that is still queued.
         */
        for (p = pio->curr_resp; p; p = p->next_resp) {
                p->response = FIELD_PREP(RESP_ERR_FIELD, RESP_ERR_HC_TERMINATED);
                if (p->completion)
                        complete(p->completion);
        }
        for (p = pio->curr_xfer; p; p = p->next_xfer) {
                p->response = FIELD_PREP(RESP_ERR_FIELD, RESP_ERR_HC_TERMINATED);
                if (p->completion)
                        complete(p->completion);
        }
        pio->curr_xfer = pio->curr_rx = pio->curr_tx = pio->curr_resp = NULL;

        return true;
}

static bool mchp_hci_pio_dequeue_xfer(struct mchp_i3c_hci *hci, struct mchp_hci_xfer *xfer, int n)
{
        struct mchp_hci_pio_data *pio = hci->io_data;
        int ret;

        spin_lock_irq(&pio->lock);
        dev_dbg(&hci->master.dev,"n=%d status=%#x/%#x", n,
            pio_reg_read(MCHP_INTR_STATUS), pio_reg_read(MCHP_INTR_SIGNAL_ENABLE));
        dev_dbg(&hci->master.dev,"main_status = %#x/%#x",
            readl(hci->base_regs + 0x20), readl(hci->base_regs + 0x28));

        ret = mchp_hci_pio_dequeue_xfer_common(hci, pio, xfer, n);
        spin_unlock_irq(&pio->lock);
        return ret;
}

static void mchp_hci_pio_err(struct mchp_i3c_hci *hci, struct mchp_hci_pio_data *pio,
                        u32 status)
{
        /* TODO: this ought to be more sophisticated eventually */

        if (pio_reg_read(MCHP_INTR_STATUS) & STAT_RESP_READY) {
                /* this may happen when an error is signaled with ROC unset */
                u32 resp = pio_reg_read(MCHP_RESPONSE_QUEUE_PORT);

                dev_err(&hci->master.dev,
                        "orphan response (%#x) on error\n", resp);
        }

        /* dump states on programming errors */
        if (status & STAT_PROG_ERRORS) {
                u32 queue = pio_reg_read(MCHP_QUEUE_CUR_STATUS);
                u32 data = pio_reg_read(MCHP_DATA_BUFFER_CUR_STATUS);

                dev_err(&hci->master.dev,
                        "prog error %#lx (C/R/I = %ld/%ld/%ld, TX/RX = %ld/%ld)\n",
                        status & STAT_PROG_ERRORS,
                        FIELD_GET(MCHP_CUR_CMD_Q_EMPTY_LEVEL, queue),
                        FIELD_GET(MCHP_CUR_RESP_Q_LEVEL, queue),
                        FIELD_GET(MCHP_CUR_IBI_Q_LEVEL, queue),
                        FIELD_GET(MCHP_CUR_TX_BUF_LVL, data),
                        FIELD_GET(MCHP_CUR_RX_BUF_LVL, data));
        }

        /* just bust out everything with pending responses for now */
        mchp_hci_pio_dequeue_xfer_common(hci, pio, pio->curr_resp, 1);
        /* ... and half-way TX transfers if any */
        if (pio->curr_tx && pio->curr_tx->data_left != pio->curr_tx->data_len)
                mchp_hci_pio_dequeue_xfer_common(hci, pio, pio->curr_tx, 1);
        /* then reset the hardware */
        mchp_mipi_i3c_hci_pio_reset(hci);
        mchp_mipi_i3c_hci_resume(hci);

        dev_dbg(&hci->master.dev,"status=%#x/%#x",
            pio_reg_read(MCHP_INTR_STATUS), pio_reg_read(MCHP_INTR_SIGNAL_ENABLE));
}

static void mchp_hci_pio_set_ibi_thresh(struct mchp_i3c_hci *hci,
                                   struct mchp_hci_pio_data *pio,
                                   unsigned int thresh_val)
{
        u32 regval = pio->reg_queue_thresh;

        regval &= ~MCHP_QUEUE_IBI_STATUS_THLD;
        regval |= FIELD_PREP(MCHP_QUEUE_IBI_STATUS_THLD, thresh_val);
        /* write the threshold reg only if it changes */
        if (regval != pio->reg_queue_thresh) {
                pio_reg_write(MCHP_QUEUE_THLD_CTRL, regval);
                pio->reg_queue_thresh = regval;
                dev_dbg(&hci->master.dev,"%d", thresh_val);
        }
}

static bool mchp_hci_pio_get_ibi_segment(struct mchp_i3c_hci *hci,
                                    struct mchp_hci_pio_data *pio)
{
        struct hci_pio_ibi_data *ibi = &pio->ibi;
        unsigned int nr_words, thresh_val;
        u32 *p;

        p = ibi->data_ptr;
        p += (ibi->seg_len - ibi->seg_cnt) / 4;

        while ((nr_words = ibi->seg_cnt/4)) {
                /* determine our IBI queue threshold value */
                thresh_val = min(nr_words, pio->max_ibi_thresh);
                mchp_hci_pio_set_ibi_thresh(hci, pio, thresh_val);
                /* bail out if we don't have that amount of data ready */
                if (!(pio_reg_read(MCHP_INTR_STATUS) & STAT_IBI_STATUS_THLD))
                        return false;
                /* extract the data from the IBI port */
                nr_words = thresh_val;
                ibi->seg_cnt -= nr_words * 4;
                dev_dbg(&hci->master.dev,"now %d left %d", nr_words * 4, ibi->seg_cnt);
                while (nr_words--)
                        *p++ = pio_reg_read(MCHP_IBI_PORT);
        }

	if (ibi->seg_cnt) {
                /*
                 * There are trailing bytes in the last word.
                 * Fetch it and extract bytes in an endian independent way.
                 * Unlike the TX case, we must not write past the end of
                 * the destination buffer.
                 */
                u32 data;
                u8 *p_byte = (u8 *)p;

                mchp_hci_pio_set_ibi_thresh(hci, pio, 1);
                if (!(pio_reg_read(MCHP_INTR_STATUS) & STAT_IBI_STATUS_THLD))
                        return false;
                dev_dbg(&hci->master.dev,"trailing %d", ibi->seg_cnt);
                data = pio_reg_read(MCHP_IBI_PORT);
                data = (__force u32) cpu_to_le32(data);
                while (ibi->seg_cnt--) {
                        *p_byte++ = data;
                        data >>= 8;
                }
        }

        return true;
}

static bool mchp_hci_pio_prep_new_ibi(struct mchp_i3c_hci *hci, struct mchp_hci_pio_data *pio)
{
        struct hci_pio_ibi_data *ibi = &pio->ibi;
        struct i3c_dev_desc *dev;
        struct mchp_i3c_hci_dev_data *dev_data;
        struct hci_pio_dev_ibi_data *dev_ibi;
        u32 ibi_status;

        /*
         * We have a new IBI. Try to set up its payload retrieval.
         * When returning true, the IBI data has to be consumed whether
         * or not we are set up to capture it. If we return true with
         * ibi->slot == NULL that means the data payload has to be
         * drained out of the IBI port and dropped.
         */
	
	ibi_status = pio_reg_read(MCHP_IBI_PORT);
        dev_dbg(&hci->master.dev,"status = %#x", ibi_status);
        ibi->addr = FIELD_GET(MCHP_IBI_TARGET_ADDR, ibi_status);
        if (ibi_status & MCHP_IBI_ERROR) {
                dev_err(&hci->master.dev, "IBI error from %#x\n", ibi->addr);
                return false;
        }

        ibi->last_seg = ibi_status & MCHP_IBI_LAST_STATUS;
        ibi->seg_len = FIELD_GET(MCHP_IBI_DATA_LENGTH, ibi_status);
        ibi->seg_cnt = ibi->seg_len;

        dev = mchp_i3c_hci_addr_to_dev(hci, ibi->addr);
        if (!dev) {
                dev_err(&hci->master.dev,
                        "IBI for unknown device %#x\n", ibi->addr);
                return true;
        }

        dev_data = i3c_dev_get_master_data(dev);
        dev_ibi = dev_data->ibi_data;
        ibi->max_len = dev_ibi->max_len;

        if (ibi->seg_len > ibi->max_len) {
                dev_err(&hci->master.dev, "IBI payload too big (%d > %d)\n",
                        ibi->seg_len, ibi->max_len);
                return true;
        }

        ibi->slot = i3c_generic_ibi_get_free_slot(dev_ibi->pool);
        if (!ibi->slot) {
                dev_err(&hci->master.dev, "no free slot for IBI\n");
        } else {
                ibi->slot->len = 0;
                ibi->data_ptr = ibi->slot->data;
        }
        return true;
}

static void mchp_hci_pio_free_ibi_slot(struct mchp_i3c_hci *hci, struct mchp_hci_pio_data *pio)
{
        struct hci_pio_ibi_data *ibi = &pio->ibi;
        struct hci_pio_dev_ibi_data *dev_ibi;

        if (ibi->slot) {
                dev_ibi = ibi->slot->dev->common.master_priv;
                i3c_generic_ibi_recycle_slot(dev_ibi->pool, ibi->slot);
                ibi->slot = NULL;
        }
}

static bool mchp_hci_pio_process_ibi(struct mchp_i3c_hci *hci, struct mchp_hci_pio_data *pio)
{
        struct hci_pio_ibi_data *ibi = &pio->ibi;

        if (!ibi->slot && !ibi->seg_cnt && ibi->last_seg)
                if (!mchp_hci_pio_prep_new_ibi(hci, pio))
                        return false;

        for (;;) {
                u32 ibi_status;
                unsigned int ibi_addr;

                if (ibi->slot) {
                        if (!mchp_hci_pio_get_ibi_segment(hci, pio))
                                return false;
                        ibi->slot->len += ibi->seg_len;
                        ibi->data_ptr += ibi->seg_len;
                        if (ibi->last_seg) {
                                /* was the last segment: submit it and leave */
                                i3c_master_queue_ibi(ibi->slot->dev, ibi->slot);
                                ibi->slot = NULL;
                                mchp_hci_pio_set_ibi_thresh(hci, pio, 1);
                                return true;
                        }
                } else if (ibi->seg_cnt) {
                        /*
                         * No slot but a non-zero count. This is the result
                         * of some error and the payload must be drained.
                         * This normally does not happen therefore no need
                         * to be extra optimized here.
                         */
			mchp_hci_pio_set_ibi_thresh(hci, pio, 1);
                        do {
                                if (!(pio_reg_read(MCHP_INTR_STATUS) & STAT_IBI_STATUS_THLD))
                                        return false;
                                pio_reg_read(MCHP_IBI_PORT);
                        } while (--ibi->seg_cnt);
                        if (ibi->last_seg)
                                return true;
                }

                /* try to move to the next segment right away */
                mchp_hci_pio_set_ibi_thresh(hci, pio, 1);
                if (!(pio_reg_read(MCHP_INTR_STATUS) & STAT_IBI_STATUS_THLD))
                        return false;
                ibi_status = pio_reg_read(MCHP_IBI_PORT);
                ibi_addr = FIELD_GET(MCHP_IBI_TARGET_ADDR, ibi_status);
                if (ibi->addr != ibi_addr) {
                        /* target address changed before last segment */
                        dev_err(&hci->master.dev,
                                "unexp IBI address changed from %d to %d\n",
                                ibi->addr, ibi_addr);
                        mchp_hci_pio_free_ibi_slot(hci, pio);
                }
                ibi->last_seg = ibi_status & MCHP_IBI_LAST_STATUS;
                ibi->seg_len = FIELD_GET(MCHP_IBI_DATA_LENGTH, ibi_status);
                ibi->seg_cnt = ibi->seg_len;
                if (ibi->slot && ibi->slot->len + ibi->seg_len > ibi->max_len) {
                        dev_err(&hci->master.dev,
                                "IBI payload too big (%d > %d)\n",
                                ibi->slot->len + ibi->seg_len, ibi->max_len);
                        mchp_hci_pio_free_ibi_slot(hci, pio);
                }
        }

        return false;
}

static int mchp_hci_pio_request_ibi(struct mchp_i3c_hci *hci, struct i3c_dev_desc *dev,
                               const struct i3c_ibi_setup *req)
{
        struct mchp_i3c_hci_dev_data *dev_data = i3c_dev_get_master_data(dev);
        struct i3c_generic_ibi_pool *pool;
        struct hci_pio_dev_ibi_data *dev_ibi;

        dev_ibi = kmalloc(sizeof(*dev_ibi), GFP_KERNEL);
        if (!dev_ibi)
                return -ENOMEM;
        pool = i3c_generic_ibi_alloc_pool(dev, req);
        if (IS_ERR(pool)) {
                kfree(dev_ibi);
                return PTR_ERR(pool);
        }
        dev_ibi->pool = pool;
        dev_ibi->max_len = req->max_payload_len;
        dev_data->ibi_data = dev_ibi;
        return 0;
}

static void mchp_hci_pio_free_ibi(struct mchp_i3c_hci *hci, struct i3c_dev_desc *dev)
{
        struct mchp_i3c_hci_dev_data *dev_data = i3c_dev_get_master_data(dev);
        struct hci_pio_dev_ibi_data *dev_ibi = dev_data->ibi_data;

        dev_data->ibi_data = NULL;
        i3c_generic_ibi_free_pool(dev_ibi->pool);
        kfree(dev_ibi);
}

static void mchp_hci_pio_recycle_ibi_slot(struct mchp_i3c_hci *hci,
                                    struct i3c_dev_desc *dev,
                                    struct i3c_ibi_slot *slot)
{
        struct mchp_i3c_hci_dev_data *dev_data = i3c_dev_get_master_data(dev);
        struct hci_pio_dev_ibi_data *dev_ibi = dev_data->ibi_data;

        i3c_generic_ibi_recycle_slot(dev_ibi->pool, slot);
}

static bool mchp_hci_pio_irq_handler(struct mchp_i3c_hci *hci, unsigned int unused)
{
        struct mchp_hci_pio_data *pio = hci->io_data;
        u32 status;

        spin_lock(&pio->lock);
        status = pio_reg_read(MCHP_INTR_STATUS);
        dev_dbg(&hci->master.dev,"(in) status: %#x/%#x", status, pio->enabled_irqs);
        status &= pio->enabled_irqs | STAT_LATENCY_WARNINGS;
        if (!status) {
                spin_unlock(&pio->lock);
                return false;
        }

        if (status & STAT_IBI_STATUS_THLD)
                mchp_hci_pio_process_ibi(hci, pio);

        if (status & STAT_RX_THLD)
                if (mchp_hci_pio_process_rx(hci, pio))
                        pio->enabled_irqs &= ~STAT_RX_THLD;
        if (status & STAT_TX_THLD)
                if (mchp_hci_pio_process_tx(hci, pio))
                        pio->enabled_irqs &= ~STAT_TX_THLD;
        if (status & STAT_RESP_READY)
                if (mchp_hci_pio_process_resp(hci, pio))
                        pio->enabled_irqs &= ~STAT_RESP_READY;

        if (unlikely(status & STAT_LATENCY_WARNINGS)) {
                pio_reg_write(MCHP_INTR_STATUS, status & STAT_LATENCY_WARNINGS);
                dev_warn_ratelimited(&hci->master.dev,
                                     "encountered warning condition %#lx\n",
                                     status & STAT_LATENCY_WARNINGS);
        }

        if (unlikely(status & STAT_ALL_ERRORS)) {
                pio_reg_write(MCHP_INTR_STATUS, status & STAT_ALL_ERRORS);
                mchp_hci_pio_err(hci, pio, status & STAT_ALL_ERRORS);
        }

        if (status & STAT_CMD_QUEUE_READY)
                if (mchp_hci_pio_process_cmd(hci, pio))
                        pio->enabled_irqs &= ~STAT_CMD_QUEUE_READY;

        pio_reg_write(MCHP_INTR_SIGNAL_ENABLE, pio->enabled_irqs);
        dev_dbg(&hci->master.dev,"(out) status: %#x/%#x",
            pio_reg_read(MCHP_INTR_STATUS), pio_reg_read(MCHP_INTR_SIGNAL_ENABLE));
        spin_unlock(&pio->lock);
        return true;
}

const struct mchp_hci_io_ops mchp_mipi_i3c_hci_pio = {
        .init                   = mchp_hci_pio_init,
        .cleanup                = mchp_hci_pio_cleanup,
        .queue_xfer             = mchp_hci_pio_queue_xfer,
        .dequeue_xfer           = mchp_hci_pio_dequeue_xfer,
        .irq_handler            = mchp_hci_pio_irq_handler,
        .request_ibi            = mchp_hci_pio_request_ibi,
        .free_ibi               = mchp_hci_pio_free_ibi,
        .recycle_ibi_slot       = mchp_hci_pio_recycle_ibi_slot,
};

static inline struct mchp_i3c_hci *to_i3c_hci(struct i3c_master_controller *m)
{
	return container_of(m, struct mchp_i3c_hci, master);
}

static int mchp_i3c_hci_bus_init(struct i3c_master_controller *m)
{
	struct mchp_i3c_hci *hci = to_i3c_hci(m);
	struct i3c_device_info info;
	int ret;

	if (hci->cmd == &mchp_mipi_i3c_hci_cmd_v1) {
		ret = mchp_mipi_i3c_hci_dat_v1.init(hci);
		if (ret)
			return ret;
	}

	ret = i3c_master_get_free_addr(m, 0);
	if (ret < 0)
		return ret;
	reg_write(MCHP_MASTER_DEVICE_ADDR,
		  MCHP_MASTER_DYNAMIC_ADDR(ret) | MCHP_MASTER_DYNAMIC_ADDR_VALID);
	memset(&info, 0, sizeof(info));
	info.dyn_addr = ret;
	ret = i3c_master_set_info(m, &info);
	if (ret)
		return ret;

	ret = hci->io->init(hci);
	if (ret)
		return ret;

	microchip_set_resp_buf_thld(hci);

	reg_set(MCHP_HC_CONTROL, MCHP_HC_CONTROL_BUS_ENABLE);
	dev_dbg(&hci->master.dev,"HC_CONTROL = %#x", reg_read(MCHP_HC_CONTROL));

	return 0;
}

static void mchp_i3c_hci_bus_cleanup(struct i3c_master_controller *m)
{
	struct mchp_i3c_hci *hci = to_i3c_hci(m);
	struct platform_device *pdev = to_platform_device(m->dev.parent);

	reg_clear(MCHP_HC_CONTROL, MCHP_HC_CONTROL_BUS_ENABLE);
	synchronize_irq(platform_get_irq(pdev, 0));
	hci->io->cleanup(hci);
	if (hci->cmd == &mchp_mipi_i3c_hci_cmd_v1)
		mchp_mipi_i3c_hci_dat_v1.cleanup(hci);
}

void mchp_mipi_i3c_hci_resume(struct mchp_i3c_hci *hci)
{
	reg_set(MCHP_HC_CONTROL, MCHP_HC_CONTROL_RESUME);
}

/* located here rather than pio.c because needed bits are in core reg space */
void mchp_mipi_i3c_hci_pio_reset(struct mchp_i3c_hci *hci)
{
	reg_write(MCHP_RESET_CONTROL, MCHP_RX_FIFO_RST | MCHP_TX_FIFO_RST | MCHP_RESP_QUEUE_RST);
}

/* located here rather than dct.c because needed bits are in core reg space */
void mchp_mipi_i3c_hci_dct_index_reset(struct mchp_i3c_hci *hci)
{
	reg_write(MCHP_DCT_SECTION, FIELD_PREP(MCHP_DCT_TABLE_INDEX, 0));
}

static int mchp_i3c_hci_send_ccc_cmd(struct i3c_master_controller *m,
				struct i3c_ccc_cmd *ccc)
{
	struct mchp_i3c_hci *hci = to_i3c_hci(m);
	struct mchp_hci_xfer *xfer;
	bool raw = !!(hci->quirks & HCI_QUIRK_RAW_CCC);
	bool prefixed = raw && !!(ccc->id & I3C_CCC_DIRECT);
	unsigned int nxfers = ccc->ndests + prefixed;
	DECLARE_COMPLETION_ONSTACK(done);
	int i, last, ret = 0;

	dev_dbg(&hci->master.dev,"cmd=%#x rnw=%d ndests=%d data[0].len=%d",
	    ccc->id, ccc->rnw, ccc->ndests, ccc->dests[0].payload.len);

	xfer = mchp_hci_alloc_xfer(nxfers);
	if (!xfer)
		return -ENOMEM;

	if (prefixed) {
		xfer->data = NULL;
		xfer->data_len = 0;
		xfer->rnw = false;
		hci->cmd->prep_ccc(hci, xfer, I3C_BROADCAST_ADDR,
				   ccc->id, true);
		xfer++;
	}

	for (i = 0; i < nxfers - prefixed; i++) {
		xfer[i].data = ccc->dests[i].payload.data;
		xfer[i].data_len = ccc->dests[i].payload.len;
		xfer[i].rnw = ccc->rnw;
		ret = hci->cmd->prep_ccc(hci, &xfer[i], ccc->dests[i].addr,
					 ccc->id, raw);
		if (ret)
			goto out;
		xfer[i].cmd_desc[0] |= CMD_0_ROC;
	}
	last = i - 1;
	xfer[last].cmd_desc[0] |= CMD_0_TOC;
	xfer[last].completion = &done;

	if (prefixed)
		xfer--;

	ret = hci->io->queue_xfer(hci, xfer, nxfers);
	if (ret)
		goto out;
	if (!wait_for_completion_timeout(&done, HZ) &&
	    hci->io->dequeue_xfer(hci, xfer, nxfers)) {
		ret = -ETIME;
		goto out;
	}
	for (i = prefixed; i < nxfers; i++) {
		if (ccc->rnw)
			ccc->dests[i - prefixed].payload.len =
				RESP_DATA_LENGTH(xfer[i].response);
		switch (RESP_STATUS(xfer[i].response)) {
		case RESP_SUCCESS:
			continue;
		case RESP_ERR_ADDR_HEADER:
		case RESP_ERR_NACK:
			ccc->err = I3C_ERROR_M2;
			fallthrough;
		default:
			ret = -EIO;
			goto out;
		}
	}

	if (ccc->rnw)
		dev_dbg(&hci->master.dev,"got: %*ph",
		    ccc->dests[0].payload.len, ccc->dests[0].payload.data);

out:
	mchp_hci_free_xfer(xfer, nxfers);
	return ret;
}

static int mchp_i3c_hci_daa(struct i3c_master_controller *m)
{
	struct mchp_i3c_hci *hci = to_i3c_hci(m);
	return hci->cmd->perform_daa(hci);
}

static int mchp_i3c_hci_alloc_safe_xfer_buf(struct mchp_i3c_hci *hci,
				       struct mchp_hci_xfer *xfer)
{
	if (hci->io == &mchp_mipi_i3c_hci_pio ||
	    xfer->data == NULL || !is_vmalloc_addr(xfer->data))
		return 0;
	if (xfer->rnw)
		xfer->bounce_buf = kzalloc(xfer->data_len, GFP_KERNEL);
	else
		xfer->bounce_buf = kmemdup(xfer->data,
					   xfer->data_len, GFP_KERNEL);

	return xfer->bounce_buf == NULL ? -ENOMEM : 0;
}

static void mchp_i3c_hci_free_safe_xfer_buf(struct mchp_i3c_hci *hci,
				       struct mchp_hci_xfer *xfer)
{
	if (hci->io == &mchp_mipi_i3c_hci_pio || xfer->bounce_buf == NULL)
	return;
	if (xfer->rnw)
		memcpy(xfer->data, xfer->bounce_buf, xfer->data_len);

	kfree(xfer->bounce_buf);
}

static int mchp_i3c_hci_priv_xfers(struct i3c_dev_desc *dev,
			      struct i3c_priv_xfer *i3c_xfers,
			      int nxfers)
{
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct mchp_i3c_hci *hci = to_i3c_hci(m);
	struct mchp_hci_xfer *xfer;
	DECLARE_COMPLETION_ONSTACK(done);
	unsigned int size_limit;
	int i, last, ret = 0;

	dev_dbg(&hci->master.dev,"nxfers = %d", nxfers);

	xfer = mchp_hci_alloc_xfer(nxfers);
	if (!xfer)
		return -ENOMEM;

	size_limit = 1U << 16;
	for (i = 0; i < nxfers; i++) {
		xfer[i].data_len = i3c_xfers[i].len;
		ret = -EFBIG;
		if (xfer[i].data_len >= size_limit)
			goto out;
		xfer[i].rnw = i3c_xfers[i].rnw;
		if (i3c_xfers[i].rnw) {
			xfer[i].data = i3c_xfers[i].data.in;
		} else {
			/* silence the const qualifier warning with a cast */
			xfer[i].data = (void *) i3c_xfers[i].data.out;
		}
		hci->cmd->prep_i3c_xfer(hci, dev, &xfer[i]);
		xfer[i].cmd_desc[0] |= CMD_0_ROC;
		ret = mchp_i3c_hci_alloc_safe_xfer_buf(hci, &xfer[i]);
		if (ret)
			goto out;
	}
	last = i - 1;
	xfer[last].cmd_desc[0] |= CMD_0_TOC;
	xfer[last].completion = &done;

	ret = hci->io->queue_xfer(hci, xfer, nxfers);
	if (ret)
		goto out;
	if (!wait_for_completion_timeout(&done, HZ) &&
	    hci->io->dequeue_xfer(hci, xfer, nxfers)) {
		ret = -ETIME;
		goto out;
	}
	for (i = 0; i < nxfers; i++) {
		if (i3c_xfers[i].rnw)
			i3c_xfers[i].len = RESP_DATA_LENGTH(xfer[i].response);
		if (RESP_STATUS(xfer[i].response) != RESP_SUCCESS) {
			ret = -EIO;
			goto out;
		}
	}

out:
	for (i = 0; i < nxfers; i++)
		mchp_i3c_hci_free_safe_xfer_buf(hci, &xfer[i]);

	mchp_hci_free_xfer(xfer, nxfers);
	return ret;
}

static int mchp_i3c_hci_i2c_xfers(struct i2c_dev_desc *dev,
			     const struct i2c_msg *i2c_xfers, int nxfers)
{
	struct i3c_master_controller *m = i2c_dev_get_master(dev);
	struct mchp_i3c_hci *hci = to_i3c_hci(m);
	struct mchp_hci_xfer *xfer;
	DECLARE_COMPLETION_ONSTACK(done);
	int i, last, ret = 0;

	dev_dbg(&hci->master.dev,"nxfers = %d", nxfers);

	xfer = mchp_hci_alloc_xfer(nxfers);
	if (!xfer)
		return -ENOMEM;

	for (i = 0; i < nxfers; i++) {
		xfer[i].data = i2c_xfers[i].buf;
		xfer[i].data_len = i2c_xfers[i].len;
		xfer[i].rnw = i2c_xfers[i].flags & I2C_M_RD;
		hci->cmd->prep_i2c_xfer(hci, dev, &xfer[i]);
		xfer[i].cmd_desc[0] |= CMD_0_ROC;
		ret = mchp_i3c_hci_alloc_safe_xfer_buf(hci, &xfer[i]);
		if (ret)
			goto out;
	}
	last = i - 1;
	xfer[last].cmd_desc[0] |= CMD_0_TOC;
	xfer[last].completion = &done;

	ret = hci->io->queue_xfer(hci, xfer, nxfers);
	if (ret)
		goto out;
	if (!wait_for_completion_timeout(&done, HZ) &&
	    hci->io->dequeue_xfer(hci, xfer, nxfers)) {
		ret = -ETIME;
		goto out;
	}
	for (i = 0; i < nxfers; i++) {
		if (RESP_STATUS(xfer[i].response) != RESP_SUCCESS) {
			ret = -EIO;
			goto out;
		}
	}

out:
	for (i = 0; i < nxfers; i++)
		mchp_i3c_hci_free_safe_xfer_buf(hci, &xfer[i]);

	mchp_hci_free_xfer(xfer, nxfers);
	return ret;
}

static int mchp_i3c_hci_attach_i3c_dev(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct mchp_i3c_hci *hci = to_i3c_hci(m);
	struct mchp_i3c_hci_dev_data *dev_data;
	int ret;

	dev_data = kzalloc(sizeof(*dev_data), GFP_KERNEL);
	if (!dev_data)
		return -ENOMEM;
	if (hci->cmd == &mchp_mipi_i3c_hci_cmd_v1) {
		ret = mchp_mipi_i3c_hci_dat_v1.alloc_entry(hci);
		if (ret < 0) {
			kfree(dev_data);
			return ret;
		}
		mchp_mipi_i3c_hci_dat_v1.set_dynamic_addr(hci, ret, dev->info.dyn_addr);
		dev_data->dat_idx = ret;
	}
	i3c_dev_set_master_data(dev, dev_data);
	return 0;
}

static int mchp_i3c_hci_reattach_i3c_dev(struct i3c_dev_desc *dev, u8 old_dyn_addr)
{
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct mchp_i3c_hci *hci = to_i3c_hci(m);
	struct mchp_i3c_hci_dev_data *dev_data = i3c_dev_get_master_data(dev);

	if (hci->cmd == &mchp_mipi_i3c_hci_cmd_v1)
		mchp_mipi_i3c_hci_dat_v1.set_dynamic_addr(hci, dev_data->dat_idx,
					     dev->info.dyn_addr);
	return 0;
}

static void mchp_i3c_hci_detach_i3c_dev(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct mchp_i3c_hci *hci = to_i3c_hci(m);
	struct mchp_i3c_hci_dev_data *dev_data = i3c_dev_get_master_data(dev);

	i3c_dev_set_master_data(dev, NULL);
	if (hci->cmd == &mchp_mipi_i3c_hci_cmd_v1)
		mchp_mipi_i3c_hci_dat_v1.free_entry(hci, dev_data->dat_idx);
	kfree(dev_data);
}

static int mchp_i3c_hci_attach_i2c_dev(struct i2c_dev_desc *dev)
{
	struct i3c_master_controller *m = i2c_dev_get_master(dev);
	struct mchp_i3c_hci *hci = to_i3c_hci(m);
	struct mchp_i3c_hci_dev_data *dev_data;
	int ret;

	if (hci->cmd != &mchp_mipi_i3c_hci_cmd_v1)
		return 0;
	dev_data = kzalloc(sizeof(*dev_data), GFP_KERNEL);
	if (!dev_data)
		return -ENOMEM;
	ret = mchp_mipi_i3c_hci_dat_v1.alloc_entry(hci);
	if (ret < 0) {
		kfree(dev_data);
		return ret;
	}
	mchp_mipi_i3c_hci_dat_v1.set_static_addr(hci, ret, dev->addr);
	mchp_mipi_i3c_hci_dat_v1.set_flags(hci, ret, DAT_0_I2C_DEVICE, 0);
	dev_data->dat_idx = ret;
	i2c_dev_set_master_data(dev, dev_data);
	return 0;
}

static void mchp_i3c_hci_detach_i2c_dev(struct i2c_dev_desc *dev)
{
	struct i3c_master_controller *m = i2c_dev_get_master(dev);
	struct mchp_i3c_hci *hci = to_i3c_hci(m);
	struct mchp_i3c_hci_dev_data *dev_data = i2c_dev_get_master_data(dev);

	if (dev_data) {
		i2c_dev_set_master_data(dev, NULL);
		if (hci->cmd == &mchp_mipi_i3c_hci_cmd_v1)
			mchp_mipi_i3c_hci_dat_v1.free_entry(hci, dev_data->dat_idx);
		kfree(dev_data);
	}
}

static int mchp_i3c_hci_request_ibi(struct i3c_dev_desc *dev,
			       const struct i3c_ibi_setup *req)
{
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct mchp_i3c_hci *hci = to_i3c_hci(m);
	struct mchp_i3c_hci_dev_data *dev_data = i3c_dev_get_master_data(dev);
	unsigned int dat_idx = dev_data->dat_idx;

	if (req->max_payload_len != 0)
		mchp_mipi_i3c_hci_dat_v1.set_flags(hci, dat_idx, DAT_0_IBI_PAYLOAD, 0);
	else
		mchp_mipi_i3c_hci_dat_v1.clear_flags(hci, dat_idx, DAT_0_IBI_PAYLOAD, 0);
	return hci->io->request_ibi(hci, dev, req);
}

static void mchp_i3c_hci_free_ibi(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct mchp_i3c_hci *hci = to_i3c_hci(m);

	hci->io->free_ibi(hci, dev);
}

static int mchp_i3c_hci_enable_ibi(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct mchp_i3c_hci *hci = to_i3c_hci(m);
	struct mchp_i3c_hci_dev_data *dev_data = i3c_dev_get_master_data(dev);

	mchp_mipi_i3c_hci_dat_v1.clear_flags(hci, dev_data->dat_idx, DAT_0_SIR_REJECT, 0);
	return i3c_master_enec_locked(m, dev->info.dyn_addr, I3C_CCC_EVENT_SIR);
}

static int mchp_i3c_hci_disable_ibi(struct i3c_dev_desc *dev)
{
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct mchp_i3c_hci *hci = to_i3c_hci(m);
	struct mchp_i3c_hci_dev_data *dev_data = i3c_dev_get_master_data(dev);

	mchp_mipi_i3c_hci_dat_v1.set_flags(hci, dev_data->dat_idx, DAT_0_SIR_REJECT, 0);
	return i3c_master_disec_locked(m, dev->info.dyn_addr, I3C_CCC_EVENT_SIR);
}

static void mchp_i3c_hci_recycle_ibi_slot(struct i3c_dev_desc *dev,
				     struct i3c_ibi_slot *slot)
{
	struct i3c_master_controller *m = i3c_dev_get_master(dev);
	struct mchp_i3c_hci *hci = to_i3c_hci(m);

	hci->io->recycle_ibi_slot(hci, dev, slot);
}

static const struct i3c_master_controller_ops mchp_i3c_hci_ops = {
	.bus_init		= mchp_i3c_hci_bus_init,
	.bus_cleanup		= mchp_i3c_hci_bus_cleanup,
	.do_daa			= mchp_i3c_hci_daa,
	.send_ccc_cmd		= mchp_i3c_hci_send_ccc_cmd,
	.priv_xfers		= mchp_i3c_hci_priv_xfers,
	.i2c_xfers		= mchp_i3c_hci_i2c_xfers,
	.attach_i3c_dev		= mchp_i3c_hci_attach_i3c_dev,
	.reattach_i3c_dev	= mchp_i3c_hci_reattach_i3c_dev,
	.detach_i3c_dev		= mchp_i3c_hci_detach_i3c_dev,
	.attach_i2c_dev		= mchp_i3c_hci_attach_i2c_dev,
	.detach_i2c_dev		= mchp_i3c_hci_detach_i2c_dev,
	.request_ibi		= mchp_i3c_hci_request_ibi,
	.free_ibi		= mchp_i3c_hci_free_ibi,
	.enable_ibi		= mchp_i3c_hci_enable_ibi,
	.disable_ibi		= mchp_i3c_hci_disable_ibi,
	.recycle_ibi_slot	= mchp_i3c_hci_recycle_ibi_slot,
};

static irqreturn_t mchp_i3c_hci_irq_handler(int irq, void *dev_id)
{
	struct mchp_i3c_hci *hci = dev_id;
	irqreturn_t result = IRQ_NONE;
	u32 val;

	val = reg_read(MCHP_INTR_STATUS);
	dev_dbg(&hci->master.dev,"INTR_STATUS = %#x", val);

	if (val & MCHP_INTR_HC_INTERNAL_ERR) {
		dev_err(&hci->master.dev, "Host Controller Internal Error\n");
		val &= ~MCHP_INTR_HC_INTERNAL_ERR;
	}
		
	hci->io->irq_handler(hci, 0);
	
	if (val)
		dev_err(&hci->master.dev, "unexpected INTR_STATUS %#x\n", val);
	else
		result = IRQ_HANDLED;

	return result;
}

static int mchp_i3c_hci_init(struct mchp_i3c_hci *hci)
{
	bool size_in_dwords, mode_selector;
	u32 regval, offset;
	int ret;

	/* Validate HCI hardware version */
	regval = reg_read(MCHP_HCI_VERSION);
	hci->version_major = (regval >> 8) & 0xf;
	hci->version_minor = (regval >> 4) & 0xf;
	hci->revision = regval & 0xf;
	dev_notice(&hci->master.dev, "MIPI I3C HCI v%u.%u r%02u\n",
		   hci->version_major, hci->version_minor, hci->revision);
	/* known versions */
	switch (regval & ~0xf) {
	case 0x100:	/* version 1.0 */
	case 0x110:	/* version 1.1 */
	case 0x200:	/* version 2.0 */
		break;
	default:
		dev_err(&hci->master.dev, "unsupported HCI version\n");
		return -EPROTONOSUPPORT;
	}

	hci->caps = reg_read(MCHP_HC_CAPABILITIES);
	dev_dbg(&hci->master.dev,"caps = %#x", hci->caps);

	size_in_dwords = hci->version_major < 1 ||
			 (hci->version_major == 1 && hci->version_minor < 1);

	regval = reg_read(MCHP_DAT_SECTION);
	offset = FIELD_GET(MCHP_DAT_TABLE_OFFSET, regval);
	hci->DAT_regs = offset ? hci->base_regs + offset : NULL;
	hci->DAT_entries = FIELD_GET(MCHP_DAT_TABLE_SIZE, regval);
	hci->DAT_entry_size = 8;
	if (size_in_dwords)
		hci->DAT_entries = 4 * hci->DAT_entries / hci->DAT_entry_size;
	dev_info(&hci->master.dev, "DAT: %u %u-bytes entries at offset %#x\n",
		 hci->DAT_entries, hci->DAT_entry_size, offset);

	regval = reg_read(MCHP_DCT_SECTION);
	offset = FIELD_GET(MCHP_DCT_TABLE_OFFSET, regval);
	hci->DCT_regs = offset ? hci->base_regs + offset : NULL;
	hci->DCT_entries = FIELD_GET(MCHP_DCT_TABLE_SIZE, regval);
	hci->DCT_entry_size = 16;
	if (size_in_dwords)
		hci->DCT_entries = 4 * hci->DCT_entries / hci->DCT_entry_size;
	dev_info(&hci->master.dev, "DCT: %u %u-bytes entries at offset %#x\n",
		 hci->DCT_entries, hci->DCT_entry_size, offset);

	regval = reg_read(MCHP_RING_HEADERS_SECTION);
	offset = FIELD_GET(MCHP_RING_HEADERS_OFFSET, regval);
	hci->RHS_regs = offset ? hci->base_regs + offset : NULL;
	dev_info(&hci->master.dev, "Ring Headers at offset %#x\n", offset);

	regval = reg_read(MCHP_PIO_SECTION);
	offset = FIELD_GET(MCHP_PIO_REGS_OFFSET, regval);
	hci->PIO_regs = offset ? hci->base_regs + offset : NULL;
	dev_info(&hci->master.dev, "PIO section at offset %#x\n", offset);

	regval = reg_read(MCHP_EXT_CAPS_SECTION);
	offset = FIELD_GET(MCHP_EXT_CAPS_OFFSET, regval);
	hci->EXTCAPS_regs = offset ? hci->base_regs + offset : NULL;
	dev_info(&hci->master.dev, "Extended Caps at offset %#x\n", offset);

	ret = i3c_hci_parse_ext_caps(hci);
	if (ret)
		return ret;

	/*
	 * Now let's reset the hardware.
	 * SOFT_RST must be clear before we write to it.
	 * Then we must wait until it clears again.
	 */
	ret = readx_poll_timeout(reg_read, MCHP_RESET_CONTROL, regval,
				 !(regval & MCHP_SOFT_RST), 1, 10000);
	if (ret)
		return -ENXIO;
	reg_write(MCHP_RESET_CONTROL, MCHP_SOFT_RST);
	ret = readx_poll_timeout(reg_read, MCHP_RESET_CONTROL, regval,
				 !(regval & MCHP_SOFT_RST), 1, 10000);
	if (ret)
		return -ENXIO;

	/* Disable all interrupts and allow all signal updates */
	reg_write(MCHP_INTR_SIGNAL_ENABLE, 0x0);
	reg_write(MCHP_INTR_STATUS_ENABLE, 0xffffffff);
	
	hci->cmd = &mchp_mipi_i3c_hci_cmd_v1;
	mode_selector = hci->version_major > 1 ||
				(hci->version_major == 1 && hci->version_minor > 0);

	/* Quirk for HCI_QUIRK_PIO_MODE on MICROCHIP platforms */
	if (hci->quirks & MCHP_HCI_QUIRK_PIO_MODE) {
			hci->RHS_regs = NULL;
	}

	hci->io = &mchp_mipi_i3c_hci_pio;
	dev_info(&hci->master.dev, "Using PIO\n");
	
	microchip_set_od_pp_timing(hci);

	return 0;
}

static int mchp_i3c_hci_probe(struct platform_device *pdev)
{
	struct mchp_i3c_hci *hci;
	int irq, ret;

	hci = devm_kzalloc(&pdev->dev, sizeof(*hci), GFP_KERNEL);
	if (!hci)
		return -ENOMEM;
	hci->base_regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(hci->base_regs))
		return PTR_ERR(hci->base_regs);

	hci->gclk = devm_clk_get(&pdev->dev, "gclk");
	if (IS_ERR(hci->gclk))
		return PTR_ERR(hci->gclk);

	hci->pclk = devm_clk_get(&pdev->dev, "pclk");
	if (IS_ERR(hci->pclk))
		return PTR_ERR(hci->pclk);

	ret = clk_prepare_enable(hci->gclk);
	if (ret)
		goto err_disable_gclk;

	ret = clk_prepare_enable(hci->pclk);
	if (ret)
		goto err_disable_pclk;

	platform_set_drvdata(pdev, hci);
	
	hci->master.dev.init_name = dev_name(&pdev->dev);

	hci->quirks = (unsigned long)device_get_match_data(&pdev->dev);

	ret = mchp_i3c_hci_init(hci);
	if (ret)
		return ret;

	irq = platform_get_irq(pdev, 0);
	ret = devm_request_irq(&pdev->dev, irq, mchp_i3c_hci_irq_handler,
			       0, NULL, hci);
	if (ret)
		return ret;

	ret = i3c_master_register(&hci->master, &pdev->dev,
				  &mchp_i3c_hci_ops, false);
	if (ret)
		return ret;

	return 0;

err_disable_pclk:
	clk_disable_unprepare(hci->pclk);

err_disable_gclk:
	clk_disable_unprepare(hci->gclk);

	return ret;
}

static void mchp_i3c_hci_remove(struct platform_device *pdev)
{
	struct mchp_i3c_hci *hci = platform_get_drvdata(pdev);

	i3c_master_unregister(&hci->master);
}

static const __maybe_unused struct of_device_id mchp_i3c_hci_of_match[] = {
	{ .compatible = "microchip,sama7d65-i3c-hci", .data = (void *)(MCHP_HCI_QUIRK_PIO_MODE | MCHP_HCI_QUIRK_OD_PP_TIMING | MCHP_HCI_QUIRK_RESP_BUF_THLD) },
	{},
};
MODULE_DEVICE_TABLE(of, mchp_i3c_hci_of_match);

static struct platform_driver mchp_i3c_hci_driver = {
	.probe = mchp_i3c_hci_probe,
	.remove_new = mchp_i3c_hci_remove,
	.driver = {
		.name = "sama7d65-i3c-hci-master",
		.of_match_table = of_match_ptr(mchp_i3c_hci_of_match),
	},
};
module_platform_driver(mchp_i3c_hci_driver);

MODULE_AUTHOR("Durai Manickam KR <durai.manickamkr@microchip.com>");
MODULE_DESCRIPTION("Microchip SAMA7d65 I3C HCI master driver");
MODULE_LICENSE("GPL");
