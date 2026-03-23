// SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause)
/*
 * NETC NTMP (NETC Table Management Protocol) 2.0 Library
 * Copyright 2025 NXP
 */

#include <linux/dma-mapping.h>
#include <linux/fsl/netc_global.h>
#include <linux/iopoll.h>

#include "ntmp_private.h"

#define NETC_CBDR_TIMEOUT		1000 /* us */
#define NETC_CBDR_DELAY_US		10
#define NETC_CBDR_MR_EN			BIT(31)

#define NTMP_BASE_ADDR_ALIGN		128
#define NTMP_DATA_ADDR_ALIGN		32

/* Define NTMP Table ID */
#define NTMP_MAFT_ID			1
#define NTMP_RSST_ID			3
#define NTMP_IPFT_ID			13
#define NTMP_FDBT_ID			15
#define NTMP_VFT_ID			18
#define NTMP_BPT_ID			41

/* Generic Update Actions for most tables */
#define NTMP_GEN_UA_CFGEU		BIT(0)
#define NTMP_GEN_UA_STSEU		BIT(1)

/* Specific Update Actions for some tables */
#define BPT_UA_BPSEU			BIT(1)

/* Query Action: 0: Full query, 1: Only query entry ID */
#define NTMP_QA_ENTRY_ID		1

#define NTMP_ENTRY_ID_SIZE		4
#define RSST_ENTRY_NUM			64
#define RSST_STSE_DATA_SIZE(n)		((n) * 8)
#define RSST_CFGE_DATA_SIZE(n)		(n)

int ntmp_init_cbdr(struct netc_cbdr *cbdr, struct device *dev,
		   const struct netc_cbdr_regs *regs)
{
	int cbd_num = NETC_CBDR_BD_NUM;
	size_t size;

	size = cbd_num * sizeof(union netc_cbd) + NTMP_BASE_ADDR_ALIGN;
	cbdr->addr_base = dma_alloc_coherent(dev, size, &cbdr->dma_base,
					     GFP_KERNEL);
	if (!cbdr->addr_base)
		return -ENOMEM;

	cbdr->dma_size = size;
	cbdr->bd_num = cbd_num;
	cbdr->regs = *regs;
	cbdr->dev = dev;

	/* The base address of the Control BD Ring must be 128 bytes aligned */
	cbdr->dma_base_align =  ALIGN(cbdr->dma_base,  NTMP_BASE_ADDR_ALIGN);
	cbdr->addr_base_align = PTR_ALIGN(cbdr->addr_base,
					  NTMP_BASE_ADDR_ALIGN);

	spin_lock_init(&cbdr->ring_lock);

	cbdr->next_to_use = netc_read(cbdr->regs.pir);
	cbdr->next_to_clean = netc_read(cbdr->regs.cir);

	/* Step 1: Configure the base address of the Control BD Ring */
	netc_write(cbdr->regs.bar0, lower_32_bits(cbdr->dma_base_align));
	netc_write(cbdr->regs.bar1, upper_32_bits(cbdr->dma_base_align));

	/* Step 2: Configure the number of BDs of the Control BD Ring */
	netc_write(cbdr->regs.lenr, cbdr->bd_num);

	/* Step 3: Enable the Control BD Ring */
	netc_write(cbdr->regs.mr, NETC_CBDR_MR_EN);

	return 0;
}
EXPORT_SYMBOL_GPL(ntmp_init_cbdr);

void ntmp_free_cbdr(struct netc_cbdr *cbdr)
{
	/* Disable the Control BD Ring */
	netc_write(cbdr->regs.mr, 0);
	dma_free_coherent(cbdr->dev, cbdr->dma_size, cbdr->addr_base,
			  cbdr->dma_base);
	memset(cbdr, 0, sizeof(*cbdr));
}
EXPORT_SYMBOL_GPL(ntmp_free_cbdr);

static int ntmp_get_free_cbd_num(struct netc_cbdr *cbdr)
{
	return (cbdr->next_to_clean - cbdr->next_to_use - 1 +
		cbdr->bd_num) % cbdr->bd_num;
}

static union netc_cbd *ntmp_get_cbd(struct netc_cbdr *cbdr, int index)
{
	return &((union netc_cbd *)(cbdr->addr_base_align))[index];
}

static void ntmp_clean_cbdr(struct netc_cbdr *cbdr)
{
	union netc_cbd *cbd;
	int i;

	i = cbdr->next_to_clean;
	while (netc_read(cbdr->regs.cir) != i) {
		cbd = ntmp_get_cbd(cbdr, i);
		memset(cbd, 0, sizeof(*cbd));
		i = (i + 1) % cbdr->bd_num;
	}

	cbdr->next_to_clean = i;
}

static struct netc_cbdr *netc_select_cbdr(struct ntmp_user *user)
{
	int cpu, i;

	for (i = 0; i < user->cbdr_num; i++) {
		if (spin_is_locked(&user->ring[i].ring_lock))
			continue;

		return &user->ring[i];
	}

	/* If all the command BDRs are busy now, we select
	 * one of them, but need to wait for a while to use.
	 */
	cpu = raw_smp_processor_id();

	return &user->ring[cpu % user->cbdr_num];
}

static int netc_xmit_ntmp_cmd(struct ntmp_user *user, union netc_cbd *cbd)
{
	union netc_cbd *cur_cbd;
	struct netc_cbdr *cbdr;
	int i, err;
	u16 status;
	u32 val;

	if (user->cbdr_num == 1)
		cbdr = &user->ring[0];
	else
		cbdr = netc_select_cbdr(user);

	spin_lock_bh(&cbdr->ring_lock);

	if (unlikely(!ntmp_get_free_cbd_num(cbdr)))
		ntmp_clean_cbdr(cbdr);

	i = cbdr->next_to_use;
	cur_cbd = ntmp_get_cbd(cbdr, i);
	*cur_cbd = *cbd;
	dma_wmb();

	/* Update producer index of both software and hardware */
	i = (i + 1) % cbdr->bd_num;
	cbdr->next_to_use = i;
	netc_write(cbdr->regs.pir, i);

	err = read_poll_timeout_atomic(netc_read, val, val == i,
				       NETC_CBDR_DELAY_US, NETC_CBDR_TIMEOUT,
				       true, cbdr->regs.cir);
	if (unlikely(err))
		goto cbdr_unlock;

	dma_rmb();
	/* Get the writeback command BD, because the caller may need
	 * to check some other fields of the response header.
	 */
	*cbd = *cur_cbd;

	/* Check the writeback error status */
	status = le16_to_cpu(cbd->resp_hdr.error_rr) & NTMP_RESP_ERROR;
	if (unlikely(status)) {
		err = -EIO;
		dev_err(user->dev, "Command BD error: 0x%04x\n", status);
	}

	ntmp_clean_cbdr(cbdr);
	dma_wmb();

cbdr_unlock:
	spin_unlock_bh(&cbdr->ring_lock);

	return err;
}

static int ntmp_alloc_data_mem(struct ntmp_dma_buf *data, void **buf_align)
{
	void *buf;

	buf = dma_alloc_coherent(data->dev, data->size + NTMP_DATA_ADDR_ALIGN,
				 &data->dma, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	data->buf = buf;
	*buf_align = PTR_ALIGN(buf, NTMP_DATA_ADDR_ALIGN);

	return 0;
}

static void ntmp_free_data_mem(struct ntmp_dma_buf *data)
{
	dma_free_coherent(data->dev, data->size + NTMP_DATA_ADDR_ALIGN,
			  data->buf, data->dma);
}

static void ntmp_fill_request_hdr(union netc_cbd *cbd, dma_addr_t dma,
				  int len, int table_id, int cmd,
				  int access_method)
{
	dma_addr_t dma_align;

	memset(cbd, 0, sizeof(*cbd));
	dma_align = ALIGN(dma, NTMP_DATA_ADDR_ALIGN);
	cbd->req_hdr.addr = cpu_to_le64(dma_align);
	cbd->req_hdr.len = cpu_to_le32(len);
	cbd->req_hdr.cmd = cmd;
	cbd->req_hdr.access_method = FIELD_PREP(NTMP_ACCESS_METHOD,
						access_method);
	cbd->req_hdr.table_id = table_id;
	cbd->req_hdr.ver_cci_rr = FIELD_PREP(NTMP_HDR_VERSION,
					     NTMP_HDR_VER2);
	/* For NTMP version 2.0 or later version */
	cbd->req_hdr.npf = cpu_to_le32(NTMP_NPF);
}

static void ntmp_fill_crd(struct ntmp_cmn_req_data *crd, u8 tblv,
			  u8 qa, u16 ua)
{
	crd->update_act = cpu_to_le16(ua);
	crd->tblv_qact = NTMP_TBLV_QACT(tblv, qa);
}

static void ntmp_fill_crd_eid(struct ntmp_req_by_eid *rbe, u8 tblv,
			      u8 qa, u16 ua, u32 entry_id)
{
	ntmp_fill_crd(&rbe->crd, tblv, qa, ua);
	rbe->entry_id = cpu_to_le32(entry_id);
}

static const char *ntmp_table_name(int tbl_id)
{
	switch (tbl_id) {
	case NTMP_MAFT_ID:
		return "MAC Address Filter Table";
	case NTMP_RSST_ID:
		return "RSS Table";
	case NTMP_IPFT_ID:
		return "Ingress Port Filter Table";
	case NTMP_FDBT_ID:
		return "FDB Table";
	case NTMP_VFT_ID:
		return "VLAN Filter Table";
	case NTMP_BPT_ID:
		return "Buffer Pool Table";
	default:
		return "Unknown Table";
	}
}

static int ntmp_delete_entry_by_id(struct ntmp_user *user, int tbl_id,
				   u8 tbl_ver, u32 entry_id, u32 req_len,
				   u32 resp_len)
{
	struct ntmp_dma_buf data = {
		.dev = user->dev,
		.size = max(req_len, resp_len),
	};
	struct ntmp_req_by_eid *req;
	union netc_cbd cbd;
	int err;

	err = ntmp_alloc_data_mem(&data, (void **)&req);
	if (err)
		return err;

	ntmp_fill_crd_eid(req, tbl_ver, 0, 0, entry_id);
	ntmp_fill_request_hdr(&cbd, data.dma, NTMP_LEN(req_len, resp_len),
			      tbl_id, NTMP_CMD_DELETE, NTMP_AM_ENTRY_ID);

	err = netc_xmit_ntmp_cmd(user, &cbd);
	if (err)
		dev_err(user->dev,
			"Failed to delete entry 0x%x of %s, err: %pe",
			entry_id, ntmp_table_name(tbl_id), ERR_PTR(err));

	ntmp_free_data_mem(&data);

	return err;
}

static int ntmp_query_entry_by_id(struct ntmp_user *user, int tbl_id,
				  u32 len, struct ntmp_req_by_eid *req,
				  dma_addr_t dma, bool compare_eid)
{
	struct ntmp_cmn_resp_query *resp;
	int cmd = NTMP_CMD_QUERY;
	union netc_cbd cbd;
	u32 entry_id;
	int err;

	entry_id = le32_to_cpu(req->entry_id);
	if (le16_to_cpu(req->crd.update_act))
		cmd = NTMP_CMD_QU;

	/* Request header */
	ntmp_fill_request_hdr(&cbd, dma, len, tbl_id, cmd, NTMP_AM_ENTRY_ID);
	err = netc_xmit_ntmp_cmd(user, &cbd);
	if (err) {
		dev_err(user->dev,
			"Failed to query entry 0x%x of %s, err: %pe\n",
			entry_id, ntmp_table_name(tbl_id), ERR_PTR(err));
		return err;
	}

	/* For a few tables, the first field of their response data is not
	 * entry_id, so directly return success.
	 */
	if (!compare_eid)
		return 0;

	resp = (struct ntmp_cmn_resp_query *)req;
	if (unlikely(le32_to_cpu(resp->entry_id) != entry_id)) {
		dev_err(user->dev,
			"%s: query EID 0x%x doesn't match response EID 0x%x\n",
			ntmp_table_name(tbl_id), entry_id, le32_to_cpu(resp->entry_id));
		return -EIO;
	}

	return 0;
}

int ntmp_maft_add_entry(struct ntmp_user *user, u32 entry_id,
			struct maft_entry_data *maft)
{
	struct ntmp_dma_buf data = {
		.dev = user->dev,
		.size = sizeof(struct maft_req_add),
	};
	struct maft_req_add *req;
	union netc_cbd cbd;
	int err;

	err = ntmp_alloc_data_mem(&data, (void **)&req);
	if (err)
		return err;

	/* Set mac address filter table request data buffer */
	ntmp_fill_crd_eid(&req->rbe, user->tbl.maft_ver, 0, 0, entry_id);
	req->keye = maft->keye;
	req->cfge = maft->cfge;

	ntmp_fill_request_hdr(&cbd, data.dma, NTMP_LEN(data.size, 0),
			      NTMP_MAFT_ID, NTMP_CMD_ADD, NTMP_AM_ENTRY_ID);
	err = netc_xmit_ntmp_cmd(user, &cbd);
	if (err)
		dev_err(user->dev, "Failed to add MAFT entry 0x%x, err: %pe\n",
			entry_id, ERR_PTR(err));

	ntmp_free_data_mem(&data);

	return err;
}
EXPORT_SYMBOL_GPL(ntmp_maft_add_entry);

int ntmp_maft_query_entry(struct ntmp_user *user, u32 entry_id,
			  struct maft_entry_data *maft)
{
	struct ntmp_dma_buf data = {
		.dev = user->dev,
		.size = sizeof(struct maft_resp_query),
	};
	struct maft_resp_query *resp;
	struct ntmp_req_by_eid *req;
	int err;

	err = ntmp_alloc_data_mem(&data, (void **)&req);
	if (err)
		return err;

	ntmp_fill_crd_eid(req, user->tbl.maft_ver, 0, 0, entry_id);
	err = ntmp_query_entry_by_id(user, NTMP_MAFT_ID,
				     NTMP_LEN(sizeof(*req), data.size),
				     req, data.dma, true);
	if (err)
		goto end;

	resp = (struct maft_resp_query *)req;
	maft->keye = resp->keye;
	maft->cfge = resp->cfge;

end:
	ntmp_free_data_mem(&data);

	return err;
}
EXPORT_SYMBOL_GPL(ntmp_maft_query_entry);

int ntmp_maft_delete_entry(struct ntmp_user *user, u32 entry_id)
{
	return ntmp_delete_entry_by_id(user, NTMP_MAFT_ID, user->tbl.maft_ver,
				       entry_id, NTMP_EID_REQ_LEN, 0);
}
EXPORT_SYMBOL_GPL(ntmp_maft_delete_entry);

int ntmp_rsst_update_entry(struct ntmp_user *user, const u32 *table,
			   int count)
{
	struct ntmp_dma_buf data = {.dev = user->dev};
	struct rsst_req_update *req;
	union netc_cbd cbd;
	int err, i;

	if (count != RSST_ENTRY_NUM)
		/* HW only takes in a full 64 entry table */
		return -EINVAL;

	data.size = struct_size(req, groups, count);
	err = ntmp_alloc_data_mem(&data, (void **)&req);
	if (err)
		return err;

	/* Set the request data buffer */
	ntmp_fill_crd_eid(&req->rbe, user->tbl.rsst_ver, 0,
			  NTMP_GEN_UA_CFGEU | NTMP_GEN_UA_STSEU, 0);
	for (i = 0; i < count; i++)
		req->groups[i] = (u8)(table[i]);

	ntmp_fill_request_hdr(&cbd, data.dma, NTMP_LEN(data.size, 0),
			      NTMP_RSST_ID, NTMP_CMD_UPDATE, NTMP_AM_ENTRY_ID);

	err = netc_xmit_ntmp_cmd(user, &cbd);
	if (err)
		dev_err(user->dev, "Failed to update RSST entry, err: %pe\n",
			ERR_PTR(err));

	ntmp_free_data_mem(&data);

	return err;
}
EXPORT_SYMBOL_GPL(ntmp_rsst_update_entry);

int ntmp_rsst_query_entry(struct ntmp_user *user, u32 *table, int count)
{
	struct ntmp_dma_buf data = {.dev = user->dev};
	struct ntmp_req_by_eid *req;
	union netc_cbd cbd;
	int err, i;
	u8 *group;

	if (count != RSST_ENTRY_NUM)
		/* HW only takes in a full 64 entry table */
		return -EINVAL;

	data.size = NTMP_ENTRY_ID_SIZE + RSST_STSE_DATA_SIZE(count) +
		    RSST_CFGE_DATA_SIZE(count);
	err = ntmp_alloc_data_mem(&data, (void **)&req);
	if (err)
		return err;

	/* Set the request data buffer */
	ntmp_fill_crd_eid(req, user->tbl.rsst_ver, 0, 0, 0);
	ntmp_fill_request_hdr(&cbd, data.dma, NTMP_LEN(sizeof(*req), data.size),
			      NTMP_RSST_ID, NTMP_CMD_QUERY, NTMP_AM_ENTRY_ID);
	err = netc_xmit_ntmp_cmd(user, &cbd);
	if (err) {
		dev_err(user->dev, "Failed to query RSST entry, err: %pe\n",
			ERR_PTR(err));
		goto end;
	}

	group = (u8 *)req;
	group += NTMP_ENTRY_ID_SIZE + RSST_STSE_DATA_SIZE(count);
	for (i = 0; i < count; i++)
		table[i] = group[i];

end:
	ntmp_free_data_mem(&data);

	return err;
}
EXPORT_SYMBOL_GPL(ntmp_rsst_query_entry);

/**
 * ntmp_ipft_add_entry - add an entry into the ingress port filter table
 * @user: target ntmp_user struct
 * @entry: the entry data, entry->cfge (configuration element data) and
 * entry->keye (key element data) are used as input. Since the entry ID
 * is assigned by the hardware, so entry->entry_id is a returned value
 * for the driver to use, the driver can update/delete/query the entry
 * based on the entry_id.
 *
 * Return: 0 on success, otherwise a negative error code
 */
int ntmp_ipft_add_entry(struct ntmp_user *user,
			struct ipft_entry_data *entry)
{
	struct ntmp_dma_buf data = {
		.dev = user->dev,
		.size = sizeof(struct ipft_resp_query),
	};
	struct ipft_resp_query *resp;
	struct ipft_req_ua *req;
	union netc_cbd cbd;
	u32 len;
	int err;

	err = ntmp_alloc_data_mem(&data, (void **)&req);
	if (err)
		return err;

	ntmp_fill_crd(&req->crd, user->tbl.ipft_ver, NTMP_QA_ENTRY_ID,
		      NTMP_GEN_UA_CFGEU | NTMP_GEN_UA_STSEU);
	req->ak.keye = entry->keye;
	req->cfge = entry->cfge;

	len = NTMP_LEN(sizeof(*req), data.size);
	ntmp_fill_request_hdr(&cbd, data.dma, len, NTMP_IPFT_ID,
			      NTMP_CMD_AQ, NTMP_AM_TERNARY_KEY);

	err = netc_xmit_ntmp_cmd(user, &cbd);
	if (err) {
		dev_err(user->dev, "Failed to add %s entry, err: %pe\n",
			ntmp_table_name(NTMP_IPFT_ID), ERR_PTR(err));

		goto end;
	}

	resp = (struct ipft_resp_query *)req;
	entry->entry_id = le32_to_cpu(resp->entry_id);

end:
	ntmp_free_data_mem(&data);

	return err;
}
EXPORT_SYMBOL_GPL(ntmp_ipft_add_entry);

/**
 * ntmp_ipft_delete_entry - delete a specified ingress port filter table entry
 * @user: target ntmp_user struct
 * @entry_id: the specified ID of the ingress port filter table entry
 *
 * Return: 0 on success, otherwise a negative error code
 */
int ntmp_ipft_delete_entry(struct ntmp_user *user, u32 entry_id)
{
	u32 req_len = sizeof(struct ipft_req_qd);

	return ntmp_delete_entry_by_id(user, NTMP_IPFT_ID,
				       user->tbl.ipft_ver,
				       entry_id, req_len,
				       NTMP_STATUS_RESP_LEN);
}
EXPORT_SYMBOL_GPL(ntmp_ipft_delete_entry);

/**
 * ntmp_fdbt_add_entry - add an entry into the FDB table
 * @user: target ntmp_user struct
 * @entry_id: returned value, the entry ID of the new added entry
 * @keye: key element data
 * @cfge: configuration element data
 *
 * Return: 0 on success, otherwise a negative error code
 */
int ntmp_fdbt_add_entry(struct ntmp_user *user, u32 *entry_id,
			const struct fdbt_keye_data *keye,
			const struct fdbt_cfge_data *cfge)
{
	struct ntmp_dma_buf data = {
		.dev = user->dev,
		.size = sizeof(struct fdbt_req_ua),
	};
	struct fdbt_resp_query *resp;
	struct fdbt_req_ua *req;
	union netc_cbd cbd;
	u32 len;
	int err;

	err = ntmp_alloc_data_mem(&data, (void **)&req);
	if (err)
		return err;

	/* Request data */
	ntmp_fill_crd(&req->crd, user->tbl.fdbt_ver, NTMP_QA_ENTRY_ID,
		      NTMP_GEN_UA_CFGEU);
	req->ak.exact.keye = *keye;
	req->cfge = *cfge;

	len = NTMP_LEN(data.size, sizeof(*resp));
	/* The entry ID is allotted by hardware, so we need to perform
	 * a query action after the add action to get the entry ID from
	 * hardware.
	 */
	ntmp_fill_request_hdr(&cbd, data.dma, len, NTMP_FDBT_ID,
			      NTMP_CMD_AQ, NTMP_AM_EXACT_KEY);
	err = netc_xmit_ntmp_cmd(user, &cbd);
	if (err) {
		dev_err(user->dev, "Failed to add %s entry, err: %pe\n",
			ntmp_table_name(NTMP_FDBT_ID), ERR_PTR(err));
		goto end;
	}

	if (entry_id) {
		resp = (struct fdbt_resp_query *)req;
		*entry_id = le32_to_cpu(resp->entry_id);
	}

end:
	ntmp_free_data_mem(&data);

	return err;
}
EXPORT_SYMBOL_GPL(ntmp_fdbt_add_entry);

/**
 * ntmp_fdbt_update_entry - update the configuration element data of the
 * specified FDB entry
 * @user: target ntmp_user struct
 * @entry_id: the specified entry ID of the FDB table
 * @cfge: configuration element data
 *
 * Return: 0 on success, otherwise a negative error code
 */
int ntmp_fdbt_update_entry(struct ntmp_user *user, u32 entry_id,
			   const struct fdbt_cfge_data *cfge)
{
	struct ntmp_dma_buf data = {
		.dev = user->dev,
		.size = sizeof(struct fdbt_req_ua),
	};
	struct fdbt_req_ua *req;
	union netc_cbd cbd;
	u32 len;
	int err;

	err = ntmp_alloc_data_mem(&data, (void **)&req);
	if (err)
		return err;

	/* Request data */
	ntmp_fill_crd(&req->crd, user->tbl.fdbt_ver, 0, NTMP_GEN_UA_CFGEU);
	req->ak.eid.entry_id = cpu_to_le32(entry_id);
	req->cfge = *cfge;

	/* Request header */
	len = NTMP_LEN(data.size, NTMP_STATUS_RESP_LEN);
	ntmp_fill_request_hdr(&cbd, data.dma, len, NTMP_FDBT_ID,
			      NTMP_CMD_UPDATE, NTMP_AM_ENTRY_ID);
	err = netc_xmit_ntmp_cmd(user, &cbd);
	if (err)
		dev_err(user->dev, "Failed to update %s entry, err: %pe\n",
			ntmp_table_name(NTMP_FDBT_ID), ERR_PTR(err));

	ntmp_free_data_mem(&data);

	return err;
}
EXPORT_SYMBOL_GPL(ntmp_fdbt_update_entry);

/**
 * ntmp_fdbt_delete_entry - delete the specified FDB entry
 * @user: target ntmp_user struct
 * @entry_id: the specified ID of the FDB entry
 *
 * Return: 0 on success, otherwise a negative error code
 */
int ntmp_fdbt_delete_entry(struct ntmp_user *user, u32 entry_id)
{
	u32 req_len = sizeof(struct fdbt_req_qd);

	return ntmp_delete_entry_by_id(user, NTMP_FDBT_ID,
				       user->tbl.fdbt_ver,
				       entry_id, req_len,
				       NTMP_STATUS_RESP_LEN);
}
EXPORT_SYMBOL_GPL(ntmp_fdbt_delete_entry);

/**
 * ntmp_fdbt_search_port_entry - Search the FDB entry on the specified
 * port based on RESUME_ENTRY_ID
 * @user: target ntmp_user struct
 * @port: the specified switch port ID
 * @resume_entry_id: it is both an input and an output. As an input, it
 * represents the FDB entry ID to be searched. If it is a NULL entry ID,
 * it indicates that the first FDB entry for that port is being searched.
 * As an output, it represents the next FDB entry ID to be searched.
 * @entry: returned value, the response data of the searched FDB entry
 *
 * Return: 0 on success, otherwise a negative error code
 */
int ntmp_fdbt_search_port_entry(struct ntmp_user *user, int port,
				u32 *resume_entry_id,
				struct fdbt_entry_data *entry)
{
	struct ntmp_dma_buf data = {
		.dev = user->dev,
		.size = sizeof(struct fdbt_req_qd),
	};
	struct fdbt_resp_query *resp;
	struct fdbt_req_qd *req;
	union netc_cbd cbd;
	u32 len;
	int err;

	err = ntmp_alloc_data_mem(&data, (void **)&req);
	if (err)
		return err;

	/* Request data */
	ntmp_fill_crd(&req->crd, user->tbl.fdbt_ver, 0, 0);
	req->ak.search.resume_eid = cpu_to_le32(*resume_entry_id);
	req->ak.search.cfge.port_bitmap = cpu_to_le32(BIT(port));
	/* Match CFGE_DATA[PORT_BITMAP] field */
	req->ak.search.cfge_mc = FDBT_CFGE_MC_PORT_BITMAP;

	/* Request header */
	len = NTMP_LEN(data.size, sizeof(*resp));
	ntmp_fill_request_hdr(&cbd, data.dma, len, NTMP_FDBT_ID,
			      NTMP_CMD_QUERY, NTMP_AM_SEARCH);

	err = netc_xmit_ntmp_cmd(user, &cbd);
	if (err) {
		dev_err(user->dev,
			"Failed to search %s entry on port %d, err: %pe\n",
			ntmp_table_name(NTMP_FDBT_ID), port, ERR_PTR(err));
		goto end;
	}

	if (!cbd.resp_hdr.num_matched) {
		entry->entry_id = NTMP_NULL_ENTRY_ID;
		*resume_entry_id = NTMP_NULL_ENTRY_ID;
		goto end;
	}

	resp = (struct fdbt_resp_query *)req;
	*resume_entry_id = le32_to_cpu(resp->status);
	entry->entry_id = le32_to_cpu(resp->entry_id);
	entry->keye = resp->keye;
	entry->cfge = resp->cfge;
	entry->acte = resp->acte;

end:
	ntmp_free_data_mem(&data);

	return err;
}
EXPORT_SYMBOL_GPL(ntmp_fdbt_search_port_entry);

/**
 * ntmp_vft_add_entry - add an entry into the VLAN filter table
 * @user: target ntmp_user struct
 * @vid: VLAN ID
 * @cfge: configuration element data
 *
 * Return: 0 on success, otherwise a negative error code
 */
int ntmp_vft_add_entry(struct ntmp_user *user, u16 vid,
		       const struct vft_cfge_data *cfge)
{
	struct ntmp_dma_buf data = {
		.dev = user->dev,
		.size = sizeof(struct vft_req_ua),
	};
	struct vft_req_ua *req;
	union netc_cbd cbd;
	u32 len;
	int err;

	err = ntmp_alloc_data_mem(&data, (void **)&req);
	if (err)
		return err;

	/* Request data */
	ntmp_fill_crd(&req->crd, user->tbl.vft_ver, 0,
		      NTMP_GEN_UA_CFGEU);
	req->ak.exact.vid = cpu_to_le16(vid);
	req->cfge = *cfge;

	/* Request header */
	len = NTMP_LEN(data.size, NTMP_STATUS_RESP_LEN);
	ntmp_fill_request_hdr(&cbd, data.dma, len, NTMP_VFT_ID,
			      NTMP_CMD_ADD, NTMP_AM_EXACT_KEY);

	err = netc_xmit_ntmp_cmd(user, &cbd);
	if (err)
		dev_err(user->dev,
			"Failed to add %s entry, vid: %u, err: %pe\n",
			ntmp_table_name(NTMP_VFT_ID), vid, ERR_PTR(err));

	ntmp_free_data_mem(&data);

	return err;
}
EXPORT_SYMBOL_GPL(ntmp_vft_add_entry);

int ntmp_bpt_update_entry(struct ntmp_user *user, u32 entry_id,
			  const struct bpt_cfge_data *cfge)
{
	struct ntmp_dma_buf data = {
		.dev = user->dev,
		.size = sizeof(struct bpt_req_update),
	};
	struct bpt_req_update *req;
	union netc_cbd cbd;
	int err;

	err = ntmp_alloc_data_mem(&data, (void **)&req);
	if (err)
		return err;

	ntmp_fill_crd_eid(&req->rbe, user->tbl.bpt_ver, 0,
			  NTMP_GEN_UA_CFGEU | BPT_UA_BPSEU, entry_id);
	req->cfge = *cfge;
	ntmp_fill_request_hdr(&cbd, data.dma, NTMP_LEN(data.size, 0),
			      NTMP_BPT_ID, NTMP_CMD_UPDATE, NTMP_AM_ENTRY_ID);

	err = netc_xmit_ntmp_cmd(user, &cbd);
	if (err)
		dev_err(user->dev,
			"Failed to update %s entry 0x%x, err: %pe\n",
			ntmp_table_name(NTMP_BPT_ID), entry_id, ERR_PTR(err));

	ntmp_free_data_mem(&data);

	return err;
}
EXPORT_SYMBOL_GPL(ntmp_bpt_update_entry);

MODULE_DESCRIPTION("NXP NETC Library");
MODULE_LICENSE("Dual BSD/GPL");
