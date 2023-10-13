/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: transh.h
 *
 * Abstract: define related macro about transmission
 *
 * Version: 1.00
 *
 * Environment:	OS Independent
 *
 */

#ifndef _TRANS_HD_H
#define _TRANS_HD_H

/* OS platform */
#define DMA_BUF_ALIGN_SIZE (1<<12)

/* HOST attr */
#define SDHCI_CPAS_ADMA2_26BIT_LEN      (1<<0)

/* general desc */
#define GENERAL_DESC_END_BIT 0x2

/* ADMA2 */
/* 1. attr */
#define ADMA2_DESC_LINK_VALID  0x31
#define ADMA2_DESC_TRAN_VALID  0x21
#define ADMA2_DESC_INT_VALID   0x5

#define ADMA2_DESC_END_BIT      0x2
#define ADMA2_DESC_INT_BIT		0x4

/* 2. user ext */
#define ADMA2_16BIT_LEN_SIZE (1<<16)
#define ADMA2_26BIT_LEN_SIZE (1<<26)

#define ADMA2_ITEM_LEN                  8
#define ADMA2_128BIT_ITEM_LEN   16
#define MAX_ADMA2_ITEM_LEN      (ADMA2_128BIT_ITEM_LEN)

/* ADMA3 */
/* 1. attr */
#define ADMA3_INTEGRATE_DESC_VALID 0x39

#define ADMA3_DESC_SD_VALID  0x9
#define ADMA3_DESC_UHS2_VALID  0x19
#define ADMA3_DESC_SD_END_VALID  0xB

/* 2. ADMA3 */
#define RESEVED_ADMA3_INTEGRATEDDESC_LINE_SIZE  2

#define ADMA3_INTEGRATEDDESC_ITEM_LEN          8
#define ADMA3_INTEGRATEDDESC_128BIT_ITEM_LEN   16
#define MAX_ADMA3_INTEGRATE_ITEM_LEN (ADMA3_INTEGRATEDDESC_128BIT_ITEM_LEN)

#define ADMA3_CMDDESC_ITEM_LENGTH                       8
#define ADMA3_CMDDESC_ITEM_NUM_UHSI                     4
#define ADMA3_CMDDESC_ITEM_NUM_UHSII                    8

#define MAX_ADMA3_INTERGATE_TABLE_LEN_PER_QUEUE_PER_NODE \
	(MAX_ADMA3_INTEGRATE_ITEM_LEN * (1+RESEVED_ADMA3_INTEGRATEDDESC_LINE_SIZE))
#define MAX_ADMA3_INTERGATE_TABLE_LEN_PER_NODE \
	(MAX_ADMA3_INTERGATE_TABLE_LEN_PER_QUEUE_PER_NODE * TQ_WORK_QUEUE_SIZE)

/* MAX */

/* 1. SDMA */
#define MAX_SDMA_LIKE_DATA_SIZE (CFG_MAX_TRANSFER_LENGTH)

/* 2. desc buffer */
#define RESEVED_ADMA2_DESC_LINE_SIZE  128

#define MAX_ADMA2_TABLE_LEN ((ADMA2_MAX_DESC_LINE_SIZE)*(ADMA2_ITEM_LEN))
#define MAX_ADMA2_128BIT_TABLE_LEN ((ADMA2_MAX_DESC_LINE_SIZE)*(ADMA2_128BIT_ITEM_LEN))
#define MAX_GENERAL_DESC_TABLE_LEN ((ADMA2_MAX_DESC_LINE_SIZE+RESEVED_ADMA2_DESC_LINE_SIZE)*MAX_ADMA2_ITEM_LEN)

#define MAX_DUMP_MODE_DESC_SIZE	(16*1024)

#define MAX_ADMA3_CMDDESC_TABLE_LEN (ADMA3_CMDDESC_ITEM_LENGTH*ADMA3_CMDDESC_ITEM_NUM_UHSII)

/* 3.NODE */

#define MAX_NODE_BUF_SIZE (MAX_ADMA3_INTERGATE_TABLE_LEN_PER_NODE + MAX_ADMA3_CMDDESC_TABLE_LEN +\
	MAX_GENERAL_DESC_TABLE_LEN + DMA_BUF_ALIGN_SIZE)
#define MAX_SDMA_LIKE_MODE_NODE_BUF_SIZE (MAX_NODE_BUF_SIZE + MAX_SDMA_LIKE_DATA_SIZE)

/* DMA API  */
#define DMA_API_BUF_SIZE          (64*1024)
#define MIN_DMA_API_BUF_SIZE    (MAX_GENERAL_DESC_TABLE_LEN+DMA_BUF_ALIGN_SIZE + DMA_API_BUF_SIZE)

/* this is used for tag queue */
typedef struct s_note_t {
	void *data;
	void *psrb_ext;
	/* bind to card */
	sd_card_t *card;
	list_entry list;
	struct {
		/* poiner to adma2 table */
		dma_desc_buf_t head;
		/* pointer to adma2 table */
		dma_desc_buf_t end;
	} phy_node_buffer;
	dma_desc_buf_t general_desc_tbl;
	/* dbg use */
	dma_desc_buf_t general_desc_tbl_img;
	/* sdma like mode */
	dma_desc_buf_t data_tbl;
	/* dbg use */
	dma_desc_buf_t data_tbl_img;

	/* virt buffer is not merged */
	virt_buffer_t v_buff;

	/* flag 0 means it is attahment to a cmd, 1 means it has been merged */
	byte flag;
	u8 sdma_like:1;
} node_t, *pnode_t;

typedef struct {
	node_t dma_node;
	/* Add this node for ADMA2 Infinte API */
	node_t dma_node2;
	node_t *cur_node;
	u8 *adma2_inf_link_addr;

} dma_trans_api_t;

#endif
