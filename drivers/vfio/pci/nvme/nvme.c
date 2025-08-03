// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022, INTEL CORPORATION. All rights reserved
 * Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved
 */

#include <linux/device.h>
#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/types.h>
#include <linux/vfio.h>
#include <linux/anon_inodes.h>
#include <linux/kernel.h>
#include <linux/vfio_pci_core.h>

#include "nvme.h"

#define MAX_MIGRATION_SIZE (256 * 1024)

static void nvmevf_disable_fd(struct nvmevf_migration_file *migf)
{
	mutex_lock(&migf->lock);

	/* release the device states buffer */
	kvfree(migf->vf_data);
	migf->vf_data = NULL;
	migf->disabled = true;
	migf->total_length = 0;
	migf->filp->f_pos = 0;
	mutex_unlock(&migf->lock);
}

static void nvmevf_disable_fds(struct nvmevf_pci_core_device *nvmevf_dev)
{
	if (nvmevf_dev->resuming_migf) {
		nvmevf_disable_fd(nvmevf_dev->resuming_migf);
		fput(nvmevf_dev->resuming_migf->filp);
		nvmevf_dev->resuming_migf = NULL;
	}

	if (nvmevf_dev->saving_migf) {
		nvmevf_disable_fd(nvmevf_dev->saving_migf);
		fput(nvmevf_dev->saving_migf->filp);
		nvmevf_dev->saving_migf = NULL;
	}
}

static void nvmevf_state_mutex_unlock(struct nvmevf_pci_core_device *nvmevf_dev)
{
	lockdep_assert_held(&nvmevf_dev->state_mutex);
again:
	spin_lock(&nvmevf_dev->reset_lock);
	if (nvmevf_dev->deferred_reset) {
		nvmevf_dev->deferred_reset = false;
		spin_unlock(&nvmevf_dev->reset_lock);
		nvmevf_dev->mig_state = VFIO_DEVICE_STATE_RUNNING;
		nvmevf_disable_fds(nvmevf_dev);
		goto again;
	}
	mutex_unlock(&nvmevf_dev->state_mutex);
	spin_unlock(&nvmevf_dev->reset_lock);
}

static struct nvmevf_pci_core_device *nvmevf_drvdata(struct pci_dev *pdev)
{
	struct vfio_pci_core_device *core_device = dev_get_drvdata(&pdev->dev);

	return container_of(core_device, struct nvmevf_pci_core_device,
			    core_device);
}

/*
 * Convert byte length to nvme's 0-based num dwords
 */
static inline u32 bytes_to_nvme_numd(size_t len)
{
	if (len < 4)
		return 0;
	return (len >> 2) - 1;
}

static int nvmevf_cmd_suspend_device(struct nvmevf_pci_core_device *nvmevf_dev)
{
	struct pci_dev *dev = nvmevf_dev->core_device.pdev;
	struct nvme_command c = { };
	u32 cdw11 = NVME_LM_SUSPEND_TYPE_SUSPEND << 16 | nvme_get_ctrl_id(dev);
	int ret;

	c.lm.send.opcode = nvme_admin_lm_send;
	c.lm.send.cdw10 = cpu_to_le32(NVME_LM_SEND_SEL_SUSPEND);
	c.lm.send.cdw11 = cpu_to_le32(cdw11);

	ret = nvme_submit_vf_cmd(dev, &c, NULL, NULL, 0);
	if (ret) {
		dev_warn(&dev->dev,
			 "Suspend virtual function failed (ret=0x%x)\n",
			 ret);
		return ret;
	}

	dev_dbg(&dev->dev, "Suspend command successful\n");
	return 0;
}

static int nvmevf_cmd_resume_device(struct nvmevf_pci_core_device *nvmevf_dev)
{
	struct pci_dev *dev = nvmevf_dev->core_device.pdev;
	struct nvme_command c = { };
	int ret;

	c.lm.send.opcode = nvme_admin_lm_send;
	c.lm.send.cdw10 = cpu_to_le32(NVME_LM_SEND_SEL_RESUME);
	c.lm.send.cdw11 = cpu_to_le32(nvme_get_ctrl_id(dev));

	ret = nvme_submit_vf_cmd(dev, &c, NULL, NULL, 0);
	if (ret) {
		dev_warn(&dev->dev,
			 "Resume virtual function failed (ret=0x%x)\n", ret);
		return ret;
	}
	dev_dbg(&dev->dev, "Resume command successful\n");
	return 0;
}

/**
 * Figure SCSF-FIG1: Supported Controller State Formats Data Structure
 * nvme_lm_get_ctrl_state_fmts - Query and parse CNS=0x20 format list
 * @dev:  Controller pci device
 * @fmt:  Output struct populated with NV, NUUID, and pointers
 *
 * Issues Identify CNS=0x20 (Supported Controller State Formats),
 * allocates a buffer, and parses the result into the provided struct.
 *
 * The caller must free fmt->ctrl_state_raw_buf using kfree().
 *
 * Returns 0 on success, or a negative errno on failure.
 */
static int nvme_lm_id_ctrl_state(struct pci_dev *dev,
				 struct nvme_lm_ctrl_state_fmts_info *fmt)
{
	struct nvme_command c = { };
	void *buf;
	int ret;
	__u8 nv, nuuid;
	size_t len;

	if (!fmt)
		return -EINVAL;

	/* Step 1: Read first 2 bytes to get NV and NUUID */
	buf = kzalloc(2, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	c.identify.opcode = nvme_admin_identify;
	c.identify.cns = NVME_ID_CNS_LM_CTRL_STATE_FMT;
	c.identify.nsid = cpu_to_le32(0);

	ret = nvme_submit_vf_cmd(dev, &c, NULL, buf, 2);
	if (ret)
		goto out_free;

	nv = ((__u8 *)buf)[0];
	nuuid = ((__u8 *)buf)[1];

	kfree(buf);

	/*
	 * Compute total buffer length for the full Identify CNS=0x20 response:
	 *
	 * - The first 2 bytes hold the header:
	 *     * Byte 0: NV     — number of NVMe-defined format versions
	 *     * Byte 1: NUUID  — number of vendor-specific UUID entries
	 *
	 * - Each version entry is 2 bytes (VERSION_ENTRY_SIZE)
	 * - Each UUID entry is 16 bytes (UUID_ENTRY_SIZE)
	 *
	 * Therefore:
	 *   Total length = 2 + (NV * 2) + (NUUID * 16)
	 */
	len = NVME_LM_CTRL_STATE_HDR_SIZE +
	nv * NVME_LM_VERSION_ENTRY_SIZE + nuuid * NVME_LM_UUID_ENTRY_SIZE;

	buf = kzalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memset(&c, 0, sizeof(c));
	c.identify.opcode = nvme_admin_identify;
	c.identify.cns = NVME_ID_CNS_LM_CTRL_STATE_FMT;
	c.identify.nsid = cpu_to_le32(0);

	ret = nvme_submit_vf_cmd(dev, &c, NULL, buf, len);
	if (ret)
		goto out_free;

	/* Parse the result in-place */
	fmt->nv = nv;
	fmt->nuuid = nuuid;
	fmt->vers = ((struct nvme_lm_supported_ctrl_state_fmts *)buf)->vers;
	fmt->uuids = (const void *)(fmt->vers + nv);
	fmt->ctrl_state_raw_buf = buf;
	fmt->raw_len = len;

	return 0;

out_free:
	kfree(buf);
	return ret;
}

static int nvme_lm_get_ctrl_state_fmt(struct pci_dev *dev, bool debug,
				      struct nvme_lm_ctrl_state_fmts_info *fmt)
{
	__u8 i;
	int ret;

	ret = nvme_lm_id_ctrl_state(dev, fmt);
	if (ret) {
		pr_err("Failed to get ctrl state formats (ret=%d)\n", ret);
		return ret;
	}

	if (debug)
		pr_info("NV = %u, NUUID = %u\n", fmt->nv, fmt->nuuid);

	if (debug) {
		for (i = 0; i < fmt->nv; i++) {
			pr_info("  Format[%d] Version = 0x%04x\n",
					i, le16_to_cpu(fmt->vers[i]));
		}

		for (i = 0; i < fmt->nuuid; i++) {
			char uuid_str[37]; /* 36 chars + null */

			snprintf(uuid_str, sizeof(uuid_str),
					"%02x%02x%02x%02x-%02x%02x-%02x%02x-"
					"%02x%02x-%02x%02x%02x%02x%02x%02x",
					fmt->uuids[i][0], fmt->uuids[i][1],
					fmt->uuids[i][2], fmt->uuids[i][3],
					fmt->uuids[i][4], fmt->uuids[i][5],
					fmt->uuids[i][6], fmt->uuids[i][7],
					fmt->uuids[i][8], fmt->uuids[i][9],
					fmt->uuids[i][10], fmt->uuids[i][11],
					fmt->uuids[i][12], fmt->uuids[i][13],
					fmt->uuids[i][14], fmt->uuids[i][15]);

			pr_info("  UUID[%d] = %s\n", i, uuid_str);
		}
	}

	return ret;
}

static void nvmevf_init_get_ctrl_state_cmd(struct nvme_command *c, __u16 cntlid,
					   __u8 csvi, __u8 csuuidi,
					   __u8 csuidxp, size_t buf_len)
{
	c->lm.recv.opcode = nvme_admin_lm_recv;
	c->lm.recv.sel = NVME_LM_RECV_GET_CTRL_STATE;
	/*
	 * MOS fields treated as ctrl state version index, Use NVME V1 state.
	 */
	/*
	 * For upstream read the supported controller state formats using
	 * identify command with cns value 0x20 and make sure NVME_LM_CSVI
	 * matches the on of the reported formats for NVMe states.
	 */
	c->lm.recv.mos = cpu_to_le16(csvi);
	/* Target Controller is this a right way to get the controller ID */
	c->lm.recv.cntlid = cpu_to_le16(cntlid);

	/*
	 * For upstream read the supported controller state formats using
	 * identify command with cns value 0x20 and make sure NVME_LM_CSVI
	 * matches the on of the reported formats for Vender specific states.
	 */
	/* adjust the state as per needed by setting the macro values */
	c->lm.recv.csuuidi = cpu_to_le32(csuuidi);
	c->lm.recv.csuidxp = cpu_to_le32(csuidxp);

	/*
	 * Associates the Migration Receive command with the correct migration
	 * session UUID currently we set to 0. For now asssume that initiaor
	 * and target has agreed on the UUIDX 0 for all the live migration
	 * sessions.
	 */
	c->lm.recv.uuid_index = cpu_to_le32(0);

	/*
	 * Assume that data buffer is big enoough to hold the state,
	 * 0-based dword count.
	 */
	c->lm.recv.numd = cpu_to_le32(bytes_to_nvme_numd(buf_len));
}

#define NVME_LM_MAX_NVMECS	1024
#define NVME_LM_MAX_VSD		1024

static int nvmevf_get_ctrl_state(struct pci_dev *dev,
				__u8 csvi, __u8 csuuidi, __u8 csuidxp,
				struct nvmevf_migration_file *migf,
				struct nvme_lm_ctrl_state_info *state)
{
	struct nvme_command c = { };
	struct nvme_lm_ctrl_state *hdr;
	/* Make sure hdr_len is a multiple of 4 */
	size_t hdr_len = ALIGN(sizeof(*hdr), 4);
	__u16 id = nvme_get_ctrl_id(dev);
	void *local_buf;
	size_t len;
	int ret;

	/* Step 1: Issue Migration Receive (Select = 0) to get header */
	local_buf = kzalloc(hdr_len, GFP_KERNEL);
	if (!local_buf)
		return -ENOMEM;

	nvmevf_init_get_ctrl_state_cmd(&c, id, csvi, csuuidi, csuidxp, hdr_len);
	ret = nvme_submit_vf_cmd(dev, &c, NULL, local_buf, hdr_len);
	if (ret) {
		dev_warn(&dev->dev,
			"nvme_admin_lm_recv failed (ret=0x%x)\n", ret);
		kfree(local_buf);
		return ret;
	}

	if (le16_to_cpu(hdr->nvmecss) > NVME_LM_MAX_NVMECS ||
	    le16_to_cpu(hdr->vss) > NVME_LM_MAX_VSD) {
		kfree(local_buf);
		return -EINVAL;
	}

	hdr = local_buf;
	len = hdr_len + 4 * (le16_to_cpu(hdr->nvmecss) + le16_to_cpu(hdr->vss));

	kfree(local_buf);

	if (len == hdr_len)
		dev_warn(&dev->dev, "nvmecss == 0 or vss = 0\n");

	/* Step 2: Allocate full buffer */
	migf->total_length = len;
	migf->vf_data = kvzalloc(migf->total_length, GFP_KERNEL);
	if (!migf->vf_data)
		return -ENOMEM;

	memset(&c, 0, sizeof(c));
	nvmevf_init_get_ctrl_state_cmd(&c, id, csvi, csuuidi, csuidxp, len);
	ret = nvme_submit_vf_cmd(dev, &c, NULL, migf->vf_data, len);
	if (ret)
		goto free_big;

	/* Populate state struct */
	hdr = (struct nvme_lm_ctrl_state *)migf->vf_data;
	state->raw = hdr;
	state->total_len = len;
	state->version = hdr->version;
	state->csattr = hdr->csattr;
	state->nvmecss = hdr->nvmecss;
	state->vss = hdr->vss;
	state->nvme_cs = hdr->data;
	state->vsd = hdr->data + le16_to_cpu(hdr->nvmecss) * 4;

	return ret;

free_big:
	kvfree(migf->vf_data);
	return ret;
}

static const struct nvme_lm_nvme_cs_v0_state *
nvme_lm_parse_nvme_cs_v0_state(const void *data, size_t len, u16 *niosq,
			       u16 *niocq)
{
	const struct nvme_lm_nvme_cs_v0_state *hdr = data;
	size_t hdr_len = sizeof(*hdr);
	size_t iosq_sz, iocq_sz, total;
	u16 sq, cq;

	if (!data || len < hdr_len)
		return NULL;

	sq = le16_to_cpu(hdr->niosq);
	cq = le16_to_cpu(hdr->niocq);

	iosq_sz = sq * sizeof(struct nvme_lm_iosq_state);
	iocq_sz = cq * sizeof(struct nvme_lm_iocq_state);
	total = hdr_len + iosq_sz + iocq_sz;

	if (len < total)
		return NULL;

	if (niosq)
		*niosq = sq;
	if (niocq)
		*niocq = cq;

	return hdr;
}

static void nvme_lm_debug_ctrl_state(struct nvme_lm_ctrl_state_info *state)
{
	const struct nvme_lm_nvme_cs_v0_state *cs;
	const struct nvme_lm_iosq_state *iosq;
	const struct nvme_lm_iocq_state *iocq;
	u16 niosq, niocq;
	int i;

	pr_info("Controller State:\n");
	pr_info("Version    : 0x%04x\n", le16_to_cpu(state->version));
	pr_info("CSATTR     : 0x%02x\n", state->csattr);
	pr_info("NVMECS Len : %u bytes\n", le16_to_cpu(state->nvmecss) * 4);
	pr_info("VSD Len    : %u bytes\n", le16_to_cpu(state->vss) * 4);

	cs = nvme_lm_parse_nvme_cs_v0_state(state->nvme_cs,
					    le16_to_cpu(state->nvmecss) * 4,
					    &niosq, &niocq);
	if (!cs) {
		pr_warn("Failed to parse NVMECS\n");
		return;
	}

	iosq = cs->iosq;
	iocq = (const void *)(iosq + niosq);

	for (i = 0; i < niosq; i++) {
		pr_info("IOSQ[%d]: SIZE=%u QID=%u CQID=%u ATTR=0x%x Head=%u "
			"Tail=%u\n", i,
			le16_to_cpu(iosq[i].qsize),
			le16_to_cpu(iosq[i].qid),
			le16_to_cpu(iosq[i].cqid),
			le16_to_cpu(iosq[i].attr),
			le16_to_cpu(iosq[i].head),
			le16_to_cpu(iosq[i].tail));
	}

	for (i = 0; i < niocq; i++) {
		pr_info("IOCQ[%d]: SIZE=%u QID=%u ATTR=%u Head=%u Tail=%u\n", i,
			le16_to_cpu(iocq[i].qsize),
			le16_to_cpu(iocq[i].qid),
			le16_to_cpu(iocq[i].attr),
			le16_to_cpu(iocq[i].head),
			le16_to_cpu(iocq[i].tail));
	}
}

#define NVME_LM_CSUUIDI	0
#define NVME_LM_CSVI	NVME_LM_RECV_CSVI_NVME_V1

static int nvmevf_cmd_get_ctrl_state(struct nvmevf_pci_core_device *nvmevf_dev,
				     struct nvmevf_migration_file *migf)
{
	struct pci_dev *dev = nvmevf_dev->core_device.pdev;
	struct nvme_lm_ctrl_state_fmts_info fmt = { };
	struct nvme_lm_ctrl_state_info state = { };
	__u8 csvi = NVME_LM_CSVI;
	__u8 csuuidi = NVME_LM_CSUUIDI;
	__u8 csuidxp = 0;
	int ret;

	/*
	 * Read the supported controller state formats to make sure they match
	 * csvi value specified in vfio-nvme without this check we'd not know
	 * which controller state format we are working with.
	 */
	ret = nvme_lm_get_ctrl_state_fmt(dev, true, &fmt);
	if (ret)
		return ret;
	/*
	 * Number of versions NV cannot be less than controller state version
	 * index we are using, it's an error. Please note that CSVI is
	 * a configurable value user can define this macro at the compile time
	 * to select the required NVMe controller state version index from
	 * Supported Controller State Formats Data Structure.
	 */
	if (fmt.nv < csvi) {
		dev_warn(&dev->dev,
			 "required ctrl state format not found\n");
		ret = -EINVAL;
		goto out;
	}

	ret = nvmevf_get_ctrl_state(dev, csvi, csuuidi, csuidxp, migf, &state);
	if (ret)
		goto out;

	if (le16_to_cpu(state.version) != csvi) {
		dev_warn(&dev->dev,
			 "Unexpected controller state version: 0x%04x\n",
			 le16_to_cpu(state.version));
		ret = -EINVAL;
		goto out;
	}

	/*
	 * Now that we have received the controller state decode the state
	 * properly for debugging purpose
	 */

	nvme_lm_debug_ctrl_state(&state);

	dev_info(&dev->dev, "Get controller state successful\n");

out:
	kfree(fmt.ctrl_state_raw_buf);
	return ret;
}

static int nvmevf_cmd_set_ctrl_state(struct nvmevf_pci_core_device *nvmevf_dev,
				     struct nvmevf_migration_file *migf)
{
	struct pci_dev *dev = nvmevf_dev->core_device.pdev;
	struct nvme_command c = { };
	u32 sel = NVME_LM_SEND_SEL_SET_CTRL_STATE;
	/* assume that data buffer is big enough to hold state in one cmd */
	u32 mos = NVME_LM_SEQIND_ONLY;
	u32 cntlid = nvme_get_ctrl_id(dev);
	u32 csvi = NVME_LM_CSVI;
	u32 csuuidi = NVME_LM_CSUUIDI;
	int ret;

	c.lm.send.opcode = nvme_admin_lm_send;
	/* mos = SEQIND = 0b11 (Only) in MOS bits [17:16] */
	c.lm.send.cdw10 = cpu_to_le32((mos << 16) | sel);
	/*
	 * Assume that we are only working on NVMe state and not on vendor
	 * specific state.
	 */
	c.lm.send.cdw11 = cpu_to_le32(csuuidi << 24 | csvi << 16 | cntlid);

	/*
	 * Associates the Migration Send command with the correct migration
	 * session UUID currently we set to 0. For now asssume that initiaor
	 * and target has agreed on the UUIDX 0 for all the live migration
	 * sessions.
	 */
	c.lm.send.cdw14 = cpu_to_le32(0);
	/*
	 * Assume that data buffer is big enoough to hold the state,
	 * 0-based dword count.
	 */
	c.lm.send.cdw15 = cpu_to_le32(bytes_to_nvme_numd(migf->total_length));

	ret = nvme_submit_vf_cmd(dev, &c, NULL, migf->vf_data,
				 migf->total_length);
	if (ret) {
		dev_warn(&dev->dev,
			 "Load the device states failed (ret=0x%x)\n", ret);
		return ret;
	}

	dev_info(&dev->dev, "Set controller state successful\n");
	return 0;
}

static int nvmevf_release_file(struct inode *inode, struct file *filp)
{
	struct nvmevf_migration_file *migf = filp->private_data;

	nvmevf_disable_fd(migf);
	mutex_destroy(&migf->lock);
	kfree(migf);
	return 0;
}

static ssize_t nvmevf_resume_write(struct file *filp, const char __user *buf,
				   size_t len, loff_t *pos)
{
	struct nvmevf_migration_file *migf = filp->private_data;
	loff_t requested_length;
	ssize_t done = 0;
	int ret;

	if (pos)
		return -ESPIPE;
	pos = &filp->f_pos;

	if (*pos < 0 ||
	    check_add_overflow((loff_t)len, *pos, &requested_length))
		return -EINVAL;

	if (requested_length > MAX_MIGRATION_SIZE)
		return -ENOMEM;
	mutex_lock(&migf->lock);
	if (migf->disabled) {
		done = -ENODEV;
		goto out_unlock;
	}

	ret = copy_from_user(migf->vf_data + *pos, buf, len);
	if (ret) {
		done = -EFAULT;
		goto out_unlock;
	}
	*pos += len;
	done = len;
	migf->total_length += len;

out_unlock:
	mutex_unlock(&migf->lock);
	return done;
}

static const struct file_operations nvmevf_resume_fops = {
	.owner = THIS_MODULE,
	.write = nvmevf_resume_write,
	.release = nvmevf_release_file,
	.llseek = noop_llseek,
};

static struct nvmevf_migration_file *
nvmevf_pci_resume_device_data(struct nvmevf_pci_core_device *nvmevf_dev)
{
	struct nvmevf_migration_file *migf;
	int ret;

	migf = kzalloc(sizeof(*migf), GFP_KERNEL);
	if (!migf)
		return ERR_PTR(-ENOMEM);

	migf->filp = anon_inode_getfile("nvmevf_mig", &nvmevf_resume_fops, migf,
					O_WRONLY);
	if (IS_ERR(migf->filp)) {
		int err = PTR_ERR(migf->filp);

		kfree(migf);
		return ERR_PTR(err);
	}
	stream_open(migf->filp->f_inode, migf->filp);
	mutex_init(&migf->lock);

	/* Allocate buffer to load the device states and max states is 256K */
	migf->vf_data = kvzalloc(MAX_MIGRATION_SIZE, GFP_KERNEL);
	if (!migf->vf_data) {
		ret = -ENOMEM;
		goto out_free;
	}

	return migf;

out_free:
	fput(migf->filp);
	return ERR_PTR(ret);
}

static ssize_t nvmevf_save_read(struct file *filp, char __user *buf,
				size_t len, loff_t *pos)
{
	struct nvmevf_migration_file *migf = filp->private_data;
	ssize_t done = 0;
	int ret;

	if (pos)
		return -ESPIPE;
	pos = &filp->f_pos;

	mutex_lock(&migf->lock);
	if (*pos > migf->total_length) {
		done = -EINVAL;
		goto out_unlock;
	}

	if (migf->disabled) {
		done = -EINVAL;
		goto out_unlock;
	}

	len = min_t(size_t, migf->total_length - *pos, len);
	if (len) {
		ret = copy_to_user(buf, migf->vf_data + *pos, len);
		if (ret) {
			done = -EFAULT;
			goto out_unlock;
		}
		*pos += len;
		done = len;
	}

out_unlock:
	mutex_unlock(&migf->lock);
	return done;
}

static const struct file_operations nvmevf_save_fops = {
	.owner = THIS_MODULE,
	.read = nvmevf_save_read,
	.release = nvmevf_release_file,
	.llseek = noop_llseek,
};

static struct nvmevf_migration_file *
nvmevf_pci_save_device_data(struct nvmevf_pci_core_device *nvmevf_dev)
{
	struct nvmevf_migration_file *migf;
	int ret;

	migf = kzalloc(sizeof(*migf), GFP_KERNEL);
	if (!migf)
		return ERR_PTR(-ENOMEM);

	migf->filp = anon_inode_getfile("nvmevf_mig", &nvmevf_save_fops, migf,
					O_RDONLY);
	if (IS_ERR(migf->filp)) {
		int err = PTR_ERR(migf->filp);

		kfree(migf);
		return ERR_PTR(err);
	}

	stream_open(migf->filp->f_inode, migf->filp);
	mutex_init(&migf->lock);

	ret = nvmevf_cmd_get_ctrl_state(nvmevf_dev, migf);
	if (ret)
		goto out_free;

	return migf;
out_free:
	fput(migf->filp);
	return ERR_PTR(ret);
}

static struct file *
nvmevf_pci_step_device_state_locked(struct nvmevf_pci_core_device *nvmevf_dev,
				    u32 new)
{
	u32 cur = nvmevf_dev->mig_state;
	int ret;

	if (cur == VFIO_DEVICE_STATE_RUNNING && new == VFIO_DEVICE_STATE_STOP) {
		ret = nvmevf_cmd_suspend_device(nvmevf_dev);
		if (ret)
			return ERR_PTR(ret);
		return NULL;
	}

	if (cur == VFIO_DEVICE_STATE_STOP &&
	    new == VFIO_DEVICE_STATE_STOP_COPY) {
		struct nvmevf_migration_file *migf;

		migf = nvmevf_pci_save_device_data(nvmevf_dev);
		if (IS_ERR(migf))
			return ERR_CAST(migf);
		get_file(migf->filp);
		nvmevf_dev->saving_migf = migf;
		return migf->filp;
	}


	if (cur == VFIO_DEVICE_STATE_STOP_COPY &&
	    new == VFIO_DEVICE_STATE_STOP) {
		nvmevf_disable_fds(nvmevf_dev);
		return NULL;
	}

	if (cur == VFIO_DEVICE_STATE_STOP &&
	    new == VFIO_DEVICE_STATE_RESUMING) {
		struct nvmevf_migration_file *migf;

		migf = nvmevf_pci_resume_device_data(nvmevf_dev);
		if (IS_ERR(migf))
			return ERR_CAST(migf);
		get_file(migf->filp);
		nvmevf_dev->resuming_migf = migf;
		return migf->filp;
	}

	if (cur == VFIO_DEVICE_STATE_RESUMING &&
	    new == VFIO_DEVICE_STATE_STOP) {
		ret = nvmevf_cmd_set_ctrl_state(nvmevf_dev,
						nvmevf_dev->resuming_migf);
		if (ret)
			return ERR_PTR(ret);
		nvmevf_disable_fds(nvmevf_dev);
		return NULL;
	}

	if (cur == VFIO_DEVICE_STATE_STOP &&
	    new == VFIO_DEVICE_STATE_RUNNING) {
		nvmevf_cmd_resume_device(nvmevf_dev);
		return NULL;
	}

	/* vfio_mig_get_next_state() does not use arcs other than the above */
	WARN_ON(true);
	return ERR_PTR(-EINVAL);
}

static struct file *
nvmevf_pci_set_device_state(struct vfio_device *vdev,
			    enum vfio_device_mig_state new_state)
{
	struct nvmevf_pci_core_device *nvmevf_dev = container_of(vdev,
			struct nvmevf_pci_core_device, core_device.vdev);
	enum vfio_device_mig_state next_state;
	struct file *res = NULL;
	int ret;

	mutex_lock(&nvmevf_dev->state_mutex);
	while (new_state != nvmevf_dev->mig_state) {
		ret = vfio_mig_get_next_state(vdev, nvmevf_dev->mig_state,
					      new_state, &next_state);
		if (ret) {
			res = ERR_PTR(-EINVAL);
			break;
		}

		res = nvmevf_pci_step_device_state_locked(nvmevf_dev,
							  next_state);
		if (IS_ERR(res))
			break;
		nvmevf_dev->mig_state = next_state;
		if (WARN_ON(res && new_state != nvmevf_dev->mig_state)) {
			fput(res);
			res = ERR_PTR(-EINVAL);
			break;
		}
	}
	nvmevf_state_mutex_unlock(nvmevf_dev);
	return res;
}

static int nvmevf_pci_get_device_state(struct vfio_device *vdev,
				       enum vfio_device_mig_state *curr_state)
{
	struct nvmevf_pci_core_device *nvmevf_dev = container_of(
			vdev, struct nvmevf_pci_core_device, core_device.vdev);

	mutex_lock(&nvmevf_dev->state_mutex);
	*curr_state = nvmevf_dev->mig_state;
	nvmevf_state_mutex_unlock(nvmevf_dev);
	return 0;
}

static const struct vfio_migration_ops nvmevf_pci_mig_ops = {
	.migration_set_state = nvmevf_pci_set_device_state,
	.migration_get_state = nvmevf_pci_get_device_state,
};

static bool nvmevf_migration_supp(struct pci_dev *pdev)
{
	struct nvme_command c = { };
	u8 lm_supported = false;
	struct nvme_id_ctrl *id;
	__u16 oacs;
	int ret;

	c.identify.opcode = nvme_admin_identify;
	c.identify.cns = NVME_ID_CNS_CTRL;

	id = kmalloc(sizeof(struct nvme_id_ctrl), GFP_KERNEL);
	if (!id)
		return false;

	ret = nvme_submit_vf_cmd(pdev, &c, NULL, id,
				 sizeof(struct nvme_id_ctrl));
	if (ret) {
		dev_warn(&pdev->dev, "Get identify ctrl failed (ret=0x%x)\n",
			 ret);
		lm_supported = false;
		goto out;
	}

	oacs = le16_to_cpu(id->oacs);
	lm_supported = oacs & NVME_CTRL_OACS_HMLMS ? true : false;
out:
	kfree(id);
	return lm_supported;
}

static int nvmevf_migration_init_dev(struct vfio_device *core_vdev)
{
	struct nvmevf_pci_core_device *nvmevf_dev;
	struct pci_dev *pdev;
	int vf_id;
	int ret = -1;

	nvmevf_dev = container_of(core_vdev, struct nvmevf_pci_core_device,
				  core_device.vdev);
	pdev = to_pci_dev(core_vdev->dev);

	if (!pdev->is_virtfn)
		return ret;

	/*
	 * Get the identify controller data structure to check the live
	 * migration support.
	 */
	if (!nvmevf_migration_supp(pdev))
		return ret;

	nvmevf_dev->migrate_cap = 1;

	vf_id = pci_iov_vf_id(pdev);
	if (vf_id < 0)
		return ret;
	nvmevf_dev->vf_id = vf_id + 1;
	core_vdev->migration_flags = VFIO_MIGRATION_STOP_COPY;

	mutex_init(&nvmevf_dev->state_mutex);
	spin_lock_init(&nvmevf_dev->reset_lock);
	core_vdev->mig_ops = &nvmevf_pci_mig_ops;

	return vfio_pci_core_init_dev(core_vdev);
}

static int nvmevf_pci_open_device(struct vfio_device *core_vdev)
{
	struct nvmevf_pci_core_device *nvmevf_dev;
	struct vfio_pci_core_device *vdev;
	int ret;

	nvmevf_dev = container_of(core_vdev, struct nvmevf_pci_core_device,
			core_device.vdev);
	vdev = &nvmevf_dev->core_device;

	ret = vfio_pci_core_enable(vdev);
	if (ret)
		return ret;

	if (nvmevf_dev->migrate_cap)
		nvmevf_dev->mig_state = VFIO_DEVICE_STATE_RUNNING;
	vfio_pci_core_finish_enable(vdev);
	return 0;
}

static void nvmevf_pci_close_device(struct vfio_device *core_vdev)
{
	struct nvmevf_pci_core_device *nvmevf_dev;

	nvmevf_dev = container_of(core_vdev, struct nvmevf_pci_core_device,
			core_device.vdev);

	if (nvmevf_dev->migrate_cap) {
		mutex_lock(&nvmevf_dev->state_mutex);
		nvmevf_disable_fds(nvmevf_dev);
		nvmevf_state_mutex_unlock(nvmevf_dev);
	}

	vfio_pci_core_close_device(core_vdev);
}

static const struct vfio_device_ops nvmevf_pci_ops = {
	.name = "nvme-vfio-pci",
	.init = nvmevf_migration_init_dev,
	.release = vfio_pci_core_release_dev,
	.open_device = nvmevf_pci_open_device,
	.close_device = nvmevf_pci_close_device,
	.ioctl = vfio_pci_core_ioctl,
	.device_feature = vfio_pci_core_ioctl_feature,
	.read = vfio_pci_core_read,
	.write = vfio_pci_core_write,
	.mmap = vfio_pci_core_mmap,
	.request = vfio_pci_core_request,
	.match = vfio_pci_core_match,
};

static int nvmevf_pci_probe(struct pci_dev *pdev,
			    const struct pci_device_id *id)
{
	struct nvmevf_pci_core_device *nvmevf_dev;
	int ret;

	nvmevf_dev = vfio_alloc_device(nvmevf_pci_core_device, core_device.vdev,
				       &pdev->dev, &nvmevf_pci_ops);
	if (IS_ERR(nvmevf_dev))
		return PTR_ERR(nvmevf_dev);

	dev_set_drvdata(&pdev->dev, &nvmevf_dev->core_device);
	ret = vfio_pci_core_register_device(&nvmevf_dev->core_device);
	if (ret)
		goto out_put_dev;

	return 0;

out_put_dev:
	vfio_put_device(&nvmevf_dev->core_device.vdev);
	return ret;
}

static void nvmevf_pci_remove(struct pci_dev *pdev)
{
	struct nvmevf_pci_core_device *nvmevf_dev = nvmevf_drvdata(pdev);

	vfio_pci_core_unregister_device(&nvmevf_dev->core_device);
	vfio_put_device(&nvmevf_dev->core_device.vdev);
}

static void nvmevf_pci_aer_reset_done(struct pci_dev *pdev)
{
	struct nvmevf_pci_core_device *nvmevf_dev = nvmevf_drvdata(pdev);

	if (!nvmevf_dev->migrate_cap)
		return;

	/*
	 * As the higher VFIO layers are holding locks across reset and using
	 * those same locks with the mm_lock we need to prevent ABBA deadlock
	 * with the state_mutex and mm_lock.
	 * In case the state_mutex was taken already we defer the cleanup work
	 * to the unlock flow of the other running context.
	 */
	spin_lock(&nvmevf_dev->reset_lock);
	nvmevf_dev->deferred_reset = true;
	if (!mutex_trylock(&nvmevf_dev->state_mutex)) {
		spin_unlock(&nvmevf_dev->reset_lock);
		return;
	}
	spin_unlock(&nvmevf_dev->reset_lock);
	nvmevf_state_mutex_unlock(nvmevf_dev);
}

static const struct pci_error_handlers nvmevf_err_handlers = {
	.reset_done = nvmevf_pci_aer_reset_done,
	.error_detected = vfio_pci_core_aer_err_detected,
};

static struct pci_driver nvmevf_pci_driver = {
	.name = KBUILD_MODNAME,
	.probe = nvmevf_pci_probe,
	.remove = nvmevf_pci_remove,
	.err_handler = &nvmevf_err_handlers,
	.driver_managed_dma = true,
};

module_pci_driver(nvmevf_pci_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chaitanya Kulkarni <kch@nvidia.com>");
MODULE_AUTHOR("Lei Rao <lei.rao@intel.com>");
MODULE_DESCRIPTION("NVMe VFIO PCI - VFIO PCI driver with live migration support for NVMe");
