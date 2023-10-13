// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 BHT Inc.
 *
 * File Name: linux_scsi.c
 *
 * Abstract: SCSI function
 *
 * Version: 1.00
 *
 * Author: Peter.Guo
 *
 * Environment:	Linux
 *
 * History:
 *
 * 5/20/2015		Creation	Peter.Guo
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/pm.h>
#include <linux/mempool.h>

#include "../include/basic.h"
#include "../include/reqapi.h"
#include "../include/funcapi.h"
#include "../include/tqapi.h"
#include "../include/cardapi.h"
#include "../include/hostapi.h"
#include "../include/debug.h"
#include "../include/util.h"
#include "linux_scsi.h"
#include "../tagqueue/tq_trans_api.h"
#include "../include/cmdhandler.h"

extern struct scsi_host_template bht_scsi_template;

static srb_ext_t *bht_scsi_alloc_srb_ext(bht_dev_ext_t *pdx,
					 struct scsi_cmnd *srb)
{
	/* todo use cache to improve performance */
	srb_ext_t *srb_ext = mempool_alloc(bht_sd_mem_pool, GFP_ATOMIC);

	if (srb_ext == NULL) {
		DbgErr("alloc srbext is null\n");

		goto exit;
	}
	memset(srb_ext, 0, sizeof(srb_ext_t));
	srb_ext->psrb = srb;
exit:
	return srb_ext;
}

static void bht_scsi_free_srb_ext(bht_dev_ext_t *pdx, srb_ext_t *srb_ext)
{
	if (srb_ext != NULL)
		mempool_free(srb_ext, bht_sd_mem_pool);
}

/*
 *	The function to register and Init scsi host
 */
bool bht_scsi_init(bht_dev_ext_t *pdx, struct device *dev)
{
	bool ret = FALSE;
	int error = 0;

	DbgInfo(MODULE_OS_ENTRY, FEATURE_FUNC_TRACE, NOT_TO_RAM,
		"BHT scsi init begin\n");

	if (pdx->scsi_init_flag == 1) {
		DbgInfo(MODULE_OS_ENTRY, FEATURE_FUNC_TRACE, NOT_TO_RAM,
			"BHT scsi initialized, and return\n");
		return TRUE;
	}

	bht_scsi_template.can_queue = bht_scsi_template.cmd_per_lun =
	    os_min(pdx->cfg->host_item.test_tag_queue_capability.max_srb,
		   LINUX_SCSI_MAX_QUEUE_DPETH);
	pdx->os.scsi_host =
	    scsi_host_alloc(&bht_scsi_template, sizeof(unsigned long));
	if (!pdx->os.scsi_host) {
		DbgErr("Unable to register controller with SCSI subsystem\n");
		goto exit;
	}

	pdx->dev = dev;
	pdx->os.scsi_host->hostdata[0] = (unsigned long)pdx;
	pdx->os.scsi_host->irq = pdx->host.pci_dev.irq;
	pdx->os.scsi_host->base = (unsigned long)pdx->host.pci_dev.membase;
	pdx->os.scsi_host->max_id = 1;
	pdx->os.scsi_host->max_lun = 1;
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 13, 0)
	pdx->os.scsi_host->no_write_same = 1;
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
	pdx->os.scsi_host->use_clustering = 1;
#endif
	pdx->os.scsi_host->max_sectors = bht_scsi_template.max_sectors;

	error = scsi_add_host(pdx->os.scsi_host, dev);
	if (error) {
		DbgErr("scsi add host failed\n");
		return FALSE;
	}
	scsi_scan_host(pdx->os.scsi_host);
	pdx->scsi_init_flag = 1;
	ret = TRUE;

exit:
	DbgInfo(MODULE_OS_ENTRY, FEATURE_FUNC_TRACE, NOT_TO_RAM,
		"BHT scsi init end\n");
	return ret;
}

/*
 *	The function to remove scsi host
 */
void bht_scsi_uinit(bht_dev_ext_t *pdx)
{
	DbgInfo(MODULE_OS_ENTRY, FEATURE_FUNC_TRACE, NOT_TO_RAM,
		"BHT scsi uinit begin\n");
	if (pdx->scsi_init_flag == 0) {
		DbgInfo(MODULE_OS_ENTRY, FEATURE_FUNC_TRACE, NOT_TO_RAM,
			"BHT scsi uinit excuted,and return\n");
		return;
	}
	if ((pdx != NULL) && (&pdx->os != NULL) && (pdx->os.scsi_host != NULL)) {
		scsi_remove_host(pdx->os.scsi_host);
		scsi_host_put(pdx->os.scsi_host);
		pdx->scsi_init_flag = 0;
	}
	DbgInfo(MODULE_OS_ENTRY, FEATURE_FUNC_TRACE, NOT_TO_RAM,
		"BHT scsi uinit end\n");
}

/*
 *	This function is used to save the sense code and fill sense buffer
 */
static void bht_scsi_set_sensecode(bht_dev_ext_t *pdx, struct scsi_cmnd *srb,
				   byte sense_key, byte sense_code)
{
	PSENSE_DATA senseBuffer = (PSENSE_DATA) srb->sense_buffer;

	pdx->scsi.sense_key = sense_key;
	pdx->scsi.sense_code = sense_code;

	if (senseBuffer != NULL && sense_key != 0) {
		senseBuffer->ErrorCode = 0x70;
		senseBuffer->AdditionalSenseLength = 0xb;
		senseBuffer->SenseKey = pdx->scsi.sense_key;
		senseBuffer->AdditionalSenseCode = pdx->scsi.sense_code;
		senseBuffer->AdditionalSenseCodeQualifier = 0;
		DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
			"%s sense=%d code=%d\n", __func__, sense_key,
			sense_code);
	}

}

/*
 *	Handle Scsi command Test unit ready
 */
static void bht_scsi_test_unit_ready(bht_dev_ext_t *pdx, struct scsi_cmnd *srb)
{
	if (req_card_ready(pdx)) {
		srb->result = GOOD;
	} else {
		srb->result = CHECK_CONDITION;
		bht_scsi_set_sensecode(pdx, srb, SCSI_SENSE_NOT_READY,
				       SCSI_ADSENSE_NO_MEDIA_IN_DEVICE);
	}
	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		" %s,  SrbResult:%x\n", __func__, srb->result);
}

/*
 *	Handle SCSI Command Request Sense
 */
static void bht_scsi_request_sense(bht_dev_ext_t *pdx, struct scsi_cmnd *srb)
{

	PSENSE_DATA senseBuffer = (PSENSE_DATA) srb->sense_buffer;

	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Enter %s\n", __func__);
	if (senseBuffer == NULL) {
		srb->result = CHECK_CONDITION;
	} else {
		srb->result = GOOD;
		senseBuffer->ErrorCode = 0x70;
		senseBuffer->AdditionalSenseLength = 0xb;
		senseBuffer->SenseKey = pdx->scsi.sense_key;
		senseBuffer->AdditionalSenseCode = pdx->scsi.sense_code;
		senseBuffer->AdditionalSenseCodeQualifier = 0;
		bht_scsi_set_sensecode(pdx, srb, 0, 0);
	}

	scsi_sg_copy_from_buffer(srb, (byte *) senseBuffer,
				 os_min(srb->sdb.length, sizeof(SENSE_DATA)));
	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Exit %s, SrbResult=%d\n", __func__, srb->result);
}

/*
 *	Handle Mode Sense scsi command
 */
static void bht_scsi_exec_modesense(bht_dev_ext_t *pdx, struct scsi_cmnd *srb)
{
	MODE_PAGE_HEADER hdr;
	MODE_PAGE_8 pg_8;
	MODEPAGE19 pg_9;
	byte pmdata[512];
	byte mode_code;
	sd_card_t *card = &pdx->card;

	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Enter %s\n", __func__);
	memset(pmdata, 0, 512);

	hdr.DataLength = 3;
	hdr.MediumType = 0x00;
	hdr.Reserved = 0;

	if (card->write_protected)
		hdr.Reserved |= 0x80;

	hdr.BlockDescLength = 0;
	pg_8.PageCode = 0x08;
	pg_8.PageSavable = 1;
	pg_8.PageLength = 0x12;

	pg_8.WriteCacheEnable = 1;
	pg_8.DisablePrefetchTransfer[0] = 0xFF;
	pg_8.DisablePrefetchTransfer[1] = 0xFF;
	pg_8.MaximumPrefetch[0] = 0xFF;
	pg_8.MaximumPrefetch[1] = 0xFF;
	pg_8.MaximumPrefetchCeiling[0] = 0xFF;
	pg_8.MaximumPrefetchCeiling[1] = 0xFF;
	pg_8.NumberofCacheSegments = 0x20;

	pg_9.PageCode = 0x19;
	pg_9.PageSavable = 1;
	pg_9.PageLength = 0x04;
	pg_9.ProtocolIdentifier = 0x0A;
	pg_9.SynchronousTransferTimeout[0] = 0x00;
	pg_9.SynchronousTransferTimeout[1] = 0x01;

	mode_code = srb->cmnd[2];

	switch (mode_code) {
	case 0x08:
		hdr.DataLength += sizeof(MODE_PAGE_8);
		memcpy(pmdata, &hdr, sizeof(MODE_PAGE_HEADER));
		memcpy(pmdata + sizeof(MODE_PAGE_HEADER), &pg_8,
		       sizeof(MODE_PAGE_8));
		break;
	case 0x19:
		hdr.DataLength += sizeof(MODEPAGE19);
		memcpy(pmdata, &hdr, sizeof(MODE_PAGE_HEADER));
		memcpy(pmdata + sizeof(MODE_PAGE_HEADER), &pg_9,
		       sizeof(MODEPAGE19));
		break;
	case 0x3F:
		hdr.DataLength += (sizeof(MODEPAGE19) + sizeof(MODE_PAGE_8));
		memcpy(pmdata, &hdr, sizeof(MODE_PAGE_HEADER));
		memcpy(pmdata + sizeof(MODE_PAGE_HEADER), &pg_8,
		       sizeof(MODE_PAGE_8));
		memcpy(pmdata + sizeof(MODE_PAGE_HEADER) + sizeof(MODE_PAGE_8),
		       &pg_9, sizeof(MODEPAGE19));
		break;
	default:
		break;
	}

	srb->result = GOOD;
	scsi_sg_copy_from_buffer(srb, pmdata, os_min(srb->sdb.length, 512));
	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Exit %s\n", __func__);
}

/*
 *	Handle SCSI Command Allow Removalabe
 */
static void bht_scsi_allow_removal(bht_dev_ext_t *pdx, struct scsi_cmnd *srb)
{
	byte prevent = srb->cmnd[4] & 0x3;

	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Enter %s prevent=%d\n", __func__, prevent);
	if (req_card_ready(pdx) == FALSE) {
		srb->result = CHECK_CONDITION;

		bht_scsi_set_sensecode(pdx, srb, SCSI_SENSE_NOT_READY,
				       SCSI_ADSENSE_NO_MEDIA_IN_DEVICE);
	} else {
		srb->result = GOOD;
		if (prevent == 0x01 || prevent == 0x11)
			pdx->scsi.prevent_eject = 1;
		else
			pdx->scsi.prevent_eject = 0;
	}
	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Exit %s\n", __func__);
}

static bool bht_scsi_error_handle(bht_dev_ext_t *pdx, struct scsi_cmnd *srb,
				  e_req_result result)
{
	bool ret = TRUE;

	if (result == REQ_RESULT_PENDING) {
	} else if (result == REQ_RESULT_QUEUE_BUSY) {
	} else if (result == REQ_RESULT_NO_CARD) {
		srb->result = CHECK_CONDITION;
		bht_scsi_set_sensecode(pdx, srb, SCSI_SENSE_NOT_READY,
				       SCSI_ADSENSE_NO_MEDIA_IN_DEVICE);
	} else if (result == REQ_RESULT_PROTECTED) {
		srb->result = CHECK_CONDITION;
		bht_scsi_set_sensecode(pdx, srb, SCSI_SENSE_DATA_PROTECT,
				       SCSI_ADSENSE_WRITE_PROTECT);
	} else {
		srb->result = CHECK_CONDITION;
		bht_scsi_set_sensecode(pdx, srb, SCSI_SENSE_ILLEGAL_REQUEST,
				       SCSI_ADSENSE_ILLEGAL_BLOCK);
	}

	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		" %s, SrbResult:%x\n", __func__, srb->result);
	return ret;
}

/*
 *	Fill Capacity Data
 */
static void bht_scsi_get_capacity(bht_dev_ext_t *pdx, u32 secnt,
				  struct scsi_cmnd *srb)
{
	READ_CAPACITY_DATA capacity;
	PREAD_CAPACITY_DATA pcapacity = &capacity;

	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Enter %s\n", __func__);

	memset(pcapacity, 0, sizeof(READ_CAPACITY_DATA));
	/* Big Endian for Capacity data */
	pcapacity->LogicalBlockAddress = swapu32((secnt > 0) ? (secnt - 1) : 0);
	/* Big Endian for Capacity data */
	pcapacity->BytesPerBlock = swapu32(SD_BLOCK_LEN);
	srb->result = GOOD;
	scsi_sg_copy_from_buffer(srb, pcapacity,
				 os_min(srb->sdb.length,
					sizeof(READ_CAPACITY_DATA)));
	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Exit %s, capacity:%x\n", __func__,
		pcapacity->LogicalBlockAddress);
}

/*
 *	Fill Inquriy Data function
 */
static void bht_scsi_get_inquiry(bht_dev_ext_t *pdx, e_card_type card_type,
				 struct scsi_cmnd *srb)
{
	_INQUIRYDATA inqdata;
	_INQUIRYDATA *pinqdata = &inqdata;

	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Enter %s\n", __func__);

	os_memset(pinqdata, 0, sizeof(_INQUIRYDATA));
	/* TYPE_DISK; */
	pinqdata->DeviceType = 0x00;

	pinqdata->Versions = 0x00;
	pinqdata->ResponseDataFormat = 0x02;
	pinqdata->AdditionalLength = sizeof(_INQUIRYDATA) - 4;
	strncpy((char *)(pinqdata->VendorId), "BHT-SD ", 8);
	switch (card_type) {
	case CARD_SDIO:
		strncpy((char *)(pinqdata->ProductId), "SDIO ", 5);
		break;
	case CARD_SD:
	case CARD_UHS2:
		strncpy((char *)(pinqdata->ProductId), "SD ", 3);
		break;
	case CARD_EMMC:
	case CARD_MMC:
		strncpy((char *)(pinqdata->ProductId), "MMC ", 4);
		break;
	case CARD_TEST:
		strncpy((char *)(pinqdata->ProductId), "TestCR ", 6);
		break;
	default:
		strncpy((char *)(pinqdata->ProductId), "CR              ", 16);
		break;
	}
	strncpy((char *)(pinqdata->ProductRevisionLevel), "104a", 4);
	srb->result = GOOD;
	scsi_sg_copy_from_buffer(srb, pinqdata,
				 os_min(srb->sdb.length, sizeof(_INQUIRYDATA)));
	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Exit %s\n", __func__);
}

/*
 *	Callback for basci scsi command
 */
static void bht_scsi_srb_basic_cmd_done(void *p, void *srb_ext)
{
	srb_ext_t *p_srb_ext = srb_ext;
	bht_dev_ext_t *pdx = p;
	struct scsi_cmnd *srb = p_srb_ext->psrb;
	sd_card_t *card = &pdx->card;

	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Enter %s\n", __func__);
	if (srb == NULL) {
		pdx->p_srb_ext = NULL;
		goto exit;
	}
	if (p_srb_ext->req.result != REQ_RESULT_OK) {
		srb->result = CHECK_CONDITION;
		bht_scsi_set_sensecode(pdx, srb, SCSI_SENSE_NOT_READY,
				       SCSI_ADSENSE_NO_MEDIA_IN_DEVICE);
		goto end;
	}

	switch (srb->cmnd[0]) {
	case INQUIRY:
		{
			bht_scsi_get_inquiry(pdx, card->card_type, srb);
			break;
		}
	case READ_CAPACITY:
		{
			bht_scsi_get_capacity(pdx, (u32) card->sec_count, srb);
			break;
		}
	case START_STOP:
		{
			srb->result = GOOD;
			break;
		}
	default:
		break;
	}

end:
	bht_scsi_free_srb_ext(pdx, srb_ext);
	pdx->p_srb_ext = NULL;

	scsi_done(srb);
exit:
	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Exit %s\n", __func__);
}

/*
 *	handle start stop scsi command
 *	return true means done while return false means pending
 */
static bool bht_scsi_load_unload(bht_dev_ext_t *pdx, struct scsi_cmnd *srb,
				 bool *busy)
{
	bool ret = TRUE;
	byte load = srb->cmnd[4] & 0x01;
	byte loej = srb->cmnd[4] & 0x02;
	e_req_result result = REQ_RESULT_OK;
	srb_ext_t *srb_ext = NULL;

	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Enter %s load=%d loej=%d\n", __func__, load, loej);

	if (loej == 0) {
		srb->result = GOOD;
		goto exit;
	}

	if (load) {
		/*
		 * For Load command we don't need use thread
		 * because card access module can Init card
		 */

		pdx->scsi.scsi_eject = 0;
		srb->result = GOOD;

		if (req_card_ready(pdx) == FALSE) {
			srb->result = CHECK_CONDITION;
			DbgErr("Scsi load failed\n");
			bht_scsi_set_sensecode(pdx, srb, SCSI_SENSE_NOT_READY,
					       SCSI_ADSENSE_NO_MEDIA_IN_DEVICE);
		}
		goto exit;
	} else {
		/* already ejected */
		if (pdx->scsi.scsi_eject) {
			srb->result = GOOD;
			goto exit;
		}
		/* eject is not allowed */
		else if (pdx->scsi.prevent_eject) {
			srb->result = CHECK_CONDITION;
			bht_scsi_set_sensecode(pdx, srb,
					       SCSI_SENSE_ILLEGAL_REQUEST,
					       SCSI_ADSENSE_ILLEGAL_COMMAND);
			goto exit;
		}
	}

	/* below code is for unload operation       */
	srb_ext = bht_scsi_alloc_srb_ext(pdx, srb);
	if (srb_ext == NULL) {
		result = REQ_RESULT_ABORT;
		goto exit;
	}
	srb_ext->req.data_dir = DATA_DIR_NONE;
	srb_ext->req.srb_buff = NULL;
	srb_ext->req.srb_sg_len = 0;
	srb_ext->req.gen_req_t.code = GEN_IO_CODE_EJECT;
	srb_ext->req.gen_req_t.arg1 = load;
	srb_ext->req.srb_done_cb = bht_scsi_srb_basic_cmd_done;
	result = req_eject(pdx, srb_ext);

	if (result == REQ_RESULT_OK) {
		pdx->scsi.scsi_eject = 1;
		srb->result = GOOD;
	} else {
		bht_scsi_error_handle(pdx, srb, result);
		/* cmd handle by thread case */
		if (result == REQ_RESULT_PENDING)
			pdx->scsi.scsi_eject = 1;
	}

exit:
	if (result == REQ_RESULT_PENDING) {
		*busy = TRUE;
		ret = FALSE;
	} else {
		if (result == REQ_RESULT_QUEUE_BUSY)
			*busy = TRUE;
		bht_scsi_free_srb_ext(pdx, srb_ext);
	}
	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Enter %s, ret:%x\n", __func__, ret);
	return ret;
}

/*
 *	Handle Inquriy scsi command
 *	return true means done while return false means pending
 */
static bool bht_scsi_exec_inquiry(bht_dev_ext_t *pdx, struct scsi_cmnd *srb,
				  bool *busy)
{

	e_req_result result = REQ_RESULT_NO_CARD;
	sd_card_t *card = &pdx->card;
	srb_ext_t *srb_ext = NULL;
	bool ret = TRUE;

	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Enter %s\n", __func__);

	srb_ext = bht_scsi_alloc_srb_ext(pdx, srb);
	if (srb_ext == NULL) {
		result = REQ_RESULT_ABORT;
		goto exit;
	}
	srb_ext->req.data_dir = DATA_DIR_IN;
	/* the callback will handle the data itself */
	srb_ext->req.srb_buff = NULL;
	srb_ext->req.gen_req_t.code = GEN_IO_CODE_INIT_CARD;
	srb_ext->req.srb_done_cb = bht_scsi_srb_basic_cmd_done;
	result = req_chk_card_info(pdx, srb_ext);

	if (result == REQ_RESULT_OK) {
		bht_scsi_get_inquiry(pdx, card->card_type, srb);
	} else {
		if (result == REQ_RESULT_QUEUE_BUSY)
			*busy = TRUE;
		bht_scsi_error_handle(pdx, srb, result);
	}
exit:
	if (result == REQ_RESULT_PENDING)
		ret = FALSE;
	else
		bht_scsi_free_srb_ext(pdx, srb_ext);

	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Exit %s, ret:%x\n", __func__, ret);
	return ret;
}

/*
 *	Handle GetCapacity scsi command
 *	return true means done while return false means pending
 */
static bool bht_scsi_exec_capacity(bht_dev_ext_t *pdx, struct scsi_cmnd *srb,
				   bool *busy)
{
	e_req_result result = REQ_RESULT_NO_CARD;
	sd_card_t *card = &pdx->card;
	srb_ext_t *srb_ext;

	bool ret = TRUE;

	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Enter %s\n", __func__);

	srb_ext = bht_scsi_alloc_srb_ext(pdx, srb);
	if (srb_ext == NULL) {
		result = REQ_RESULT_ABORT;
		goto exit;
	}
	srb_ext->req.data_dir = DATA_DIR_IN;
	/* the callback will handle the data itself */
	srb_ext->req.srb_buff = NULL;
	srb_ext->req.srb_sg_len = 0;
	srb_ext->req.gen_req_t.code = GEN_IO_CODE_INIT_CARD;
	srb_ext->req.srb_done_cb = bht_scsi_srb_basic_cmd_done;

	result = req_chk_card_info(pdx, srb_ext);

	if (result == REQ_RESULT_OK) {
		bht_scsi_get_capacity(pdx, (u32) (card->sec_count), srb);
	} else {
		if (result == REQ_RESULT_QUEUE_BUSY)
			*busy = TRUE;
		bht_scsi_error_handle(pdx, srb, result);
	}
exit:
	if (result == REQ_RESULT_PENDING)
		ret = FALSE;
	else
		bht_scsi_free_srb_ext(pdx, srb_ext);

	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_SCSICMD_TRACE, NOT_TO_RAM,
		"Exit %s, ret:%x\n", __func__, ret);
	return ret;
}

/*
 *	Helper function to get rw parameter
 */
static void bht_scsi_get_rw_parameter(byte *cdb, u32 *pStartBlock,
				      u32 *pBlockCount)
{
	byte array[4];

	array[0] = cdb[5];
	array[1] = cdb[4];
	array[2] = cdb[3];
	array[3] = cdb[2];
	*pStartBlock = *((u32 *) &array);

	array[0] = cdb[8];
	array[1] = cdb[7];
	array[2] = 0;
	array[3] = 0;
	*pBlockCount = *((u32 *) &array);

	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_RW_TRACE, NOT_TO_RAM,
		"%s, StartBlock:%x, BlockCount:%x\n", __func__,
		*pStartBlock, *pBlockCount);
}

/*
 *	Call back for Tagged io and scsi rw
 */
static void bht_scsi_srb_tagio_done(void *p, void *srb_ext)
{
	srb_ext_t *p_srb_ext = srb_ext;
	srb_ext_t *p_srb_ext_1 = p_srb_ext;
	srb_ext_t *p_srb_ext_2 = NULL;
	bht_dev_ext_t *pdx = p;
	struct scsi_cmnd *srb = p_srb_ext->psrb;

	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	if (srb == NULL) {
		pdx->p_srb_ext = NULL;
		DbgErr("tagio complete called with srb is null\n");
		goto exit;
	}

	srb->result = CHECK_CONDITION;

	switch (p_srb_ext->req.result) {
	case REQ_RESULT_OK:
		{
			if (p_srb_ext->prev != NULL) {
				p_srb_ext_1 = p_srb_ext->prev;
				p_srb_ext_2 = p_srb_ext;
				DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_RW_TRACE,
					NOT_TO_RAM,
					"p_srb_ext_2: dir=%d, sec_cnt=%d, sec_addr=0x%x, p_srb_ext_1=%p\n",
					p_srb_ext_2->req.data_dir,
					p_srb_ext_2->req.tag_req_t.sec_cnt,
					p_srb_ext_2->req.tag_req_t.sec_addr,
					p_srb_ext_2->prev);
			} else if (p_srb_ext->req.data_dir == DATA_DIR_IN
				   && p_srb_ext->req.tag_req_t.sec_cnt > 1
				   && (p_srb_ext->req.tag_req_t.sec_addr +
				       p_srb_ext->req.tag_req_t.sec_cnt ==
				       pdx->card.sec_count)) {
				p_srb_ext_1 = p_srb_ext;
				p_srb_ext_2 = bht_scsi_alloc_srb_ext(pdx, srb);
				if (p_srb_ext_2 == NULL) {
					DbgErr
					    ("Failed when alloc srb_ext_2 for read latest block\n");
				} else {
					p_srb_ext_2->prev = p_srb_ext_1;
					p_srb_ext_1->next = p_srb_ext_2;
					p_srb_ext_2->req.data_dir = DATA_DIR_IN;
					p_srb_ext_2->req.tag_req_t.sec_cnt = 1;
					p_srb_ext_2->req.tag_req_t.sec_addr =
					    pdx->card.sec_count - 1;
					p_srb_ext_2->req.tag_req_t.use_cmd = 0;
					p_srb_ext_2->req.srb_done_cb =
					    bht_scsi_srb_tagio_done;
					p_srb_ext_2->req.srb_buff =
					    (PVOID) ((unsigned long long)
						     p_srb_ext->req.srb_buff +
						     (p_srb_ext->req.tag_req_t.sec_cnt -
						      1) * SD_BLOCK_LEN);
				}
			}
			if (FALSE ==
			    cfg_dma_need_sdma_like_buffer(pdx->cfg->host_item.test_dma_mode_setting.dma_mode)) {
				if (p_srb_ext_2 == NULL
				    || p_srb_ext_2 == p_srb_ext)
					os_free_sg_list(pdx, srb);
				else {
					if (p_srb_ext_1->req.srb_sg_list[p_srb_ext_1->req.srb_sg_len -
							    1].Length >=
					    SD_BLOCK_LEN) {
						p_srb_ext_2->req.srb_sg_len = 1;
						p_srb_ext_2->req.srb_sg_list[0].Address =
						    p_srb_ext_1->req.srb_sg_list
						    [p_srb_ext_1->req.srb_sg_len - 1].Address +
						    p_srb_ext_1->req.srb_sg_list
						    [p_srb_ext_1->req.srb_sg_len - 1].Length -
						    SD_BLOCK_LEN;
						p_srb_ext_2->req.srb_sg_list[0].Length =
						    SD_BLOCK_LEN;
					} else {
						DbgErr
						    ("sg list latest record size is %d, less than SD block size\n",
						     p_srb_ext_1->req.srb_sg_list
						     [p_srb_ext_1->req.srb_sg_len
						      - 1].Length);
						bht_scsi_free_srb_ext(pdx,
								      p_srb_ext_2);
						p_srb_ext_1->next = NULL;
						p_srb_ext_2 = NULL;
					}
				}
			} else {
				if (p_srb_ext->req.data_dir == DATA_DIR_IN) {
					if (p_srb_ext_2 == NULL
					    || p_srb_ext_2 == p_srb_ext)
						scsi_sg_copy_from_buffer(srb,
									 p_srb_ext->req.srb_buff,
									 srb->sdb.length);
				}
			}
			srb->result = GOOD;
			break;
		}
	case REQ_RESULT_NO_CARD:
	case REQ_RESULT_ABORT:
		{
			bht_scsi_set_sensecode(pdx, srb, SCSI_SENSE_NOT_READY,
					       SCSI_ADSENSE_NO_MEDIA_IN_DEVICE);
			break;
		}
	case REQ_RESULT_PROTECTED:
		{
			bht_scsi_set_sensecode(pdx, srb,
					       SCSI_SENSE_DATA_PROTECT,
					       SCSI_ADSENSE_WRITE_PROTECT);
			break;
		}
		/* RW error case */
	default:
		{
			bht_scsi_set_sensecode(pdx, srb,
					       SCSI_SENSE_UNIT_ATTENTION, 0);
			break;
		}

	}

	/* tag queue is empty we need to set device to free status */
	if (tq_is_empty(pdx)) {
		func_autotimer_start(pdx);
		if (pdx->host.feature.hw_led_fix == 0)
			host_led_ctl(&pdx->host, FALSE);
	}

	if (p_srb_ext_2 == NULL || p_srb_ext_2 == p_srb_ext) {
		DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_RW_TRACE, NOT_TO_RAM,
			"scsi io done for srb(%p)\n", srb);
		bht_scsi_free_srb_ext(pdx, p_srb_ext_2);
		bht_scsi_free_srb_ext(pdx, p_srb_ext_1);
		pdx->p_srb_ext = NULL;

		scsi_done(srb);
	} else {
		int result = req_tag_io_add(pdx, p_srb_ext_2);

		if (result != REQ_RESULT_OK)
			bht_scsi_error_handle(pdx, srb, result);

		if (result == REQ_RESULT_PENDING) {
			DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_RW_TRACE,
				NOT_TO_RAM,
				"add srb_ext_2(%p) of srb(%p) to tag io queue\n",
				p_srb_ext_2, srb);
		} else {
			DbgErr("Failed to add srb_ext_2 %p to tag io queue\n",
			       p_srb_ext_2);
			bht_scsi_free_srb_ext(pdx, p_srb_ext_1);
			pdx->p_srb_ext = NULL;

			scsi_done(srb);
		}
	}

#ifdef DBG_PERFORMANCE
	calc_io_end(&pdx->tick);
#endif

exit:
	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_RW_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

/*
 *	Handle Read10 and Write10 scsi command
 *	return true means done while return false means pending
 */
static bool bht_scsi_exec_rw(bht_dev_ext_t *pdx, struct scsi_cmnd *srb,
			     bool bWrite, bool *busy)
{
	e_req_result result = REQ_RESULT_NO_CARD;
	srb_ext_t *srb_ext;
	bool ret = TRUE;

	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	srb_ext = bht_scsi_alloc_srb_ext(pdx, srb);
	if (srb_ext == NULL) {
		result = REQ_RESULT_ABORT;
		goto exit;
	}
	srb_ext->psrb = srb;
	srb_ext->req.data_dir = bWrite ? DATA_DIR_OUT : DATA_DIR_IN;
	srb_ext->req.srb_buff = pdx->os.virt_buff;
	if (FALSE ==
	    cfg_dma_need_sdma_like_buffer(pdx->cfg->host_item.test_dma_mode_setting.dma_mode))
		srb_ext->req.srb_sg_len =
		    os_get_sg_list(pdx, srb, srb_ext->req.srb_sg_list);
	else {
		if (bWrite)
			scsi_sg_copy_to_buffer(srb, srb_ext->req.srb_buff,
					       srb->sdb.length);
		srb_ext->req.srb_sg_len = 0;
	}
	srb_ext->req.srb_done_cb = bht_scsi_srb_tagio_done;
	srb_ext->req.tag_req_t.use_cmd = 0;

	/*
	 * Get Scsi parameter for RW
	 */
	bht_scsi_get_rw_parameter(srb->cmnd,
				  &srb_ext->req.tag_req_t.sec_addr,
				  &srb_ext->req.tag_req_t.sec_cnt);
	calc_req_start(&pdx->tick, srb_ext->req.tag_req_t.sec_cnt, bWrite);

	/* Workaround for GG8 chip DDR200 write operation: timing issue */
	if (bWrite && pdx->host.chip_type == CHIP_GG8
	    && pdx->card.info.sw_cur_setting.sd_access_mode ==
	    SD_FNC_AM_DDR200) {
		DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_RW_TRACE, NOT_TO_RAM,
			"Enter GG8 DDR200 workaround patch\n");
		if (tq_judge_request_continuous
		    (card_is_low_capacity(&pdx->card), pdx->last_req.data_dir,
		     pdx->last_req.sec_addr, pdx->last_req.sec_cnt,
		     srb_ext->req.data_dir,
		     srb_ext->req.tag_req_t.sec_addr) == FALSE) {
			DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_RW_TRACE,
				NOT_TO_RAM,
				"Set flag to add NOP descriptor table\n");
			srb_ext->req.gg8_ddr200_workaround = 1;
		} else
			srb_ext->req.gg8_ddr200_workaround = 0;

	} else
		srb_ext->req.gg8_ddr200_workaround = 0;

	pdx->last_req.data_dir = srb_ext->req.data_dir;
	pdx->last_req.sec_addr = srb_ext->req.tag_req_t.sec_addr;
	pdx->last_req.sec_cnt = srb_ext->req.tag_req_t.sec_cnt;

	result = req_tag_io_add(pdx, srb_ext);

exit:
	if (result != REQ_RESULT_OK)
		bht_scsi_error_handle(pdx, srb, result);

	if (result == REQ_RESULT_PENDING)
		ret = FALSE;
	else {
		if (result == REQ_RESULT_QUEUE_BUSY)
			*busy = TRUE;
		bht_scsi_free_srb_ext(pdx, srb_ext);
	}
	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Exit %s, ret:%x\n", __func__, ret);
	return ret;
}

/*
 *	The Entry to handle SCSI command
 */
static int bht_scsi_queuecommand_lck(struct scsi_cmnd *srb)
{
	bht_dev_ext_t *pdx = (bht_dev_ext_t *) (srb->device->host->hostdata[0]);
	bool cmd_done = TRUE;
	bool dev_busy = FALSE;

	DbgInfo(MODULE_OS_ENTRYAPI, FEATURE_RW_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (pdx->signature != BHT_PDX_SIGNATURE) {
		DbgErr("bht scsi queuecommand pdx signature is wrong\n");
		return SCSI_MLQUEUE_HOST_BUSY;
	}

	if (srb->cmnd == NULL) {
		DbgErr
		    ("bhtscsi_queuecommand srb->cmnd is NULL, cmd_len is %d ,srb %p\n",
		     srb->cmd_len, srb);
		return SCSI_RETURN_NOT_HANDLED;
	}

	srb->result = 0;
	func_thermal_update_time(pdx);

	switch (srb->cmnd[0]) {
	case TEST_UNIT_READY:
		{
			bht_scsi_test_unit_ready(pdx, srb);
			break;
		}
	case REQUEST_SENSE:
		{
			bht_scsi_request_sense(pdx, srb);
			break;
		}
	case INQUIRY:
		{
			cmd_done = bht_scsi_exec_inquiry(pdx, srb, &dev_busy);
			break;
		}
	case READ_CAPACITY:
		{
			cmd_done = bht_scsi_exec_capacity(pdx, srb, &dev_busy);
			break;
		}
	case READ_10:
		{
			cmd_done = bht_scsi_exec_rw(pdx, srb, FALSE, &dev_busy);
			break;
		}
	case WRITE_10:
		{
			cmd_done = bht_scsi_exec_rw(pdx, srb, TRUE, &dev_busy);
			break;
		}
	case MODE_SENSE:
		{
			bht_scsi_exec_modesense(pdx, srb);
			break;
		}
	case VERIFY:
	case SEND_DIAGNOSTIC:
	case SYNCHRONIZE_CACHE:
		{
			srb->result = GOOD;

			scsi_done(srb);
			break;
		}
	case START_STOP:
		{
			cmd_done = bht_scsi_load_unload(pdx, srb, &dev_busy);
			break;
		}

	case ALLOW_MEDIUM_REMOVAL:
		{
			bht_scsi_allow_removal(pdx, srb);
			break;
		}

	default:
		{
			srb->result = CHECK_CONDITION;
			bht_scsi_set_sensecode(pdx, srb,
					       SCSI_SENSE_ILLEGAL_REQUEST,
					       SCSI_ADSENSE_ILLEGAL_COMMAND);
			break;
		}
	}

	if (cmd_done && dev_busy == FALSE)
		scsi_done(srb);
	else if (dev_busy)
		return SCSI_MLQUEUE_DEVICE_BUSY;
	return 0;

}

static enum scsi_timeout_action bht_scsi_eh_timeout(struct scsi_cmnd *srb)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 18, 0)
	enum scsi_timeout_action retval = SCSI_EH_DONE;
#else
	enum blk_eh_timer_return retval = BLK_EH_NOT_HANDLED;
#endif

	bht_dev_ext_t *pdx = (bht_dev_ext_t *) (srb->device->host->hostdata[0]);

	/* todo It is may not safer for busreset is at high IRQL; and one event to slove this */
	DbgErr("SCSI eh_timeout Enter\n");
	if (os_pending_thread(pdx, TRUE) == FALSE)
		DbgErr("bus rest pending thread failed\n");
	func_autotimer_stop(pdx);
	card_power_off(&pdx->card, TRUE);
	host_init(&pdx->host);
	req_cancel_all_io(pdx);
	os_pending_thread(pdx, FALSE);
	DbgErr("SCSI eh_timeout Exit\n");

	return retval;
}

static DEF_SCSI_QCMD(bht_scsi_queuecommand)
/*
 * this defines our 'SCSI host'
 */
struct scsi_host_template bht_scsi_template = {
	.name = "BHT SD Card Reader",
	.module = THIS_MODULE,
	.proc_name = "bht_scsi_host",
	.queuecommand = bht_scsi_queuecommand,
	.eh_timed_out = bht_scsi_eh_timeout,
	.can_queue = 1,
	.this_id = -1,
	.sg_tablesize = SG_ALL,
	.max_sectors = CFG_MAX_TRANSFER_LENGTH / SD_BLOCK_LEN,
	.cmd_per_lun = 1,

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
	.use_clustering = TRUE,
#endif

	.emulated = FALSE,
};
