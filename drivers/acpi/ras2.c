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
 * HW platform which supports ACPI RAS2. Driver adds auxiliary devices
 * for each RAS2 memory feature which binds to the memory ACPI RAS2 driver.
 */

#define pr_fmt(fmt) "ACPI RAS2: " fmt

#include <linux/delay.h>
#include <linux/export.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <acpi/pcc.h>
#include <acpi/ras2.h>

static struct acpi_table_ras2 *__read_mostly ras2_tab;

/**
 * struct ras2_pcc_subspace - Data structure for PCC communication
 * @mbox_client:	struct mbox_client object
 * @pcc_chan:		Pointer to struct pcc_mbox_chan
 * @comm_addr:		Pointer to RAS2 PCC shared memory region
 * @elem:		List for registered RAS2 PCC channel subspaces
 * @pcc_lock:		PCC lock to provide mutually exclusive access
 *			to PCC channel subspace
 * @deadline_us:	Poll PCC status register timeout in micro secs
 *			for PCC command complete
 * @pcc_mpar:		Maximum Periodic Access Rate(MPAR) for PCC channel
 * @pcc_mrtt:		Minimum Request Turnaround Time(MRTT) in micro secs
 *			OS must wait after completion of a PCC command before
 *			issue next command
 * @last_cmd_cmpl_time:	completion time of last PCC command
 * @last_mpar_reset:	Time of last MPAR count reset.
 * @mpar_count:		MPAR count
 * @pcc_id:		Identifier of the RAS2 platform communication channel
 * @pcc_chnl_acq:	Status of PCC channel acquired.
 * @kref:		kref object
 */
struct ras2_pcc_subspace {
	struct mbox_client		mbox_client;
	struct pcc_mbox_chan		*pcc_chan;
	struct acpi_ras2_shmem __iomem	*comm_addr;
	struct list_head		elem;
	struct mutex			pcc_lock;
	unsigned int			deadline_us;
	unsigned int			pcc_mpar;
	unsigned int			pcc_mrtt;
	ktime_t				last_cmd_cmpl_time;
	ktime_t				last_mpar_reset;
	int				mpar_count;
	int				pcc_id;
	bool				pcc_chnl_acq;
	struct kref			kref;
};

/*
 * Arbitrary retries for PCC commands because the remote processor
 * could be much slower to reply. Keeping it high enough to cover
 * emulators where the processors run painfully slow.
 */
#define RAS2_NUM_RETRIES 600ULL

#define RAS2_FEAT_TYPE_MEMORY 0x00

/* Static variables for the RAS2 PCC subspaces */
static DEFINE_MUTEX(ras2_pcc_list_lock);
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
	u32 cap_status;
	u16 status;
	u32 rc;

	/*
	 * As per ACPI spec, the PCC space will be initialized by
	 * platform and should have set the command completion bit when
	 * PCC can be used by OSPM.
	 *
	 * Poll PCC status register every 3us(delay_us) for maximum of
	 * deadline_us(timeout_us) until PCC command complete bit is set(cond).
	 */
	rc = readw_relaxed_poll_timeout(&gen_comm_base->status, status,
					status & PCC_STATUS_CMD_COMPLETE, 3,
					pcc_subspace->deadline_us);
	if (rc) {
		pr_warn("PCC check channel failed for : %d rc=%d\n",
			pcc_subspace->pcc_id, rc);
		return rc;
	}

	if (status & PCC_STATUS_ERROR) {
		cap_status = readw_relaxed(&gen_comm_base->set_caps_status);
		rc = ras2_report_cap_error(cap_status);

		status &= ~PCC_STATUS_ERROR;
		writew_relaxed(status, &gen_comm_base->status);
		return rc;
	}

	if (status & PCC_STATUS_CMD_COMPLETE)
		return 0;

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
	struct mbox_chan *pcc_channel;
	unsigned int time_delta;
	int rc;

	rc = ras2_check_pcc_chan(pcc_subspace);
	if (rc < 0)
		return rc;

	pcc_channel = pcc_subspace->pcc_chan->mchan;

	/*
	 * Handle the Minimum Request Turnaround Time(MRTT).
	 * "The minimum amount of time that OSPM must wait after the completion
	 * of a command before issuing the next command, in microseconds."
	 */
	if (pcc_subspace->pcc_mrtt) {
		time_delta = ktime_us_delta(ktime_get(),
					    pcc_subspace->last_cmd_cmpl_time);
		if (pcc_subspace->pcc_mrtt > time_delta)
			udelay(pcc_subspace->pcc_mrtt - time_delta);
	}

	/*
	 * Handle the non-zero Maximum Periodic Access Rate(MPAR).
	 * "The maximum number of periodic requests that the subspace channel can
	 * support, reported in commands per minute. 0 indicates no limitation."
	 *
	 * This parameter should be ideally zero or large enough so that it can
	 * handle maximum number of requests that all the cores in the system can
	 * collectively generate. If it is not, we will follow the spec and just
	 * not send the request to the platform after hitting the MPAR limit in
	 * any 60s window.
	 */
	if (pcc_subspace->pcc_mpar) {
		if (pcc_subspace->mpar_count == 0) {
			time_delta = ktime_ms_delta(ktime_get(),
						    pcc_subspace->last_mpar_reset);
			if (time_delta < 60 * MSEC_PER_SEC) {
				dev_dbg(ras2_ctx->dev,
					"PCC cmd not sent due to MPAR limit");
				return -EIO;
			}
			pcc_subspace->last_mpar_reset = ktime_get();
			pcc_subspace->mpar_count = pcc_subspace->pcc_mpar;
		}
		pcc_subspace->mpar_count--;
	}

	/* Write to the shared comm region */
	writew_relaxed(cmd, &gen_comm_base->command);

	/* Flip CMD COMPLETE bit */
	writew_relaxed(0, &gen_comm_base->status);

	/* Ring doorbell */
	rc = mbox_send_message(pcc_channel, &cmd);
	if (rc < 0) {
		dev_warn(ras2_ctx->dev,
			 "Err sending PCC mbox message. cmd:%d, rc:%d\n", cmd, rc);
		return rc;
	}

	/*
	 * If Minimum Request Turnaround Time is non-zero, we need
	 * to record the completion time of both READ and WRITE
	 * command for proper handling of MRTT, so we need to check
	 * for pcc_mrtt in addition to CMD_READ.
	 */
	if (cmd == PCC_CMD_EXEC_RAS2 || pcc_subspace->pcc_mrtt) {
		rc = ras2_check_pcc_chan(pcc_subspace);
		if (pcc_subspace->pcc_mrtt)
			pcc_subspace->last_cmd_cmpl_time = ktime_get();
	}

	if (pcc_channel->mbox->txdone_irq)
		mbox_chan_txdone(pcc_channel, rc);
	else
		mbox_client_txdone(pcc_channel, rc);

	return rc >= 0 ? 0 : rc;
}
EXPORT_SYMBOL_GPL(ras2_send_pcc_cmd);

static void ras2_list_pcc_release(struct kref *kref)
{
	struct ras2_pcc_subspace *pcc_subspace =
		container_of(kref, struct ras2_pcc_subspace, kref);

	guard(mutex)(&ras2_pcc_list_lock);
	list_del(&pcc_subspace->elem);
	pcc_mbox_free_channel(pcc_subspace->pcc_chan);
	kfree(pcc_subspace);
}

static void ras2_pcc_get(struct ras2_pcc_subspace *pcc_subspace)
{
	kref_get(&pcc_subspace->kref);
}

static void ras2_pcc_put(struct ras2_pcc_subspace *pcc_subspace)
{
	kref_put(&pcc_subspace->kref,  &ras2_list_pcc_release);
}

static struct ras2_pcc_subspace *ras2_get_pcc_subspace(int pcc_id)
{
	struct ras2_pcc_subspace *pcc_subspace;

	mutex_lock(&ras2_pcc_list_lock);
	list_for_each_entry(pcc_subspace, &ras2_pcc_subspaces, elem) {
		if (pcc_subspace->pcc_id != pcc_id)
			continue;
		ras2_pcc_get(pcc_subspace);
		mutex_unlock(&ras2_pcc_list_lock);
		return pcc_subspace;
	}
	mutex_unlock(&ras2_pcc_list_lock);

	return NULL;
}

static int ras2_register_pcc_channel(struct ras2_mem_ctx *ras2_ctx, int pcc_id)
{
	struct ras2_pcc_subspace *pcc_subspace;
	struct pcc_mbox_chan *pcc_chan;
	struct mbox_client *mbox_cl;

	if (pcc_id < 0)
		return -EINVAL;

	pcc_subspace = ras2_get_pcc_subspace(pcc_id);
	if (pcc_subspace) {
		ras2_ctx->pcc_subspace	= pcc_subspace;
		ras2_ctx->comm_addr	= pcc_subspace->comm_addr;
		ras2_ctx->dev		= pcc_subspace->pcc_chan->mchan->mbox->dev;
		ras2_ctx->pcc_lock	= &pcc_subspace->pcc_lock;
		return 0;
	}

	pcc_subspace = kzalloc(sizeof(*pcc_subspace), GFP_KERNEL);
	if (!pcc_subspace)
		return -ENOMEM;

	mbox_cl			= &pcc_subspace->mbox_client;
	mbox_cl->knows_txdone	= true;

	pcc_chan = pcc_mbox_request_channel(mbox_cl, pcc_id);
	if (IS_ERR(pcc_chan)) {
		kfree(pcc_subspace);
		return PTR_ERR(pcc_chan);
	}

	pcc_subspace->pcc_id		= pcc_id;
	pcc_subspace->pcc_chan		= pcc_chan;
	pcc_subspace->comm_addr		= acpi_os_ioremap(pcc_chan->shmem_base_addr,
							  pcc_chan->shmem_size);
	pcc_subspace->deadline_us	= RAS2_NUM_RETRIES * pcc_chan->latency;
	pcc_subspace->pcc_mrtt		= pcc_chan->min_turnaround_time;
	pcc_subspace->pcc_mpar		= pcc_chan->max_access_rate;
	pcc_subspace->mbox_client.knows_txdone	= true;
	pcc_subspace->pcc_chnl_acq	= true;

	kref_init(&pcc_subspace->kref);

	mutex_lock(&ras2_pcc_list_lock);
	list_add(&pcc_subspace->elem, &ras2_pcc_subspaces);
	ras2_pcc_get(pcc_subspace);
	mutex_unlock(&ras2_pcc_list_lock);

	ras2_ctx->pcc_subspace	= pcc_subspace;
	ras2_ctx->comm_addr	= pcc_subspace->comm_addr;
	ras2_ctx->dev		= pcc_chan->mchan->mbox->dev;

	mutex_init(&pcc_subspace->pcc_lock);
	ras2_ctx->pcc_lock	= &pcc_subspace->pcc_lock;

	return 0;
}

static DEFINE_IDA(ras2_ida);
static void ras2_release(struct device *device)
{
	struct auxiliary_device *auxdev = container_of(device, struct auxiliary_device, dev);
	struct ras2_mem_ctx *ras2_ctx = container_of(auxdev, struct ras2_mem_ctx, adev);

	ida_free(&ras2_ida, auxdev->id);
	ras2_pcc_put(ras2_ctx->pcc_subspace);
	kfree(ras2_ctx);
}

static int ras2_add_aux_device(char *name, int channel)
{
	struct ras2_mem_ctx *ras2_ctx;
	int id, rc;

	ras2_ctx = kzalloc(sizeof(*ras2_ctx), GFP_KERNEL);
	if (!ras2_ctx)
		return -ENOMEM;

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

	ras2_ctx->id			= id;
	ras2_ctx->adev.id		= id;
	ras2_ctx->adev.name		= RAS2_MEM_DEV_ID_NAME;
	ras2_ctx->adev.dev.release	= ras2_release;
	ras2_ctx->adev.dev.parent	= ras2_ctx->dev;

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
	ras2_pcc_put(ras2_ctx->pcc_subspace);
ctx_free:
	kfree(ras2_ctx);

	return rc;
}

static int acpi_ras2_parse(void)
{
	struct acpi_ras2_pcc_desc *pcc_desc_list;
	u16 i, count;
	int pcc_id;
	int rc;

	if (ras2_tab->header.length < sizeof(*ras2_tab)) {
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
	for (i = 0, count = 0; i < ras2_tab->num_pcc_descs; i++, pcc_desc_list++) {
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
		return ras2_add_aux_device(RAS2_MEM_DEV_ID_NAME, pcc_id);
	}

	pcc_desc_list = (struct acpi_ras2_pcc_desc *)(ras2_tab + 1);
	for (i = 0; i < ras2_tab->num_pcc_descs; i++, pcc_desc_list++) {
		if (pcc_desc_list->feature_type != RAS2_FEAT_TYPE_MEMORY)
			continue;

		rc = ras2_add_aux_device(RAS2_MEM_DEV_ID_NAME, pcc_desc_list->channel_id);
		if (rc)
			pr_warn("Failed to add RAS2 auxiliary device rc=%d\n", rc);
	}

	return 0;
}

void __init acpi_ras2_init(void)
{
	acpi_status status;

	status = acpi_get_table(ACPI_SIG_RAS2, 0,
				(struct acpi_table_header **)&ras2_tab);
	if (ACPI_FAILURE(status)) {
		pr_err("Failed to get table, %s\n", acpi_format_exception(status));
		return;
	}

	if (acpi_ras2_parse())
		pr_err("Failed to parse RAS2 table\n");

	acpi_put_table((struct acpi_table_header *)ras2_tab);
}
