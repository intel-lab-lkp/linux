// SPDX-License-Identifier: GPL-2.0-only
/*
 * Implementation of ACPI RAS2 driver.
 *
 * Copyright (c) 2024-2025 HiSilicon Limited.
 *
 * Support for RAS2 - ACPI 6.5 Specification, section 5.2.21
 *
 * Driver contains ACPI RAS2 init, which extracts the ACPI RAS2 table and
 * get the PCC channel subspace for communicating with the ACPI compliant
 * HW platform which supports ACPI RAS2. Driver adds platform devices
 * for each RAS2 memory feature which binds to the memory ACPI RAS2 driver.
 */

#define pr_fmt(fmt)    "ACPI RAS2: " fmt

#include <linux/delay.h>
#include <linux/export.h>
#include <linux/ktime.h>
#include <acpi/pcc.h>
#include <acpi/ras2.h>

static struct acpi_table_ras2 *__read_mostly ras2_tab;

/* Data structure for PCC communication */
struct ras2_pcc_subspace {
	int pcc_id;
	struct mbox_client mbox_client;
	struct pcc_mbox_chan *pcc_chan;
	struct acpi_ras2_shmem __iomem *comm_addr;
	bool pcc_chnl_acq;
	ktime_t deadline;
	unsigned int pcc_mpar;
	unsigned int pcc_mrtt;
	struct list_head elem;
	u16 ref_count;
};

/*
 * Arbitrary Retries for PCC commands because the
 * remote processor could be much slower to reply.
 */
#define RAS2_NUM_RETRIES 600

#define RAS2_FEAT_TYPE_MEMORY        0x00

/* Global variables for the RAS2 PCC subspaces */
static DEFINE_MUTEX(ras2_pcc_lock);
static LIST_HEAD(ras2_pcc_subspaces);

static int ras2_report_cap_error(u32 cap_status)
{
	switch (cap_status) {
	case ACPI_RAS2_NOT_VALID:
	case ACPI_RAS2_NOT_SUPPORTED:
		return -EPERM;
	case ACPI_RAS2_BUSY:
		return -EBUSY;
	case ACPI_RAS2_FAILED:
	case ACPI_RAS2_ABORTED:
	case ACPI_RAS2_INVALID_DATA:
		return -EINVAL;
	default: /* 0 or other, Success */
		return 0;
	}
}

static int ras2_check_pcc_chan(struct ras2_pcc_subspace *pcc_subspace)
{
	struct acpi_ras2_shmem __iomem *gen_comm_base = pcc_subspace->comm_addr;
	ktime_t next_deadline = ktime_add(ktime_get(), pcc_subspace->deadline);
	u32 cap_status;
	u16 status;
	u32 rc;

	while (!ktime_after(ktime_get(), next_deadline)) {
		/*
		 * As per ACPI spec, the PCC space will be initialized by
		 * platform and should have set the command completion bit when
		 * PCC can be used by OSPM
		 */
		status = readw_relaxed(&gen_comm_base->status);
		if (status & RAS2_PCC_CMD_ERROR) {
			cap_status = readw_relaxed(&gen_comm_base->set_caps_status);
			rc = ras2_report_cap_error(cap_status);

			status &= ~RAS2_PCC_CMD_ERROR;
			writew_relaxed(status, &gen_comm_base->status);
			return rc;
		}

		if (status & RAS2_PCC_CMD_COMPLETE)
			return 0;
		/*
		 * Reducing the bus traffic in case this loop takes longer than
		 * a few retries.
		 */
		msleep(10);
	}

	return -EIO;
}

/**
 * ras2_send_pcc_cmd() - Send RAS2 command via PCC channel
 * @ras2_ctx:	pointer to the RAS2 context structure
 * @cmd:	command to send
 *
 * Returns: 0 on success, an error otherwise
 */
int ras2_send_pcc_cmd(struct ras2_mem_ctx *ras2_ctx, u16 cmd)
{
	struct ras2_pcc_subspace *pcc_subspace = ras2_ctx->pcc_subspace;
	struct acpi_ras2_shmem *gen_comm_base = pcc_subspace->comm_addr;
	static ktime_t last_cmd_cmpl_time, last_mpar_reset;
	struct mbox_chan *pcc_channel;
	unsigned int time_delta;
	static int mpar_count;
	int rc;

	guard(mutex)(&ras2_pcc_lock);
	rc = ras2_check_pcc_chan(pcc_subspace);
	if (rc < 0)
		return rc;

	pcc_channel = pcc_subspace->pcc_chan->mchan;

	/*
	 * Handle the Minimum Request Turnaround Time(MRTT)
	 * "The minimum amount of time that OSPM must wait after the completion
	 * of a command before issuing the next command, in microseconds"
	 */
	if (pcc_subspace->pcc_mrtt) {
		time_delta = ktime_us_delta(ktime_get(), last_cmd_cmpl_time);
		if (pcc_subspace->pcc_mrtt > time_delta)
			udelay(pcc_subspace->pcc_mrtt - time_delta);
	}

	/*
	 * Handle the non-zero Maximum Periodic Access Rate(MPAR)
	 * "The maximum number of periodic requests that the subspace channel can
	 * support, reported in commands per minute. 0 indicates no limitation."
	 *
	 * This parameter should be ideally zero or large enough so that it can
	 * handle maximum number of requests that all the cores in the system can
	 * collectively generate. If it is not, we will follow the spec and just
	 * not send the request to the platform after hitting the MPAR limit in
	 * any 60s window
	 */
	if (pcc_subspace->pcc_mpar) {
		if (mpar_count == 0) {
			time_delta = ktime_ms_delta(ktime_get(), last_mpar_reset);
			if (time_delta < 60 * MSEC_PER_SEC) {
				dev_dbg(ras2_ctx->dev,
					"PCC cmd not sent due to MPAR limit");
				return -EIO;
			}
			last_mpar_reset = ktime_get();
			mpar_count = pcc_subspace->pcc_mpar;
		}
		mpar_count--;
	}

	/* Write to the shared comm region. */
	writew_relaxed(cmd, &gen_comm_base->cmd);

	/* Flip CMD COMPLETE bit */
	writew_relaxed(0, &gen_comm_base->status);

	/* Ring doorbell */
	rc = mbox_send_message(pcc_channel, &cmd);
	if (rc < 0) {
		dev_err(ras2_ctx->dev,
			"Err sending PCC mbox message. cmd:%d, rc:%d\n",
			cmd, rc);
		return rc;
	}

	/*
	 * If Minimum Request Turnaround Time is non-zero, we need
	 * to record the completion time of both READ and WRITE
	 * command for proper handling of MRTT, so we need to check
	 * for pcc_mrtt in addition to CMD_READ
	 */
	if (cmd == RAS2_PCC_CMD_EXEC || pcc_subspace->pcc_mrtt) {
		rc = ras2_check_pcc_chan(pcc_subspace);
		if (pcc_subspace->pcc_mrtt)
			last_cmd_cmpl_time = ktime_get();
	}

	if (pcc_channel->mbox->txdone_irq)
		mbox_chan_txdone(pcc_channel, rc);
	else
		mbox_client_txdone(pcc_channel, rc);

	return rc >= 0 ? 0 : rc;
}
EXPORT_SYMBOL_GPL(ras2_send_pcc_cmd);

static int ras2_register_pcc_channel(struct ras2_mem_ctx *ras2_ctx, int pcc_id)
{
	struct ras2_pcc_subspace *pcc_subspace;
	struct pcc_mbox_chan *pcc_chan;
	struct mbox_client *mbox_cl;

	if (pcc_id < 0)
		return -EINVAL;

	mutex_lock(&ras2_pcc_lock);
	list_for_each_entry(pcc_subspace, &ras2_pcc_subspaces, elem) {
		if (pcc_subspace->pcc_id != pcc_id)
			continue;
		ras2_ctx->pcc_subspace = pcc_subspace;
		pcc_subspace->ref_count++;
		mutex_unlock(&ras2_pcc_lock);
		return 0;
	}
	mutex_unlock(&ras2_pcc_lock);

	pcc_subspace = kcalloc(1, sizeof(*pcc_subspace), GFP_KERNEL);
	if (!pcc_subspace)
		return -ENOMEM;

	mbox_cl = &pcc_subspace->mbox_client;
	mbox_cl->knows_txdone = true;

	pcc_chan = pcc_mbox_request_channel(mbox_cl, pcc_id);
	if (IS_ERR(pcc_chan)) {
		kfree(pcc_subspace);
		return PTR_ERR(pcc_chan);
	}

	*pcc_subspace = (struct ras2_pcc_subspace) {
		.pcc_id = pcc_id,
		.pcc_chan = pcc_chan,
		.comm_addr = acpi_os_ioremap(pcc_chan->shmem_base_addr,
					     pcc_chan->shmem_size),
		.deadline = ns_to_ktime(RAS2_NUM_RETRIES *
					pcc_chan->latency *
					NSEC_PER_USEC),
		.pcc_mrtt = pcc_chan->min_turnaround_time,
		.pcc_mpar = pcc_chan->max_access_rate,
		.mbox_client = {
			.knows_txdone = true,
		},
		.pcc_chnl_acq = true,
	};

	mutex_lock(&ras2_pcc_lock);
	list_add(&pcc_subspace->elem, &ras2_pcc_subspaces);
	pcc_subspace->ref_count++;
	mutex_unlock(&ras2_pcc_lock);
	ras2_ctx->pcc_subspace = pcc_subspace;
	ras2_ctx->comm_addr = pcc_subspace->comm_addr;
	ras2_ctx->dev = pcc_chan->mchan->mbox->dev;

	return 0;
}

static DEFINE_IDA(ras2_ida);
static void ras2_remove_pcc(struct ras2_mem_ctx *ras2_ctx)
{
	struct ras2_pcc_subspace *pcc_subspace = ras2_ctx->pcc_subspace;

	guard(mutex)(&ras2_pcc_lock);
	if (pcc_subspace->ref_count > 0)
		pcc_subspace->ref_count--;

	if (!pcc_subspace->ref_count) {
		list_del(&pcc_subspace->elem);
		pcc_mbox_free_channel(pcc_subspace->pcc_chan);
		kfree(pcc_subspace);
	}
}

static void ras2_release(struct device *device)
{
	struct auxiliary_device *auxdev = container_of(device, struct auxiliary_device, dev);
	struct ras2_mem_ctx *ras2_ctx = container_of(auxdev, struct ras2_mem_ctx, adev);

	ida_free(&ras2_ida, auxdev->id);
	ras2_remove_pcc(ras2_ctx);
	kfree(ras2_ctx);
}

static int ras2_add_aux_device(char *name, int channel)
{
	struct ras2_mem_ctx *ras2_ctx;
	int id, rc;

	ras2_ctx = kzalloc(sizeof(*ras2_ctx), GFP_KERNEL);
	if (!ras2_ctx)
		return -ENOMEM;

	mutex_init(&ras2_ctx->lock);

	rc = ras2_register_pcc_channel(ras2_ctx, channel);
	if (rc < 0) {
		pr_debug("failed to register pcc channel rc=%d\n", rc);
		goto ctx_free;
	}

	id = ida_alloc(&ras2_ida, GFP_KERNEL);
	if (id < 0) {
		rc = id;
		goto pcc_free;
	}

	ras2_ctx->id = id;
	ras2_ctx->adev.id = id;
	ras2_ctx->adev.name = RAS2_MEM_DEV_ID_NAME;
	ras2_ctx->adev.dev.release = ras2_release;
	ras2_ctx->adev.dev.parent = ras2_ctx->dev;

	rc = auxiliary_device_init(&ras2_ctx->adev);
	if (rc)
		goto ida_free;

	rc = auxiliary_device_add(&ras2_ctx->adev);
	if (rc) {
		auxiliary_device_uninit(&ras2_ctx->adev);
		return rc;
	}

	return 0;

ida_free:
	ida_free(&ras2_ida, id);
pcc_free:
	ras2_remove_pcc(ras2_ctx);
ctx_free:
	kfree(ras2_ctx);

	return rc;
}

static int acpi_ras2_parse(void)
{
	struct acpi_ras2_pcc_desc *pcc_desc_list;
	int pcc_id;
	u8 count = 0;
	int rc, i;

	if (ras2_tab->header.length  < sizeof(struct acpi_table_ras2)) {
		pr_warn(FW_WARN "ACPI RAS2 table present but broken (too short #1)\n");
		return -EINVAL;
	}

	if (!ras2_tab->num_pcc_descs) {
		pr_warn(FW_WARN "No PCC descs in ACPI RAS2 table\n");
		return -EINVAL;
	}

	pcc_desc_list = (struct acpi_ras2_pcc_desc *)(ras2_tab + 1);
	/* Double scan for the case of only one actual controller */
	pcc_id = -1;
	count = 0;
	for (i = 0; i < ras2_tab->num_pcc_descs; i++, pcc_desc_list++) {
		if (pcc_desc_list->feature_type != RAS2_FEAT_TYPE_MEMORY)
			continue;
		if (pcc_id == -1) {
			pcc_id = pcc_desc_list->channel_id;
			count++;
		}
		if (pcc_desc_list->channel_id != pcc_id)
			count++;
	}

	/*
	 * Workaround for the client platform with multiple scrub devices
	 * but uses single PCC subspace for communication.
	 */
	if (count == 1) {
		/* Add auxiliary device and bind ACPI RAS2 memory driver */
		rc = ras2_add_aux_device(RAS2_MEM_DEV_ID_NAME, pcc_id);
		if (rc)
			return rc;

		return 0;
	}

	pcc_desc_list = (struct acpi_ras2_pcc_desc *)(ras2_tab + 1);
	count = 0;
	for (i = 0; i < ras2_tab->num_pcc_descs; i++, pcc_desc_list++) {
		if (pcc_desc_list->feature_type != RAS2_FEAT_TYPE_MEMORY)
			continue;
		pcc_id = pcc_desc_list->channel_id;
		/* Add auxiliary device and bind ACPI RAS2 memory driver */
		rc = ras2_add_aux_device(RAS2_MEM_DEV_ID_NAME, pcc_id);
		if (rc)
			return rc;
	}

	return 0;
}

void __init acpi_ras2_init(void)
{
	acpi_status status;
	int rc;

	status = acpi_get_table(ACPI_SIG_RAS2, 0,
				(struct acpi_table_header **)&ras2_tab);
	if (ACPI_FAILURE(status) || !ras2_tab) {
		const char *msg = acpi_format_exception(status);

		pr_err("Failed to get table, %s\n", msg);
		return;
	}

	rc = acpi_ras2_parse();
	if (rc) {
		acpi_put_table((struct acpi_table_header *)ras2_tab);
		pr_err("Failed to parse RAS2 table\n");
	}
}
