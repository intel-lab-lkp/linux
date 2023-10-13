// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: transhandler.c
 *
 * Abstract: Handler for interrupt and dma buffer manager apis
 *
 * Version: 1.00
 *
 * Author: Peter.Guo
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 9/5/2014		Creation	Peter.guo
 */

#include "../include/basic.h"
#include "../include/debug.h"
#include "../include/hostapi.h"
#include "../include/cmdhandler.h"
#include "../host/hostreg.h"
#include "../include/util.h"

/*
 *
 * Function Name: gen_adma2_desc_low_32bit
 *
 * Abstract:
 *
 *			generate adma2 descriptor table low 32bit(data length & attribution)
 *
 * Input:
 *
 *			u32 len [in]: data length value
 *			u32 attr[in]: descriptor attribution value
 *
 * Output:
 *
 *			u32 [out]: the composite value for ADMA2 descriptor low 32bit value
 *
 * Return value:
 *
 *			None
 * Notes:
 *
 * Caller:
 *
 */
#define ADMA2_16BIT_MASK 0x0ffff
#define ADMA2_10BIT_MASK 0x003ff

#define  gen_adma2_desc_low_32bit(len, attr) (((len & ADMA2_16BIT_MASK) << 16) | \
	(((len >> 16) & ADMA2_10BIT_MASK) << 6) | attr)

bool dma_api_build_adma_io_add_nop(bht_dev_ext_t *pdx, sd_data_t *sd_data,
				   sg_list_t *sg, u32 sg_len);
bool dma_api_build_adma_sdma_io_add_nop(bht_dev_ext_t *pdx,
					sd_data_t *sd_data);

/*
 *
 * Function Name: end_adma2_desc_line
 *
 * Abstract:
 *
 *			end 32bit DAM address adma2 descriptor table
 *
 * Input:
 *
 *			u32 *pTable [in]: Pointer to the descriptor table
 *
 * Output:
 *
 *			None.
 *
 * Return value:
 *
 *			None
 * Notes:
 *
 * Caller:
 *
 */
static void adma2_end_desc_line(u8 *ptb, bool dma_64bit)
{
	if (dma_64bit == TRUE)
		ptb = ptb - ADMA2_128BIT_ITEM_LEN;
	else
		ptb = ptb - ADMA2_ITEM_LEN;

	*ptb |= ADMA2_DESC_END_BIT;
}

/*
 *
 * Function Name: adma2_clear_end_flag
 *
 * Abstract:
 *
 *			clear DAM address adma2 descriptor end flag table for link ADMA2 table
 *
 * Input:
 *
 *			u32 *pTable [in]: Pointer to the descriptor table
 *
 * Output:
 *
 *			None.
 *
 * Return value:
 *
 *			None
 * Notes:
 *
 * Caller:
 *
 */
static void adma2_clear_end_flag(u8 *ptb, bool dma_64bit)
{
	if (dma_64bit == TRUE)
		ptb = ptb - ADMA2_128BIT_ITEM_LEN;
	else
		ptb = ptb - ADMA2_ITEM_LEN;

	*ptb &= ~(ADMA2_DESC_END_BIT);

}

bool link_adma2_desc(u8 *pdesc, phy_addr_t *pa, bool dma_64bit)
{

	DbgInfo(MODULE_TRANS, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	adma2_clear_end_flag(pdesc, dma_64bit);

	*((u32 *) (pdesc)) = gen_adma2_desc_low_32bit(0, ADMA2_DESC_LINK_VALID);
	pdesc += 4;
	*((u32 *) (pdesc)) = os_get_phy_addr32l(*pa);
	if (dma_64bit == TRUE)
		*((u32 *) (pdesc + 4)) = os_get_phy_addr32h(*pa);
	return TRUE;
}

/*
 *
 * Function Name: build_adma2_desc_line
 *
 * Abstract:
 *
 *			build 32bit address adma2 descriptor lines for one SGlist item.
 *
 * Input:
 *
 *			u32 *pTable [in]: descripter table pointer
 *			u32  len_limit [in]: data  length limitation
 *			u32 itemTotalLen [in]: one SGlist item total length
 *			u32 itemAddrHdr [in]: one SGlist item physical address
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			the adma2 descriptor line number for one SGlist item.
 * Notes:
 *
 * Caller:
 *
 */
static int build_adma2_desc_line(u32 *pTable, u32 len_limit, u32 itemTotalLen,
				 u64 itemAddrHdr, bool dma_64bit)
{
	int adma2_line_number = 0;
	u64 *p64 = 0;

	if (itemTotalLen == 0) {
		DbgErr("%s sg len 0\n", __func__);
		return 0;
	}

	if (itemTotalLen > ADMA2_16BIT_LEN_SIZE) {
		DbgInfo(MODULE_TRANS, FEATURE_RW_TRACE, NOT_TO_RAM,
			"SGlist item pa over 64KB\n");
	}

	if (itemTotalLen < len_limit) {
		*(pTable++) =
		    gen_adma2_desc_low_32bit(itemTotalLen,
					     ADMA2_DESC_TRAN_VALID);
		if (dma_64bit == TRUE) {
			p64 = (u64 *) pTable;
			*p64 = itemAddrHdr;
		} else
			*(pTable++) = (u32) itemAddrHdr;
		adma2_line_number++;
	} else {
		u32 j = 0;

		do {
			itemTotalLen -= len_limit;
			*(pTable++) =
			    gen_adma2_desc_low_32bit(0, ADMA2_DESC_TRAN_VALID);
			if (dma_64bit == TRUE) {
				p64 = (u64 *) pTable;
				*p64 =
				    itemAddrHdr + ((u64) j * (u64) (len_limit));
				pTable += 3;
			} else
				*(pTable++) =
				    (u32) itemAddrHdr + (j) * (len_limit);

			adma2_line_number++;
			j++;
		} while (itemTotalLen >= len_limit);

		/* left small segment desc line */
		if (itemTotalLen) {
			*(pTable++) =
			    gen_adma2_desc_low_32bit(itemTotalLen,
						     ADMA2_DESC_TRAN_VALID);

			if (dma_64bit == TRUE) {
				p64 = (u64 *) pTable;
				*p64 =
				    itemAddrHdr + ((u64) j * (u64) (len_limit));
			} else
				*(pTable++) =
				    (u32) itemAddrHdr + (j) * (len_limit);

			adma2_line_number++;
		}
	}

	return adma2_line_number;
}

dma_desc_buf_t build_adma2_desc_nop(sg_list_t *sg, u32 sg_len, byte *desc_buf,
				    u32 desc_len, bool dma_64bit,
				    bool data_26bit)
{
	dma_desc_buf_t dma = { 0 };
	sg_list_t *pAddList = 0;
	u32 *pTable = 0;
	u32 i = 0;
	/* counter for ADMA2 line number */
	int adma2_line_number = 0;
	u32 adma2_data_length = 0;
	u32 max_adma2_tb_len = 0;
	u32 adma2_item_len = 0;
	u64 *p64 = 0;

	DbgInfo(MODULE_TRANS, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s sglen=%xh 64dma=%x, 26dat=%x\n", __func__, sg_len,
		dma_64bit, data_26bit);
	/* 1. Init variables */
	pAddList = sg;
	pTable = (u32 *) (desc_buf);

	/* 2. check parameters */
	if (sg_len == 0 || desc_buf == NULL || sg == NULL) {
		DbgErr("%s invalid %d %p %p\n", __func__, sg_len, desc_buf,
		       sg);
		dma.va = 0;
		return dma;
	}
	/* 3. config dma 64bit */
	if (dma_64bit == TRUE) {
		adma2_item_len = ADMA2_128BIT_ITEM_LEN;
		max_adma2_tb_len = MAX_ADMA2_128BIT_TABLE_LEN;
	} else {
		adma2_item_len = ADMA2_ITEM_LEN;
		max_adma2_tb_len = MAX_ADMA2_TABLE_LEN;
	}

	if (desc_len < max_adma2_tb_len) {
		DbgErr("%s no enough desc_len(%d)%d\n", __func__, desc_len,
		       max_adma2_tb_len);
		dma.va = 0;
		return dma;
	}

	/* 3. clear buffer */
	os_memset(pTable, 0, max_adma2_tb_len);

	/* 4. select data length limit */
	if (data_26bit == TRUE)
		adma2_data_length = ADMA2_26BIT_LEN_SIZE;
	else
		adma2_data_length = ADMA2_16BIT_LEN_SIZE;
	/* 5.1. generate adma2 descriptor NOP lines */
	{
		*(pTable++) = gen_adma2_desc_low_32bit(0, ADMA2_DESC_INT_VALID);
		if (dma_64bit == TRUE) {
			p64 = (u64 *) pTable;
			*p64 = 0;
			pTable += 3;
		} else
			*(pTable++) = 0;
		adma2_line_number++;
	}

	/* 5.2. generate adma2 descriptor lines */
	for (i = 0; i < sg_len; i++) {
		int line_cnt = 0;

		line_cnt =
		    build_adma2_desc_line(pTable, adma2_data_length,
					  pAddList[i].Length,
					  pAddList[i].Address, dma_64bit);
		pTable += (u32) ((u64) (adma2_item_len / 4) * (u64) line_cnt);
		adma2_line_number += line_cnt;
	}

	/* 6. end table */
	adma2_end_desc_line((u8 *) pTable, dma_64bit);

	/* 7. update */
	dma.va =
	    desc_buf + (u32) ((u64) (adma2_line_number) * (u64) adma2_item_len);

	DbgInfo(MODULE_TRANS, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);

	return dma;
}

/*
 *
 * Function Name: build_adma2_desc
 *
 * Abstract:
 *
 *			build 32bit address adma2 descriptor table for SGlist.
 *
 * Input:
 *
 *			sg_list_t *sg [in]: pointer the SGlist
 *			u32 sg_len [in]: the item number of  the SGlist
 *			byte *desc_buf [in]: the buffer for ADMA2 descriptor generate
 *			u32 desc_len [in]: the buffer length
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			the adma2 descriptor line number for one SGlist item.
 * Notes:
 *
 * Caller:
 *
 */
dma_desc_buf_t build_adma2_desc(sg_list_t *sg, u32 sg_len, byte *desc_buf,
				u32 desc_len, bool dma_64bit, bool data_26bit)
{
	dma_desc_buf_t dma = { 0 };
	sg_list_t *pAddList = 0;
	u32 *pTable = 0;
	u32 i = 0;
	/* counter for ADMA2 line number */
	int adma2_line_number = 0;
	u32 adma2_data_length = 0;
	u32 max_adma2_tb_len = 0;
	u32 adma2_item_len = 0;

	DbgInfo(MODULE_TRANS, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s sglen=%xh 64dma=%x, 26dat=%x\n", __func__, sg_len,
		dma_64bit, data_26bit);
	/* 1. Init variables */
	pAddList = sg;
	pTable = (u32 *) (desc_buf);

	/* 2. check parameters */
	if (sg_len == 0 || desc_buf == NULL || sg == NULL) {
		DbgErr("%s invalid %d %p %p\n", __func__, sg_len, desc_buf,
		       sg);
		dma.va = 0;
		return dma;
	}
	/* 3. config dma 64bit */
	if (dma_64bit == TRUE) {
		adma2_item_len = ADMA2_128BIT_ITEM_LEN;
		max_adma2_tb_len = MAX_ADMA2_128BIT_TABLE_LEN;
	} else {
		adma2_item_len = ADMA2_ITEM_LEN;
		max_adma2_tb_len = MAX_ADMA2_TABLE_LEN;
	}

	if (desc_len < max_adma2_tb_len) {
		DbgErr("%s no enough desc_len(%d)%d\n", __func__, desc_len,
		       max_adma2_tb_len);
		dma.va = 0;
		return dma;
	}

	/* 3. clear buffer */
	os_memset(pTable, 0, max_adma2_tb_len);
	/* 4. select data length limit */
	if (data_26bit == TRUE)
		adma2_data_length = ADMA2_26BIT_LEN_SIZE;
	else
		adma2_data_length = ADMA2_16BIT_LEN_SIZE;
	/* 5. generate adma2 descriptor lines */
	for (i = 0; i < sg_len; i++) {
		int line_cnt = 0;

		line_cnt =
		    build_adma2_desc_line(pTable, adma2_data_length,
					  pAddList[i].Length,
					  pAddList[i].Address, dma_64bit);
		pTable += (u32) ((u64) (adma2_item_len / 4) * (u64) line_cnt);
		adma2_line_number += line_cnt;
	}

	/* 6. end table */
	adma2_end_desc_line((u8 *) pTable, dma_64bit);

	/* 7. update */
	dma.va =
	    desc_buf + (u32) ((u64) (adma2_line_number) * (u64) adma2_item_len);

	DbgInfo(MODULE_TRANS, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);

	return dma;
}

/*
 *
 * Function Name: update_adma2_inf_tb
 *
 * Abstract:
 *
 *			update  adma2 descriptor  table for infinite mode
 *
 * Input:
 *
 *			u8 *pdesc [in]: Pointer to the descriptor table
 *			u8 **link_addr [in]: the previous adma2 table link address
 *			phy_addr_t *pa [in]: the current adma2 table address
 *
 * Output:
 *
 *			u8 **link_addr [out]: the new adma2 table link address for
 *			next infinite transfer.
 *
 * Return value:
 *
 *			None
 * Notes:
 *
 * Caller:
 *
 */
bool update_adma2_inf_tb(u8 *pdesc, u8 **link_addr, phy_addr_t *pa,
			 bool dma_64bit)
{
	u32 *ptb = 0;

	/* 1.update link addr */
	if (pa != NULL) {
		*((u32 *) (*link_addr)) = os_get_phy_addr32l(*pa);
		if (dma_64bit == TRUE)
			*((u32 *) (*link_addr + 4)) = os_get_phy_addr32h(*pa);
	}

	adma2_clear_end_flag(pdesc, dma_64bit);
	ptb = (u32 *) pdesc;
	*(ptb++) = ADMA2_DESC_INT_VALID;
	*(ptb++) = 0;
	if (dma_64bit == TRUE) {
		*(ptb++) = 0;
		*(ptb++) = 0;
	}
	*(ptb++) = ADMA2_DESC_LINK_VALID;
	/* 2. save new link addr */
	(*link_addr) = (u8 *) ptb;
	*(ptb++) = 0;
	if (dma_64bit == TRUE) {
		*(ptb++) = 0;
		*(ptb++) = 0;
	}
	return TRUE;
}

/*
 *
 * Function Name: build_uhs1_cmd_desc
 *
 * Abstract:
 *
 *			build uhs1 card command descriptor table for ADMA3
 *
 * Input:
 *
 *			u8 *pdesc [in]: Pointer to the descriptor table
 *			host_trans_reg_t *regs [in] : pointer to regs for build
 *
 * Output:
 *
 *
 *
 * Return value:
 *
 *			size of usage for build
 * Notes:
 *
 * Caller:
 *
 */
static u32 build_uhs1_cmd_desc(u8 *pdesc, host_trans_reg_t *regs)
{
	u32 *ptb = (u32 *) pdesc;

	*(ptb) = ADMA3_DESC_SD_VALID;
	*(ptb + 1) = regs->block_cnt;
	*(ptb + 2) = ADMA3_DESC_SD_VALID;
	*(ptb + 3) = regs->block_size;

	/* Set argument */
	*(ptb + 4) = ADMA3_DESC_SD_VALID;
	/* uhs1 use playload[0] for argument */
	*(ptb + 5) = regs->payload[0];

	/* data cmd */
	*(ptb + 6) = ADMA3_DESC_SD_VALID;
	*(ptb + 7) = regs->trans_mode;

	return ADMA3_CMDDESC_ITEM_LENGTH * ADMA3_CMDDESC_ITEM_NUM_UHSI;
}

/*
 *
 * Function Name: build_uhs2_cmd_desc
 *
 * Abstract:
 *
 *			build uhs2 card command descriptor table for ADMA3
 *
 * Input:
 *
 *			u8 *pdesc [in]: Pointer to the descriptor table
 *			host_trans_reg_t *regs [in] : pointer to regs for build
 *
 * Output:
 *
 *
 *
 * Return value:
 *
 *			size of usage for build
 * Notes:
 *
 * Caller:
 *
 */
static u32 build_uhs2_cmd_desc(u8 *pdesc, host_trans_reg_t *regs)
{
	u32 *ptb = (u32 *) pdesc;

	*ptb = ADMA3_DESC_UHS2_VALID;
	*(ptb + 1) = regs->block_size;
	*(ptb + 2) = ADMA3_DESC_UHS2_VALID;
	*(ptb + 3) = regs->block_cnt;

	*(ptb + 4) = ADMA3_DESC_UHS2_VALID;
	/* header */
	*(ptb + 5) = regs->payload[0];

	*(ptb + 6) = ADMA3_DESC_UHS2_VALID;
	/* argument */
	*(ptb + 7) = regs->payload[1];

	*(ptb + 8) = ADMA3_DESC_UHS2_VALID;
	/* block cnt */
	*(ptb + 9) = regs->payload[2];

	*(ptb + 10) = ADMA3_DESC_UHS2_VALID;
	*(ptb + 11) = regs->payload[3];
	*(ptb + 12) = ADMA3_DESC_UHS2_VALID;
	*(ptb + 13) = regs->payload[4];
	*(ptb + 14) = ADMA3_DESC_UHS2_VALID;
	*(ptb + 15) = regs->trans_mode;

	return ADMA3_CMDDESC_ITEM_LENGTH * ADMA3_CMDDESC_ITEM_NUM_UHSII;
}

u32 build_card_cmd_desc(sd_card_t *card, u8 *desc, sd_command_t *cmd)
{
	u32 size = 0;

	if (card->card_type == CARD_UHS2) {
		byte i = 0;

		for (i = 0; i < cmd->trans_reg_cnt; i++)
			size += build_uhs2_cmd_desc(desc, &cmd->trans_reg[i]);

		return size;
	} else {
		byte i = 0;

		for (i = 0; i < cmd->trans_reg_cnt; i++)
			size += build_uhs1_cmd_desc(desc, &cmd->trans_reg[i]);

		return size;
	}

}

/*
 *
 * Function Name: build_integrated_desc
 *
 * Abstract:
 *
 *			build integrated descriptor table for ADMA3
 *
 * Input:
 *
 *			u8 *desc [in]: Pointer to descriptor buffer
 *			phy_addr_t *pa [in] : the physical address
 *			bool dma_64bit [in] : 64bit dma address
 *
 * Output:
 *
 *			None.
 *
 * Return value:
 *
 *			TRUE: build ok
 * Notes:
 *
 * Caller:
 *
 */
byte *build_integrated_desc(u8 *desc, phy_addr_t *pa, bool dma_64bit)
{
	u32 *ptb = (u32 *) desc;

	*ptb = ADMA3_INTEGRATE_DESC_VALID;
	*(ptb + 1) = os_get_phy_addr32l(*pa);
	if (dma_64bit) {
		*(ptb + 2) = os_get_phy_addr32h(*pa);
		*(ptb + 3) = 0;
		return (desc + ADMA3_INTEGRATEDDESC_128BIT_ITEM_LEN);
	} else
		return (desc + ADMA3_INTEGRATEDDESC_ITEM_LEN);
}

/*
 *
 * Function Name: adma3_end_integrated_tb
 *
 * Abstract:
 *
 *			end integrated descriptor table for ADMA3
 *
 * Input:
 *
 *			u8 *desc [in]: Pointer to descriptor buffer
 *			bool dma_64bit [in] : 64bit dma address
 *
 * Output:
 *
 *			None.
 *
 * Return value:
 *
 *			TRUE: build ok
 * Notes:
 *
 * Caller:
 *
 */
bool adma3_end_integrated_tb(u8 *desc, bool dma_64bit)
{
	u32 *ptb = 0;

	if (dma_64bit)
		desc = desc - ADMA3_INTEGRATEDDESC_128BIT_ITEM_LEN;
	else
		desc = desc - ADMA3_INTEGRATEDDESC_ITEM_LEN;
	ptb = (u32 *) desc;
	*ptb = (*ptb) | GENERAL_DESC_END_BIT;
	return TRUE;
}

/*
 *
 * Function Name: get_sdma_boudary_size
 *
 * Abstract:
 *
 *			get sdma boundary size from config
 *
 * Input:
 *
 *			cfg_item_t *cfg [in]: Pointer to config
 *
 *
 * Output:
 *
 *			None.
 *
 * Return value:
 *
 *			TRUE: byte size
 * Notes:
 *
 * Caller:
 *
 */
u32 get_sdma_boudary_size(cfg_item_t *cfg)
{
	u32 len = 0;

	len = cfg->host_item.test_sdma_boundary.value;
	len = len * 1024;
	return len;
}

/*
 *
 * Function Name: dma_align
 *
 * Abstract:
 *
 *			set dma buffer alignment
 *
 * Input:
 *
 *			dma_desc_buf_t *pdma [in]: Pointer to DMA buffer which for align
 *			u32 align_size [in]: align byte size
 *
 * Output:
 *
 *			None.
 *
 * Return value:
 *
 *			TRUE: align ok
 *			FALSE: align failed due to buffer to small to align
 * Notes:
 *
 * Caller:
 *
 */
bool dma_align(dma_desc_buf_t *pdma, u32 align_size)
{
	u32 dmaAlignOffset = 0;
	bool ret = FALSE;
	/* align dma buffer */
	dmaAlignOffset = os_get_phy_addr32l(pdma->pa) % align_size;
	if (dmaAlignOffset) {
		dmaAlignOffset = align_size - dmaAlignOffset;
		if (resize_dma_buf(pdma, dmaAlignOffset) == FALSE) {
			DbgErr("align DMA buf resize failed\n");
			ret = FALSE;
			goto exit;
		}
	}
	ret = TRUE;
exit:
	return ret;
}

/*
 *
 * Function Name:  cmd_sdma_boundary
 *
 * Abstract:
 *
 *			This Function is used to handle sdma boundary interrupt callback
 *
 * Input:
 *
 *			void *card : pointer to card
 *			void *host_request poineter to host_cmd_req_t
 *
 * Output:
 *
 *			None.
 *
 * Return value:
 *
 *			INTR_CB_OK: final DMA int for sdma
 *			INTR_CB_NOEND: will get new DMA int
 * Notes:
 *			 so giving the routine another name requires you to modify the build tools.
 * Caller:
 *
 *			test case:
 *			1.user data 512B, DMA boundary size 4KB.
 *			[no DMA int occur, so need transfer complete cb do copy]
 *			2.user data 4KB, DMA boundary size 4KB.
 *			[both DMA int & transfer complete occur]
 *			3.user data 5KB, DMA boundary size 4KB.
 *			[first DMA int occur, then transfer complete occur secondly.]
 *			4.user data 12KB, DMA boundary size 4KB.
 *			[first, second DMA int occur, then both DMA int &transfer cpl ocuur]
 *			5.user data 13KB, DMA boundary size 4KB.
 */
u32 cmd_sdma_boundary(void *pcard, void *host_request)
{
	u32 ret = INTR_CB_OK;
	sd_card_t *card = pcard;
	sd_host_t *host = card->host;
	host_cmd_req_t *req = host_request;
	sd_command_t *sd_cmd = req->private;
	sd_data_t *data = sd_cmd->data;
	data_dma_mng_t *mgr = &data->data_mng;
	u32 sdma_bd_len = get_sdma_boudary_size(host->cfg);
	bool dma_64bit = host->bit64_enable ? TRUE : FALSE;
	u32 min_size = 0;
	u32 left = 0;
	byte buhs2 = sd_cmd->uhs2_cmd;
	/* u32 device_status; */

	DbgInfo(MODULE_TRANS, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	if (mgr->offset >= mgr->total_bytess)
		return INTR_CB_OK;

	/* copy data */
	left = mgr->total_bytess - mgr->offset;
	min_size = os_min(sdma_bd_len, left);

	if (data->dir == DATA_DIR_IN) {
		os_memcpy(mgr->srb_buffer[0].buff + mgr->offset,
			  mgr->driver_buff, min_size);
		mgr->offset += min_size;
		left = mgr->total_bytess - mgr->offset;
	} else {
		/* write case */
		os_memcpy(mgr->driver_buff,
			  mgr->srb_buffer[0].buff + mgr->offset, min_size);
		left = mgr->total_bytess - mgr->offset;
		mgr->offset += min_size;
	}

	/* update return value */
	if (left >= sdma_bd_len) {
		/* will get new DMA int */
		ret = INTR_CB_NOEND;
	} else
		/* it's the last one DMA int */
		ret = INTR_CB_OK;
	/* update SDMA system address : for reuse same sdma buffer, so no need update sys_addr */
	if (left > 0) {
		if (buhs2 || host->sd_host4_enable) {
			sdhci_writel(host, SDHCI_ADMA_ADDRESS,
				     os_get_phy_addr32l(mgr->sys_addr));
			if (dma_64bit)
				sdhci_writel(host, SDHCI_ADMA_ADDRESSH,
					     os_get_phy_addr32h(mgr->sys_addr));
		} else
			sdhci_writel(host, SDHCI_DMA_ADDRESS,
				     os_get_phy_addr32l(mgr->sys_addr));
	}
	DbgInfo(MODULE_TRANS, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s ret=%x ofs=%xh,tot=%xh\n", __func__, ret,
		mgr->offset, mgr->total_bytess);
	return ret;
}

/*
 *
 * Function Name: cmd_sdma_trans_done
 *
 * Abstract:
 *
 *			handle sdma transfer done.
 *
 * Input:
 *
 *			void *card : pointer to card
 *			void *host_request poineter to host_cmd_req_t
 *
 * Output:
 *
 *			None.
 *
 * Return value:
 *
 *			INTR_CB_OK: align ok
 *
 * Notes:
 *			clear DMA if transfer complete & data reach the size
 * Caller:
 *
 */
u32 cmd_sdma_trans_done(void *pcard, void *host_request)
{
	sd_card_t *card = pcard;
	host_cmd_req_t *req = (host_cmd_req_t *) host_request;
	sd_command_t *cmd = (sd_command_t *) req->private;
	sd_data_t *data = cmd->data;
	data_dma_mng_t *mgr = &data->data_mng;
	u32 sdma_bd_len = get_sdma_boudary_size(card->host->cfg);
	u32 left = 0;

	DbgInfo(MODULE_TRANS, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	left = mgr->total_bytess - mgr->offset;
	if (left > sdma_bd_len)
		DbgErr("left data over boundary size\n");
	if (left) {
		if (data->dir == DATA_DIR_IN) {
			/* copy last data */
			os_memcpy(mgr->srb_buffer[0].buff + mgr->offset,
				  mgr->driver_buff, left);
		} else {
			/* write case */
			DbgErr("sdma trans done, but need copy\n");
			os_memcpy(mgr->driver_buff,
				  mgr->srb_buffer[0].buff + mgr->offset, left);
		}
	}
	mgr->offset += left;
	/* clear DMA if transfer complete & data reach size */
	if (mgr->total_bytess <= mgr->offset) {
		if (req->int_flag_wait & SDHCI_INT_DMA_END)
			req->int_flag_wait &= ~SDHCI_INT_DMA_END;
	}
	DbgInfo(MODULE_TRANS, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);

	return INTR_CB_OK;
}

/*
 * Function Name: cmd_adma2_inf_boundary
 * Abstract: This Function is used to handle adma2 and adma2_sdma infinite boundary intr
 *
 * Input:
 *			void *card : pointer to pcard
 *			void *host_request poineter to host_cmd_req_t
 *
 *
 * Return value:
 *			0: means ok
 *			others error
 *
 * Notes:
 *
 *        so giving the routine another name requires you to modify the build tools.
 */
u32 cmd_adma2_inf_boundary(void *pcard, void *host_request)
{
	sd_card_t *card = pcard;
	sd_host_t *host = card->host;
	host_cmd_req_t *req = host_request;
	sd_command_t *sd_cmd = req->private;
	sd_data_t *data = sd_cmd->data;
	data_dma_mng_t *mgr = &data->data_mng;
	u32 i = 0;

	/* adma2 inf case no need do any action here */
	DbgInfo(MODULE_TRANS, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (sd_cmd->gg8_ddr200_workaround) {

		if (data->dir == DATA_DIR_OUT) {
			DbgInfo(MODULE_TRANS, FEATURE_RW_TRACE, NOT_TO_RAM,
				"update output phase for write case\n");
			/* Disable SD clock */
			sdhci_and32(host, SDHCI_CLOCK_CONTROL,
				    ~(SDHCI_CLOCK_CARD_EN));

			/* update output phase */
			pci_andl(host, 0x354, 0xFFFFFF0F);
			pci_orl(host, 0x354, (host->cur_output_phase << 4));

			/* update input phase */
			sdhci_and32(card->host, SDHCI_DLL_PHASE_CFG,
				    ~0x1F000000);
			sdhci_or32(card->host, SDHCI_DLL_PHASE_CFG,
				   (BIT28) |
				   (card->output_input_phase_pair
				    [host->cur_output_phase]
				    << 24));

			/* Enable SD clock */
			sdhci_or32(host, SDHCI_CLOCK_CONTROL,
				   (SDHCI_CLOCK_CARD_EN));
		}

		/* Continue transfer */
		sdhci_or32(host, SDHCI_DRIVER_CTRL_REG,
			   SDHCI_DRIVER_CTRL_ADMA2_START_INF);
		/* sd_cmd->gg8_ddr200_workaround = 0; */
	}

	/* adma2 sdma-like inf case */
	if (data->dir == DATA_DIR_IN) {
		for (i = 0; i < mgr->srb_cnt; i++) {
			os_memcpy(mgr->srb_buffer[i].buff,
				  mgr->driver_buff + mgr->srb_buffer[i].ofs,
				  mgr->srb_buffer[i].len);
		}
	}
	DbgInfo(MODULE_TRANS, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return INTR_CB_OK;
}

/*
 * Function Name: cmd_adma2_sdma_like_trans_done
 * Abstract: This Function is used to handle adma2_sdma non-inf transfer complete
 *
 * Input:
 *			void *card : pointer to vpcard
 *			void *host_request poineter to host_cmd_req_t
 *
 *
 * Return value:
 *			0: means ok
 *			others error
 *
 * Notes:
 *
 *        so giving the routine another name requires you to modify the build tools.
 */
u32 cmd_adma2_sdma_like_trans_done(void *pcard, void *host_request)
{
	host_cmd_req_t *req = (host_cmd_req_t *) host_request;
	sd_command_t *cmd = (sd_command_t *) req->private;
	sd_data_t *data = cmd->data;
	data_dma_mng_t *mgr = &data->data_mng;
	u32 i = 0;

	DbgInfo(MODULE_TRANS, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	if (data->dir == DATA_DIR_IN) {
		for (i = 0; i < mgr->srb_cnt; i++) {
			os_memcpy(mgr->srb_buffer[i].buff,
				  mgr->driver_buff + mgr->srb_buffer[i].ofs,
				  mgr->srb_buffer[i].len);
		}
	}
	DbgInfo(MODULE_TRANS, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return INTR_CB_OK;
}

/*
 * Function Name: cmd_adma3_trans_done
 * Abstract: This Function is used to handle adma3 transfer complete
 *
 * Input:
 *			void *card : pointer to pcard
 *			void *host_request poineter to host_cmd_req_t
 *
 *
 * Return value:
 *			0: means ok
 *			others error
 *
 * Notes:
 *
 *        so giving the routine another name requires you to modify the build tools.
 */
u32 cmd_adma3_trans_done(void *pcard, void *host_request)
{
	/*
	 * for adma3 no need do anything
	 * for adma3 sdma-like, do memory copy for SRB buffer.
	 */
	return INTR_CB_OK;
}

bool dma_api_build_sdma_io(bht_dev_ext_t *pdx, sd_data_t *sd_data);
bool dma_api_build_adma_sdma_io(bht_dev_ext_t *pdx, sd_data_t *sd_data);
bool dma_api_build_adma_io(bht_dev_ext_t *pdx, sd_data_t *sd_data,
			   sg_list_t *sg, u32 sg_len);

bool build_dma_ctx(void *pdx, sd_data_t *sd_data,
		   u32 cmdflag,
		   e_data_dir dir,
		   byte *data, u32 datalen, sg_list_t *sglist, u32 sg_len)
{

	bool ret = TRUE;

	sd_data->dir = dir;
	sd_data->data_mng.driver_buff = data;
	sd_data->data_mng.total_bytess = datalen;

#if (1)
	if (cmdflag & CMD_FLG_ADMA_SDMA) {
		if (cmdflag & CMD_FLG_DDR200_WORK_AROUND)
			ret = dma_api_build_adma_sdma_io_add_nop(pdx, sd_data);
		else
			ret = dma_api_build_adma_sdma_io(pdx, sd_data);

		if (ret == FALSE) {
			DbgErr("build adma io error\n");
			ret = FALSE;
			goto exit;
		}
	}
	if (cmdflag & CMD_FLG_ADMA2) {
		if (cmdflag & CMD_FLG_DDR200_WORK_AROUND)
			ret =
			    dma_api_build_adma_io_add_nop(pdx, sd_data, sglist,
							  sg_len);
		else
			ret =
			    dma_api_build_adma_io(pdx, sd_data, sglist, sg_len);

		if (ret == FALSE) {
			DbgErr("build adma io error\n");
			ret = FALSE;
			goto exit;
		}
	}
#endif
	if (cmdflag & CMD_FLG_SDMA) {
		ret = dma_api_build_sdma_io(pdx, sd_data);
		if (ret == FALSE) {
			DbgErr("build sdma io error\n");
			ret = FALSE;
			goto exit;
		}
	}
exit:
	return ret;

}

bool dma_api_io_init(bht_dev_ext_t *pdx, dma_desc_buf_t *desc_buf)
{
	node_t *node = &pdx->dma_api.dma_node;
	node_t *node2 = &pdx->dma_api.dma_node2;
	bool ret = FALSE;
	/* 1. check size */
	if (pdx->dump_mode == FALSE) {
		if (desc_buf->len < (MIN_DMA_API_BUF_SIZE)) {
			ret = FALSE;
			DbgErr("dma buf too small 0x%x <=(%x)\n", desc_buf->len,
			       (MIN_DMA_API_BUF_SIZE));
			goto exit;
		}
	}
	/* 2. assign buf */
	node->general_desc_tbl = *desc_buf;
	node->general_desc_tbl.len = MAX_GENERAL_DESC_TABLE_LEN;
	node->general_desc_tbl_img = node->general_desc_tbl;
	ret = resize_dma_buf(desc_buf, MAX_GENERAL_DESC_TABLE_LEN);
	if (ret == FALSE) {
		ret = FALSE;
		goto exit;
	}

	pdx->dma_api.cur_node = NULL;
	/* for dump mode we only use adma2 mode */
	if (pdx->dump_mode == TRUE) {
		node2->general_desc_tbl = *desc_buf;
		node2->general_desc_tbl.len = MAX_GENERAL_DESC_TABLE_LEN;
		node2->general_desc_tbl_img = node2->general_desc_tbl;
		ret = resize_dma_buf(desc_buf, MAX_GENERAL_DESC_TABLE_LEN);
		if (ret == FALSE) {
			DbgErr("Allocate node2 for dump mode failed\n");
			ret = FALSE;
			goto exit;
		}

		ret = TRUE;
		goto exit;
	}

	/* 3. align dma buffer for adma2 API buffer */
	if (dma_align(desc_buf, DMA_BUF_ALIGN_SIZE) == FALSE) {
		DbgErr("tq adma2 API buffer align failed\n");
		ret = FALSE;
		goto exit;
	}
	/* 4. allocate adma2 API buffer resource */
	node->data_tbl = *desc_buf;
	node->data_tbl.len = DMA_API_BUF_SIZE;
	node->data_tbl_img = node->data_tbl;
	DbgInfo(MODULE_TQ_DMA, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"adma2 api buf len %x pa(%x)\n", node->data_tbl.len,
		os_get_phy_addr32l(node->data_tbl.pa));
	/* update for dma buf usage */
	if (resize_dma_buf(desc_buf, DMA_API_BUF_SIZE) == FALSE) {
		ret = FALSE;
		DbgErr("%s adm2 API buf resize failed\n", __func__);
		goto exit;
	}
	ret = TRUE;
	/* DbgErr("DMA API desc %x , data %x\n",node->general_desc_tbl.pa,node->data_tbl.pa); */
exit:

	return ret;
}

/*
 *
 * Function Name:node_get_desc_res
 *
 * Abstract:
 *
 *			get node descriptor resource
 *
 * Input:
 *
 *			node_t *node [in]: Pointer to node
 *			u32 max_use_size [in]: the max maybe use size for descriptor table
 *
 * Output:
 *
 *
 *
 * Return value:
 *
 *			dma_desc_buf_t *: NULL means failed to get the descriptor resource.
 *			other means get ok.
 * Notes:
 *
 * Caller:
 *
 */

dma_desc_buf_t *node_get_desc_res(node_t *node, u32 max_use_size)
{
	dma_desc_buf_t *p = &node->general_desc_tbl;

	if (max_use_size > p->len) {
		DbgErr("%s no enough buf for desc\n", __func__);
		return NULL;
	}
	return &node->general_desc_tbl;
}

bool _adma_only_build_io(sg_list_t *sg, u32 sg_len, bool dma_64bit,
			 bool data_26bit_len, dma_desc_buf_t *end_dma,
			 data_dma_mng_t *mgr, dma_desc_buf_t *dma)
{
	bool ret = FALSE;

	*end_dma =
	    build_adma2_desc(sg, sg_len, (byte *) dma->va, dma->len, dma_64bit,
			     data_26bit_len);
	if (end_dma->va == NULL) {
		DbgErr("%s build adma2 desc failed\n", __func__);
		ret = FALSE;
		goto exit;
	}

	mgr->sys_addr = dma->pa;
	ret = TRUE;
exit:
	return ret;

}

void dbg_dump_general_desc_tb(u8 *desc, u32 size)
{
	u32 i = 0;
	u32 *pTable = (u32 *) desc;

	size = size / (sizeof(u32) * 2);
	/*
	 * for some case, need dump more
	 * dump more for 128bit infinite int + link case
	 */
	size += 4;

#define MAX_DUMP_DESC_SIZE (1024 * 16)
	if (size > MAX_DUMP_DESC_SIZE) {
		DbgInfo(MODULE_TQ_DMA, FEATURE_FUNC_DESC, NOT_TO_RAM,
			"%s over limit %x\n", __func__, size);
		size = MAX_DUMP_DESC_SIZE;
	}

	for (i = 0; i < size; i++) {
		DbgErr(" [0x%0.8Xh], [0x%0.8x]\n",
		       pTable[(i * 2) + 1], pTable[i * 2]);
	}
}

u32 pp_ofs(byte *ph, byte *pl)
{
	u64 ofs = 0;

	ofs = ph - pl;
	if (ofs >= 0xffffffff) {
		DbgErr("%s:(%x)maybe over 32bit size\n", __func__, ofs);
		return 0;
	}
	return (u32) ofs;
}

void dump_adma2_desc(u8 *desc, u8 *desc_end)
{
	u32 size = 0;

	size = pp_ofs(desc_end, desc);
	dbg_dump_general_desc_tb(desc, size);
}

/*
 *
 * Function Name:dump_node_adma2_desc
 *
 * Abstract:
 *
 *			dump node adma2 desc
 *
 * Input:
 *
 *
 * Output:
 *
 *
 *
 * Return value:
 *
 * Notes:
 *
 * Caller:
 *
 */
bool dump_node_adma2_desc(node_t *node, void *ctx)
{
	phy_addr_t sys_addr;
	u8 *desc = node->phy_node_buffer.head.va;
	u8 *desc_end = node->phy_node_buffer.end.va;

	sys_addr = node->phy_node_buffer.head.pa;
	DbgErr("sys addrl %x addrh %x\n", os_get_phy_addr32l(sys_addr),
	       os_get_phy_addr32h(sys_addr));
	dump_adma2_desc(desc, desc_end);
	return TRUE;
}

bool _dma_api_build_adma_io(node_t *node, sg_list_t *sg, u32 sg_len,
			    bool dma_64bit, bool data_26bit_len,
			    sd_data_t *sd_data)
{
	bool ret = FALSE;
	dma_desc_buf_t *pdma = 0;
	u32 adma2_size = 0;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* 1. get ADMA2 desc buffer */
	adma2_size =
	    (TRUE ==
	     dma_64bit) ? MAX_ADMA2_128BIT_TABLE_LEN : MAX_ADMA2_TABLE_LEN;
	pdma = node_get_desc_res(node, adma2_size);
	if (pdma == NULL) {
		DbgErr("%s node get desc failed\n", __func__);
		ret = FALSE;
		goto exit;
	}
	node->phy_node_buffer.head = *pdma;
	/* 2. build ADMA2 Desc */
	ret =
	    _adma_only_build_io(sg, sg_len, dma_64bit, data_26bit_len,
				&node->phy_node_buffer.end, &sd_data->data_mng,
				pdma);
exit:
	/* dump_node_adma2_desc(node,NULL); */
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s ret=%d\n", __func__, ret);
	return ret;

}

bool _dma_api_build_adma_io_add_nop(node_t *node, sg_list_t *sg, u32 sg_len,
				    bool dma_64bit, bool data_26bit_len,
				    sd_data_t *sd_data)
{
	bool ret = FALSE;
	dma_desc_buf_t *pdma = 0;
	u32 adma2_size = 0;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* 1. get ADMA2 desc buffer */
	adma2_size =
	    (TRUE ==
	     dma_64bit) ? MAX_ADMA2_128BIT_TABLE_LEN : MAX_ADMA2_TABLE_LEN;
	pdma = node_get_desc_res(node, adma2_size);
	if (pdma == NULL) {
		DbgErr("%s node get desc failed\n", __func__);
		ret = FALSE;
		goto exit;
	}
	node->phy_node_buffer.head = *pdma;

	node->phy_node_buffer.end =
	    build_adma2_desc_nop(sg, sg_len, (byte *) pdma->va, pdma->len,
				 dma_64bit, data_26bit_len);
	if (node->phy_node_buffer.end.va == NULL) {
		DbgErr("%s build adma2 desc failed\n", __func__);
		ret = FALSE;
		goto exit;
	}

	/* generate DMA INT at end */
	{
		u32 *ptb = 0;

		adma2_clear_end_flag(node->phy_node_buffer.end.va, dma_64bit);
		ptb = (u32 *) node->phy_node_buffer.end.va;
		*(ptb++) = ADMA2_DESC_INT_VALID;
		*(ptb++) = 0;
		if (dma_64bit == TRUE) {
			*(ptb++) = 0;
			*(ptb++) = 0;
		}
	}

	sd_data->data_mng.sys_addr = pdma->pa;
	ret = TRUE;

exit:
	dump_node_adma2_desc(node, NULL);
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s ret=%d\n", __func__, ret);
	return ret;

}

void adma_sdma_post_io(data_dma_mng_t *mgr, e_data_dir dir, byte *data_buf)
{
	if (dir == DATA_DIR_OUT)
		os_memcpy(data_buf, mgr->driver_buff, mgr->total_bytess);
	if (dir == DATA_DIR_IN) {
		mgr->srb_cnt = 1;
		mgr->srb_buffer[0].buff = mgr->driver_buff;
		mgr->srb_buffer[0].len = mgr->total_bytess;
		mgr->srb_buffer[0].ofs = 0;
		mgr->driver_buff = data_buf;
	}
}

/*
 *
 * Function Name: gen_sdma_like_sgl
 *
 * Abstract:
 *
 *			generate sdma-like SGlist table
 *
 * Input:
 *
 *			request_t *req [in]: Pointer to the request for build
 *			dma_desc_buf_t *pdma [in]:pointer to sdma-like buffer
 *
 *
 * Output:
 *
 *			None.
 *
 * Return value:
 *
 *			TRUE: build ok
 * Notes:
 *
 * Caller:
 *
 */
bool gen_sdma_like_sgl(request_t *req, dma_desc_buf_t *pdma)
{
	sg_list_t *sg;

	sg = req->srb_sg_list;
	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	if ((pdma->va == 0) && (pdma->len == 0)) {
		DbgErr("%s null va\n", __func__);
		return FALSE;
	}

	/* for 64bit case */
	sg[0].Address = os_get_phy_addr64(pdma->pa);
	sg[0].Length = req->tag_req_t.sec_cnt * SD_BLOCK_LEN;
	req->srb_sg_len = 1;

	if (pdma->len < sg[0].Length) {
		DbgErr("%s dma buf too small\n", __func__);
		return FALSE;
	}

	DbgInfo(MODULE_TQ_DMA, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return TRUE;
}

bool adma_sdma_gen_sglist(node_t *node, data_dma_mng_t *mgr, request_t *req)
{
	bool ret = FALSE;

	dma_desc_buf_t *pdma = 0;

	if (mgr->total_bytess > DMA_API_BUF_SIZE) {
		DbgErr("%s data total bytes too large(%x)>(%x)\n", __func__,
		       mgr->total_bytess, DMA_API_BUF_SIZE);
		ret = FALSE;
		goto exit;
	}

	/* build srb_ext */
	req->tag_req_t.sec_cnt = mgr->total_bytess / SD_BLOCK_LEN;

	/* 1. get sdma like buf address */
	pdma = &node->data_tbl;

	/* 2. generate sdma like sglist table */
	if (gen_sdma_like_sgl(req, pdma) == FALSE) {
		DbgErr("%s gen sdma-like sgl failed\n", __func__);
		ret = FALSE;
		goto exit;
	}

	ret = TRUE;
exit:

	return ret;
}

bool dma_api_build_adma_io(bht_dev_ext_t *pdx, sd_data_t *sd_data,
			   sg_list_t *sg, u32 sg_len)
{
	node_t *node = &pdx->dma_api.dma_node;

	bool ret = FALSE;

	bool dma_64bit = pdx->card.host->bit64_enable ? TRUE : FALSE;
	bool data_26bit_len =
	    pdx->cfg->host_item.test_dma_mode_setting.enable_dma_26bit_len
		? TRUE : FALSE;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* 3. get ADMA2 desc buffer */
	ret =
	    _dma_api_build_adma_io(node, sg, sg_len, dma_64bit, data_26bit_len,
				   sd_data);

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s ret=%d\n", __func__, ret);
	return ret;

}

bool dma_api_build_adma_io_add_nop(bht_dev_ext_t *pdx, sd_data_t *sd_data,
				   sg_list_t *sg, u32 sg_len)
{
	node_t *node = &pdx->dma_api.dma_node;

	bool ret = FALSE;
	bool dma_64bit = pdx->card.host->bit64_enable ? TRUE : FALSE;
	bool data_26bit_len =
	    pdx->cfg->host_item.test_dma_mode_setting.enable_dma_26bit_len
		? TRUE : FALSE;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	ret =
	    _dma_api_build_adma_io_add_nop(node, sg, sg_len, dma_64bit,
					   data_26bit_len, sd_data);

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s ret=%d\n", __func__, ret);
	return ret;

}

request_t req;

bool dma_api_build_adma_sdma_io(bht_dev_ext_t *pdx, sd_data_t *sd_data)
{
	node_t *node = &pdx->dma_api.dma_node;
	/* request_t  req ; */
	bool ret = FALSE;

	bool dma_64bit = pdx->card.host->bit64_enable ? TRUE : FALSE;
	bool data_26bit_len =
	    pdx->cfg->host_item.test_dma_mode_setting.enable_dma_26bit_len
		? TRUE : FALSE;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* check buf size */
	ret = adma_sdma_gen_sglist(node, &sd_data->data_mng, &req);
	if (ret == FALSE)
		goto exit;
	/* dump */
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
		"dump sg %llx,%x\n", req.srb_sg_list[0].Address,
		req.srb_sg_len);

	/* 3. get ADMA2 desc buffer */
	ret =
	    _dma_api_build_adma_io(node, req.srb_sg_list, req.srb_sg_len,
				   dma_64bit, data_26bit_len, sd_data);
	/* 4. */
	adma_sdma_post_io(&sd_data->data_mng, sd_data->dir, node->data_tbl.va);

exit:
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s ret=%d\n", __func__, ret);
	return ret;

}

bool dma_api_build_adma_sdma_io_add_nop(bht_dev_ext_t *pdx,
					sd_data_t *sd_data)
{
	node_t *node = &pdx->dma_api.dma_node;
	/* request_t  req ; */
	bool ret = FALSE;

	bool dma_64bit = pdx->card.host->bit64_enable ? TRUE : FALSE;
	bool data_26bit_len =
	    pdx->cfg->host_item.test_dma_mode_setting.enable_dma_26bit_len
		? TRUE : FALSE;

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* check buf size */
	ret = adma_sdma_gen_sglist(node, &sd_data->data_mng, &req);
	if (ret == FALSE)
		goto exit;
	/* dump */
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
		"dump sg %llx,%x\n", req.srb_sg_list[0].Address,
		req.srb_sg_len);

	/* 3. get ADMA2 desc buffer */
	ret =
	    _dma_api_build_adma_io_add_nop(node, req.srb_sg_list,
					   req.srb_sg_len, dma_64bit,
					   data_26bit_len, sd_data);
	/* 4. */
	adma_sdma_post_io(&sd_data->data_mng, sd_data->dir, node->data_tbl.va);

exit:
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s ret=%d\n", __func__, ret);
	return ret;

}

/*
 * only support one data buffer SDMA transfer, PIO like .
 */
bool _sdma_build_io(data_dma_mng_t *mgr, dma_desc_buf_t *dma, u32 sdma_bd_len,
		    e_data_dir dir, byte *data_buf)
{
	u32 min_size = 0;

	mgr->srb_buffer[0].buff = data_buf;
	mgr->offset = 0;
	/* fix to 1 */
	mgr->srb_cnt = 1;

	/* system addr */
	mgr->sys_addr = dma->pa;
	mgr->driver_buff = (byte *) dma->va;
	/* for write data to card,need fill data first before transfer */
	if (dir == DATA_DIR_OUT) {
		min_size = os_min(sdma_bd_len, mgr->total_bytess);
		os_memcpy(mgr->driver_buff,
			  mgr->srb_buffer[0].buff + mgr->offset, min_size);

		mgr->offset += min_size;
	}
	return TRUE;

}

bool dma_api_build_sdma_io(bht_dev_ext_t *pdx, sd_data_t *sd_data)
{
	node_t *node = &pdx->dma_api.dma_node;
	bool ret = FALSE;
	dma_desc_buf_t dma;
	data_dma_mng_t *mgr = &sd_data->data_mng;
	u32 sdma_bd_len = get_sdma_boudary_size(pdx->cfg);

	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* check buf size */
	if (sd_data->data_mng.total_bytess > DMA_API_BUF_SIZE) {
		DbgErr("%s data total bytes too large(%x)>(%x)\n", __func__,
		       sd_data->data_mng.total_bytess, DMA_API_BUF_SIZE);
		ret = FALSE;
		goto exit;
	}

	/* align dma buffer */
#define SDMA_BOUNDARY_MAX_SIZE (512*1024)
	if (sdma_bd_len > SDMA_BOUNDARY_MAX_SIZE) {
		DbgErr("%s boundary over max %x\n", __func__, sdma_bd_len);
		ret = FALSE;
		goto exit;
	} else {
		dma = node->data_tbl;
		if (dma_align(&dma, sdma_bd_len) == FALSE) {
			DbgErr("%s align failed\n", __func__);
			ret = FALSE;
			goto exit;
		}
	}

	ret =
	    _sdma_build_io(mgr, &dma, sdma_bd_len, sd_data->dir,
			   sd_data->data_mng.driver_buff);

exit:
	DbgInfo(MODULE_TQ_FLOW, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s ret=%d\n", __func__, ret);
	return ret;

}
