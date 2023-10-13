/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: cmdhandler.h
 *
 * Abstract: include related macro and declaration about SD CMD
 *
 * Version: 1.00
 *
 * Environment:	OS Independent
 */

/* Command Response */
#define    CMD_FLG_RESCHK               (1<<0)
/* Tuning */
#define    CMD_FLG_TUNE                 (1<<1)

#define    CMD_FLG_INF_BUILD		(1<<2)
#define    CMD_FLG_INF_CON		(1<<3)
#define    CMD_FLG_INF			(CMD_FLG_INF_BUILD | CMD_FLG_INF_CON)

/* R1 */
#define    CMD_FLG_R1                   (1<<4)
/* R1B */
#define    CMD_FLG_R1B                  (1<<5)
/* R2(CID,CSD) */
#define    CMD_FLG_R2                   (1<<6)
/* R3(OCR) */
#define    CMD_FLG_R3                   (1<<7)
/* R6( Publish RSA ) */
#define    CMD_FLG_R6                   (1<<8)
/* R4(I/O OCR) */
#define    CMD_FLG_R4                   (1<<9)
/* R5 */
#define    CMD_FLG_R5                   (1<<10)
/* R7 */
#define    CMD_FLG_R7                   (1<<11)
#define    CMD_FLG_RESP_MASK  (CMD_FLG_R1 | CMD_FLG_R1B | CMD_FLG_R2 | CMD_FLG_R3 | CMD_FLG_R4 | \
				CMD_FLG_R5 | CMD_FLG_R6 | CMD_FLG_R7)
#define	  CMD_FLG_ADMA_SDMA		(1<<12)
/* DMA */
#define    CMD_FLG_SDMA                 (1<<13)
/* DMA */
#define    CMD_FLG_ADMA2                (1<<14)
/* DMA */
#define    CMD_FLG_ADMA3                (1<<15)
/* DMA */
#define    CMD_FLG_DMA	(CMD_FLG_SDMA | CMD_FLG_ADMA2 | CMD_FLG_ADMA3 | CMD_FLG_ADMA_SDMA)

#define    CMD_FLG_PACKED		(1<<16)
#define    CMD_FLG_AUTO23		(1<<17)
#define    CMD_FLG_AUTO12		(1<<18)

/* Multiple data command */
#define   CMD_FLG_MULDATA			(1<<19)
#define   CMD_FLG_NO_TRANS		(1<<20)
#define	  CMD_FLG_DDR200_WORK_AROUND (1<<21)

/* ------------------------------------------------------------------------- */
/*                           SD MEMORY CARD                                  */
/* ------------------------------------------------------------------------- */
/* 1bit Bus Width */
#define    BUS_WIDTH_1BIT           0x0000
/* 4bit Bus Width */
#define    BUS_WIDTH_4BIT           0x0002

/*
 * UHS2 SD-TRAN CMD HEADER
 */
#define UHS2_CMD_HEADER_DCMD	(1 << 4)
#define UHS2_CMD_HEADER_NP	(1 << 7)
#define	UHS2_HEADER_DID(did)	(did)
#define	UHS2_HEADER_SID(sid)	(sid<<12)

/*
 * UHS2 Native command headers
 */
#define	UHS2_NATIVE_HEADER_RW	(1<<23)
#define	UHS2_NATIVE_CCMD_PLEN0	(0<<20)
#define	UHS2_NATIVE_CCMD_PLEN4	(1<<20)
#define	UHS2_NATIVE_CCMD_PLEN8	(2<<20)
#define	UHS2_NATIVE_CCMD_PLEN16	(3<<20)

#define UHS2_CMD_HEADER_APPCMD	(1 << 30)

#define	UHS2_HEADER_TMODE_DAM	(1<<19)
#define UHS2_CMD_TMODE_TLUM	(1<<20)
#define UHS2_CMD_TMODE_LM	(1<<21)
#define UHS2_CMD_TMODE_DM	(1<<22)

#define UHS2_CMD_NATIVE_RW	(1<<23)

#define UHS2_NATIVE_CCMD_IOADDR(addr)	(((addr & 0x0F00)<<8) |  ((addr & 0x00FF)<<24))
#define	UHS2_GET_NATIVE_IOADDR(head)	(((head >> 24) & 0xFF) | ((head & 0xF0000) >> 8))

#define	UHS2_GET_NATIVE_IOADDR(head)	(((head >> 24) & 0xFF) | ((head & 0xF0000) >> 8))
#define UHS2_IS_NATIVE(head) (head & UHS2_CMD_HEADER_NP)

#define	UHS2_IOADDR_FULLRESET	0x200
#define	UHS2_IOADDR_GODMT	0x201
#define	UHS2_IOADDR_DEVINIT	0x202
#define	UHS2_IOADDR_ENUM	0x203
#define	UHS2_IOADDR_ABORT	0x204

#define	UHS2_IOADDR_GEN_CAPL	0x000
#define	UHS2_IOADDR_GEN_CAPH	0x001
#define	UHS2_IOADDR_PHY_CAPL	0x002
#define	UHS2_IOADDR_PHY_CAPH	0x003
#define	UHS2_IOADDR_LINKT_CAPL	0x004
#define	UHS2_IOADDR_LINKT_CAPH	0x005

#define	UHS2_IOADDR_GEN_SETL	0x008
#define	UHS2_IOADDR_GEN_SETH	0x009
#define	UHS2_IOADDR_PHY_SETL	0x00A
#define	UHS2_IOADDR_PHY_SETH	0x00B
#define	UHS2_IOADDR_LINKT_SETL	0x00C
#define	UHS2_IOADDR_LINKT_SETH	0x00D

#define	UHS2_IOADDR_ST_REG	0x180

#define UHS2_RESP_NACK		BIT23

#define	EMMC_OCR_HI			0x40FF8000UL
#define	EMMC_OCR_LOW			0x40000080UL
#define	EMMC_OCR			       0x40FF8080UL

/* Command Define */

#define    SD_CMD0                 0x00
#define    SD_CMD1                 0x01
#define    SD_CMD2                 0x02
#define    SD_CMD3                 0x03
#define    SD_CMD4                 0x04
#define    SD_CMD5                 0x05
#define    SD_CMD6                 0x06
#define    SD_CMD7                 0x07
#define    SD_CMD8                 0x08
#define    SD_CMD9                 0x09
#define    SD_CMD10                0x0A
#define    SD_CMD11                0x0B
#define    SD_CMD12                0x0C
#define    SD_CMD13                0x0D
#define    SD_CMD14                0x0E
#define    SD_CMD15                0x0F
#define    SD_CMD16                0x10
#define    SD_CMD17                0x11
#define    SD_CMD18                0x12
#define    SD_CMD19                0x13
#define    SD_CMD20                0x14

/* DPS Passwd Authentication */
#define    SD_CMD21                0x15
#define    SD_CMD22                0x16
#define    SD_CMD23                0x17
#define    SD_CMD24                0x18
#define    SD_CMD25                0x19
#define    SD_CMD26                0x1A
#define    SD_CMD27                0x1B
#define    SD_CMD28                0x1C
#define    SD_CMD29                0x1D
#define    SD_CMD30                0x1E
#define    SD_CMD31                0x1F
#define    SD_CMD32                0x20
#define    SD_CMD33                0x21
#define    SD_CMD34                0x22
#define    SD_CMD35                0x23
#define    SD_CMD36                0x24
#define    SD_CMD37                0x25
#define    SD_CMD38                0x26
#define    SD_CMD39                0x27
#define    SD_CMD40                0x28
#define    SD_CMD41                0x29
#define    SD_CMD42                0x2A
#define    SD_CMD43                0x2B
#define    SD_CMD44                0x2C
#define    SD_CMD45                0x2D
#define    SD_CMD46                0x2E
#define    SD_CMD47                0x2F
#define    SD_CMD48                0x30
#define    SD_CMD49                0x31
#define    SD_CMD50                0x32
#define    SD_CMD51                0x33
#define    SD_CMD52                0x34
#define    SD_CMD53                0x35
#define    SD_CMD54                0x36
#define    SD_CMD55                0x37
#define    SD_CMD56                0x38
#define    SD_CMD57                0x39
#define    SD_CMD58                0x3A
#define    SD_CMD59                0x3B
#define    SD_CMD60                0x3C
#define    SD_CMD61                0x3D
#define    SD_CMD62                0x3E
#define    SD_CMD63                0x3F

#define SD_APPCMD		0x80

#define    SD_ACMD6                0x86
#define    SD_ACMD13               0x8D
/* DPS Passwd Management */
#define    SD_ACMD14               0x8E
/* DPS Reads and Decrypts */
#define    SD_ACMD15               0x8F
/* DPS Encrypts and writes */
#define    SD_ACMD16               0x90
#define    SD_ACMD22               0x96
#define    SD_ACMD23               0x97
/* DPS Read the infor of DPS, DPS_Off */
#define    SD_ACMD28               0x9C
#define    SD_ACMD41               0xA9
#define    SD_ACMD42               0xAA
#define    SD_ACMD51               0xB3

bool thread_exec_high_prio_job(bht_dev_ext_t *pdx, cb_soft_intr_t func,
			       void *data);

/*
 * check cmdline and datline inhabit
 * Fill host_trans_reg_t according to sd_cmd
 */
bool cmd_generate_reg(sd_card_t *card, sd_command_t *sd_cmd);

/*
 * Fill host_cmd_req_t according to sd_cmd
 * For legacy appcommand , will first sync execute cmd55
 */
bool cmd_execute_sync(sd_card_t *card, sd_command_t *sd_cmd,
		      req_callback func_done);

/*
 * for legacy acmd case, this fucn will do cmd55
 * For legacy appcommand , will first sync execute cmd55
 */
bool cmd_execute_sync2(sd_card_t *card, sd_command_t *sd_cmd,
		       host_cmd_req_t *req, req_callback func_done);

bool cmd_execute_sync3(sd_card_t *card, sd_command_t *sd_cmd,
		       host_cmd_req_t *req, req_callback func_done,
		       issue_post_callback post_cb);

/* inline bool cmd_can_use_packed(sd_card_t *card); */
u32 cmd_can_use_inf(sd_card_t *card, e_data_dir dir, u32 sec_addr,
		    u32 sec_cnt);

bool cmd_dat_line_chk(sd_card_t *card, sd_command_t *sd_cmd);
/*
 * Descriptor Apis
 */

/*
 * 1.need support  (26bit/16bit data length; 64bit/128bit desc)
 * 2.generat ADMA2 desc buf on pbuf.
 * 3.return end desc buffer
 * 4.upper layer handle size for merge link.
 */
#define AMDA2_26BIT_DATA_LEN (1<<0)
#define ADMA2_128BIT_DESC_LEN (1<<1)
dma_desc_buf_t build_adma2_desc_nop(sg_list_t *sg, u32 sg_len, byte *desc_buf,
				    u32 desc_len, bool dma_64bit,
				    bool data_26bit);
dma_desc_buf_t build_adma2_desc(sg_list_t *sg, u32 sg_len, byte *desc_buf,
				u32 desc_len, bool dma_64bit, bool data_26bit);
void update_adma2_desc_inf(byte *desc_end, phy_addr_t phy_addr, u32 flag);
void amda2_desc_merage(byte *tbl1_end, phy_addr_t tbl2_header);

/*
 *	build adma3 cmd item
 *	return end of cmd table addr
 */
byte *build_adma3_cmd_desc(byte *cmd_tbl, host_trans_reg_t *reg,
			   e_card_type type);

/*
 * 1. need support 64bit/128bit desc
 * (1)fill integrate table.
 * caller prepare integrate desc table for all cmd list.
 */
byte *build_integrated_desc(byte *tbl, phy_addr_t *phy_addr, bool dma_64bit);
void end_integrated_desc(byte *tbl, u32 flag);

void cmd_set_auto_cmd_flag(sd_card_t *card, u32 *cmd_flag);

bool card_is_low_capacity(sd_card_t *card);
bool cmd_is_adma_error(sd_command_t *sd_cmd);

dma_desc_buf_t *node_get_desc_res(node_t *node, u32 max_use_size);
bool update_adma2_inf_tb(u8 *pdesc, u8 **link_addr, phy_addr_t *pa,
			 bool dma_64bit);
