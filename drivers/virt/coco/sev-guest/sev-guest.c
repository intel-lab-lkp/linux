// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD Secure Encrypted Virtualization (SEV) guest driver interface
 *
 * Copyright (C) 2021 Advanced Micro Devices, Inc.
 *
 * Author: Brijesh Singh <brijesh.singh@amd.com>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/set_memory.h>
#include <linux/fs.h>
#include <linux/tsm.h>
#include <crypto/gcm.h>
#include <linux/psp-sev.h>
#include <linux/sockptr.h>
#include <linux/cleanup.h>
#include <linux/uuid.h>
#include <uapi/linux/sev-guest.h>
#include <uapi/linux/psp-sev.h>

#include <asm/svm.h>
#include <asm/sev.h>
#include <asm/sev-guest.h>

#define DEVICE_NAME	"sev-guest"

static u32 vmpck_id;
module_param(vmpck_id, uint, 0444);
MODULE_PARM_DESC(vmpck_id, "The VMPCK ID to use when communicating with the PSP.");

static inline struct snp_guest_dev *to_snp_dev(struct file *file)
{
	struct miscdevice *dev = file->private_data;

	return container_of(dev, struct snp_guest_dev, misc);
}

struct snp_req_resp {
	sockptr_t req_data;
	sockptr_t resp_data;
};

static int get_report(struct snp_guest_dev *snp_dev, struct snp_guest_request_ioctl *arg)
{
	struct snp_report_req *req = &snp_dev->req.report;
	struct snp_guest_req guest_req = {0};
	struct snp_report_resp *resp;
	int rc, resp_len;

	lockdep_assert_held(&snp_dev->cmd_mutex);

	if (!arg->req_data || !arg->resp_data)
		return -EINVAL;

	if (copy_from_user(req, (void __user *)arg->req_data, sizeof(*req)))
		return -EFAULT;

	/*
	 * The intermediate response buffer is used while decrypting the
	 * response payload. Make sure that it has enough space to cover the
	 * authtag.
	 */
	resp_len = sizeof(resp->data) + snp_dev->ctx->authsize;
	resp = kzalloc(resp_len, GFP_KERNEL_ACCOUNT);
	if (!resp)
		return -ENOMEM;

	guest_req.msg_version = arg->msg_version;
	guest_req.msg_type = SNP_MSG_REPORT_REQ;
	guest_req.vmpck_id = vmpck_id;
	guest_req.req_buf = req;
	guest_req.req_sz = sizeof(*req);
	guest_req.resp_buf = resp->data;
	guest_req.resp_sz = resp_len;
	guest_req.exit_code = SVM_VMGEXIT_GUEST_REQUEST;

	rc = snp_send_guest_request(snp_dev, &guest_req, arg);
	if (rc)
		goto e_free;

	if (copy_to_user((void __user *)arg->resp_data, resp, sizeof(*resp)))
		rc = -EFAULT;

e_free:
	kfree(resp);
	return rc;
}

static int get_derived_key(struct snp_guest_dev *snp_dev, struct snp_guest_request_ioctl *arg)
{
	struct snp_derived_key_req *req = &snp_dev->req.derived_key;
	struct snp_derived_key_resp resp = {0};
	struct snp_guest_req guest_req = {0};
	int rc, resp_len;
	/* Response data is 64 bytes and max authsize for GCM is 16 bytes. */
	u8 buf[64 + 16];

	lockdep_assert_held(&snp_dev->cmd_mutex);

	if (!arg->req_data || !arg->resp_data)
		return -EINVAL;

	/*
	 * The intermediate response buffer is used while decrypting the
	 * response payload. Make sure that it has enough space to cover the
	 * authtag.
	 */
	resp_len = sizeof(resp.data) + snp_dev->ctx->authsize;
	if (sizeof(buf) < resp_len)
		return -ENOMEM;

	if (copy_from_user(req, (void __user *)arg->req_data, sizeof(*req)))
		return -EFAULT;

	guest_req.msg_version = arg->msg_version;
	guest_req.msg_type = SNP_MSG_KEY_REQ;
	guest_req.vmpck_id = vmpck_id;
	guest_req.req_buf = req;
	guest_req.req_sz = sizeof(*req);
	guest_req.resp_buf = buf;
	guest_req.resp_sz = resp_len;
	guest_req.exit_code = SVM_VMGEXIT_GUEST_REQUEST;

	rc = snp_send_guest_request(snp_dev, &guest_req, arg);
	if (rc)
		return rc;

	memcpy(resp.data, buf, sizeof(resp.data));
	if (copy_to_user((void __user *)arg->resp_data, &resp, sizeof(resp)))
		rc = -EFAULT;

	/* The response buffer contains the sensitive data, explicitly clear it. */
	memzero_explicit(buf, sizeof(buf));
	memzero_explicit(&resp, sizeof(resp));
	return rc;
}

static int get_ext_report(struct snp_guest_dev *snp_dev, struct snp_guest_request_ioctl *arg,
			  struct snp_req_resp *io)

{
	struct snp_ext_report_req *req = &snp_dev->req.ext_report;
	struct snp_guest_req guest_req = {0};
	struct snp_report_resp *resp;
	sockptr_t certs_address;
	int ret, resp_len;

	lockdep_assert_held(&snp_dev->cmd_mutex);

	if (sockptr_is_null(io->req_data) || sockptr_is_null(io->resp_data))
		return -EINVAL;

	if (copy_from_sockptr(req, io->req_data, sizeof(*req)))
		return -EFAULT;

	/* caller does not want certificate data */
	if (!req->certs_len || !req->certs_address)
		goto cmd;

	if (req->certs_len > SEV_FW_BLOB_MAX_SIZE ||
	    !IS_ALIGNED(req->certs_len, PAGE_SIZE))
		return -EINVAL;

	if (sockptr_is_kernel(io->resp_data)) {
		certs_address = KERNEL_SOCKPTR((void *)req->certs_address);
	} else {
		certs_address = USER_SOCKPTR((void __user *)req->certs_address);
		if (!access_ok(certs_address.user, req->certs_len))
			return -EFAULT;
	}

	/*
	 * Initialize the intermediate buffer with all zeros. This buffer
	 * is used in the guest request message to get the certs blob from
	 * the host. If host does not supply any certs in it, then copy
	 * zeros to indicate that certificate data was not provided.
	 */
	memset(snp_dev->certs_data, 0, req->certs_len);
	guest_req.data_npages = req->certs_len >> PAGE_SHIFT;
cmd:
	/*
	 * The intermediate response buffer is used while decrypting the
	 * response payload. Make sure that it has enough space to cover the
	 * authtag.
	 */
	resp_len = sizeof(resp->data) + snp_dev->ctx->authsize;
	resp = kzalloc(resp_len, GFP_KERNEL_ACCOUNT);
	if (!resp)
		return -ENOMEM;

	guest_req.msg_version = arg->msg_version;
	guest_req.msg_type = SNP_MSG_REPORT_REQ;
	guest_req.vmpck_id = vmpck_id;
	guest_req.req_buf = &req->data;
	guest_req.req_sz = sizeof(req->data);
	guest_req.resp_buf = resp->data;
	guest_req.resp_sz = resp_len;
	guest_req.exit_code = SVM_VMGEXIT_EXT_GUEST_REQUEST;
	guest_req.data = snp_dev->certs_data;

	ret = snp_send_guest_request(snp_dev, &guest_req, arg);

	/* If certs length is invalid then copy the returned length */
	if (arg->vmm_error == SNP_GUEST_VMM_ERR_INVALID_LEN) {
		req->certs_len = guest_req.data_npages << PAGE_SHIFT;

		if (copy_to_sockptr(io->req_data, req, sizeof(*req)))
			ret = -EFAULT;
	}

	if (ret)
		goto e_free;

	if (guest_req.data_npages && req->certs_len &&
	    copy_to_sockptr(certs_address, snp_dev->certs_data, req->certs_len)) {
		ret = -EFAULT;
		goto e_free;
	}

	if (copy_to_sockptr(io->resp_data, resp, sizeof(*resp)))
		ret = -EFAULT;

e_free:
	kfree(resp);
	return ret;
}

static long snp_guest_ioctl(struct file *file, unsigned int ioctl, unsigned long arg)
{
	struct snp_guest_dev *snp_dev = to_snp_dev(file);
	void __user *argp = (void __user *)arg;
	struct snp_guest_request_ioctl input;
	struct snp_req_resp io;
	int ret = -ENOTTY;

	if (copy_from_user(&input, argp, sizeof(input)))
		return -EFAULT;

	input.exitinfo2 = 0xff;

	/* Message version must be non-zero */
	if (!input.msg_version)
		return -EINVAL;

	mutex_lock(&snp_dev->cmd_mutex);

	/* Check if the VMPCK is not empty */
	if (snp_is_vmpck_empty(snp_dev->vmpck_id)) {
		dev_err_ratelimited(snp_dev->dev, "VMPCK is disabled\n");
		mutex_unlock(&snp_dev->cmd_mutex);
		return -ENOTTY;
	}

	switch (ioctl) {
	case SNP_GET_REPORT:
		ret = get_report(snp_dev, &input);
		break;
	case SNP_GET_DERIVED_KEY:
		ret = get_derived_key(snp_dev, &input);
		break;
	case SNP_GET_EXT_REPORT:
		/*
		 * As get_ext_report() may be called from the ioctl() path and a
		 * kernel internal path (configfs-tsm), decorate the passed
		 * buffers as user pointers.
		 */
		io.req_data = USER_SOCKPTR((void __user *)input.req_data);
		io.resp_data = USER_SOCKPTR((void __user *)input.resp_data);
		ret = get_ext_report(snp_dev, &input, &io);
		break;
	default:
		break;
	}

	mutex_unlock(&snp_dev->cmd_mutex);

	if (input.exitinfo2 && copy_to_user(argp, &input, sizeof(input)))
		return -EFAULT;

	return ret;
}

static const struct file_operations snp_guest_fops = {
	.owner	= THIS_MODULE,
	.unlocked_ioctl = snp_guest_ioctl,
};

struct snp_msg_report_resp_hdr {
	u32 status;
	u32 report_size;
	u8 rsvd[24];
};

struct snp_msg_cert_entry {
	guid_t guid;
	u32 offset;
	u32 length;
};

static int sev_report_new(struct tsm_report *report, void *data)
{
	struct snp_msg_cert_entry *cert_table;
	struct tsm_desc *desc = &report->desc;
	struct snp_guest_dev *snp_dev = data;
	struct snp_msg_report_resp_hdr hdr;
	const u32 report_size = SZ_4K;
	const u32 ext_size = SEV_FW_BLOB_MAX_SIZE;
	u32 certs_size, i, size = report_size + ext_size;
	int ret;

	if (desc->inblob_len != SNP_REPORT_USER_DATA_SIZE)
		return -EINVAL;

	void *buf __free(kvfree) = kvzalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	guard(mutex)(&snp_dev->cmd_mutex);

	/* Check if the VMPCK is not empty */
	if (snp_is_vmpck_empty(snp_dev->vmpck_id)) {
		dev_err_ratelimited(snp_dev->dev, "VMPCK is disabled\n");
		return -ENOTTY;
	}

	cert_table = buf + report_size;
	struct snp_ext_report_req ext_req = {
		.data = { .vmpl = desc->privlevel },
		.certs_address = (__u64)cert_table,
		.certs_len = ext_size,
	};
	memcpy(&ext_req.data.user_data, desc->inblob, desc->inblob_len);

	struct snp_guest_request_ioctl input = {
		.msg_version = 1,
		.req_data = (__u64)&ext_req,
		.resp_data = (__u64)buf,
		.exitinfo2 = 0xff,
	};
	struct snp_req_resp io = {
		.req_data = KERNEL_SOCKPTR(&ext_req),
		.resp_data = KERNEL_SOCKPTR(buf),
	};

	ret = get_ext_report(snp_dev, &input, &io);
	if (ret)
		return ret;

	memcpy(&hdr, buf, sizeof(hdr));
	if (hdr.status == SEV_RET_INVALID_PARAM)
		return -EINVAL;
	if (hdr.status == SEV_RET_INVALID_KEY)
		return -EINVAL;
	if (hdr.status)
		return -ENXIO;
	if ((hdr.report_size + sizeof(hdr)) > report_size)
		return -ENOMEM;

	void *rbuf __free(kvfree) = kvzalloc(hdr.report_size, GFP_KERNEL);
	if (!rbuf)
		return -ENOMEM;

	memcpy(rbuf, buf + sizeof(hdr), hdr.report_size);
	report->outblob = no_free_ptr(rbuf);
	report->outblob_len = hdr.report_size;

	certs_size = 0;
	for (i = 0; i < ext_size / sizeof(struct snp_msg_cert_entry); i++) {
		struct snp_msg_cert_entry *ent = &cert_table[i];

		if (guid_is_null(&ent->guid) && !ent->offset && !ent->length)
			break;
		certs_size = max(certs_size, ent->offset + ent->length);
	}

	/* Suspicious that the response populated entries without populating size */
	if (!certs_size && i)
		dev_warn_ratelimited(snp_dev->dev, "certificate slots conveyed without size\n");

	/* No certs to report */
	if (!certs_size)
		return 0;

	/* Suspicious that the certificate blob size contract was violated
	 */
	if (certs_size > ext_size) {
		dev_warn_ratelimited(snp_dev->dev, "certificate data truncated\n");
		certs_size = ext_size;
	}

	void *cbuf __free(kvfree) = kvzalloc(certs_size, GFP_KERNEL);
	if (!cbuf)
		return -ENOMEM;

	memcpy(cbuf, cert_table, certs_size);
	report->auxblob = no_free_ptr(cbuf);
	report->auxblob_len = certs_size;

	return 0;
}

static const struct tsm_ops sev_tsm_ops = {
	.name = KBUILD_MODNAME,
	.report_new = sev_report_new,
};

static void unregister_sev_tsm(void *data)
{
	tsm_unregister(&sev_tsm_ops);
}

static int __init sev_guest_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct snp_guest_dev *snp_dev;
	struct miscdevice *misc;
	int ret;

	if (!cc_platform_has(CC_ATTR_GUEST_SEV_SNP))
		return -ENODEV;

	snp_dev = devm_kzalloc(&pdev->dev, sizeof(struct snp_guest_dev), GFP_KERNEL);
	if (!snp_dev)
		return -ENOMEM;

	if (!snp_assign_vmpck(snp_dev, vmpck_id)) {
		dev_err(dev, "invalid vmpck id %u\n", vmpck_id);
		ret = -EINVAL;
		goto e_free_snpdev;
	}

	if (snp_setup_psp_messaging(snp_dev)) {
		dev_err(dev, "Unable to setup PSP messaging vmpck id %u\n", snp_dev->vmpck_id);
		ret = -ENODEV;
		goto e_free_snpdev;
	}

	mutex_init(&snp_dev->cmd_mutex);
	platform_set_drvdata(pdev, snp_dev);
	snp_dev->dev = dev;

	snp_dev->certs_data = alloc_shared_pages(SEV_FW_BLOB_MAX_SIZE);
	if (!snp_dev->certs_data)
		goto e_free_ctx;

	misc = &snp_dev->misc;
	misc->minor = MISC_DYNAMIC_MINOR;
	misc->name = DEVICE_NAME;
	misc->fops = &snp_guest_fops;

	ret = tsm_register(&sev_tsm_ops, snp_dev, &tsm_report_extra_type);
	if (ret)
		goto e_free_cert_data;

	ret = devm_add_action_or_reset(&pdev->dev, unregister_sev_tsm, NULL);
	if (ret)
		goto e_free_cert_data;

	ret =  misc_register(misc);
	if (ret)
		goto e_free_cert_data;

	dev_info(dev, "Initialized SEV guest driver (using vmpck_id %u)\n", snp_dev->vmpck_id);

	return 0;

e_free_cert_data:
	free_shared_pages(snp_dev->certs_data, SEV_FW_BLOB_MAX_SIZE);
e_free_ctx:
	kfree(snp_dev->ctx);
e_free_snpdev:
	kfree(snp_dev);
	return ret;
}

static int __exit sev_guest_remove(struct platform_device *pdev)
{
	struct snp_guest_dev *snp_dev = platform_get_drvdata(pdev);

	free_shared_pages(snp_dev->certs_data, SEV_FW_BLOB_MAX_SIZE);
	misc_deregister(&snp_dev->misc);
	kfree(snp_dev->ctx);
	kfree(snp_dev);

	return 0;
}

/*
 * This driver is meant to be a common SEV guest interface driver and to
 * support any SEV guest API. As such, even though it has been introduced
 * with the SEV-SNP support, it is named "sev-guest".
 */
static struct platform_driver sev_guest_driver = {
	.remove		= __exit_p(sev_guest_remove),
	.driver		= {
		.name = "sev-guest",
	},
};

module_platform_driver_probe(sev_guest_driver, sev_guest_probe);

MODULE_AUTHOR("Brijesh Singh <brijesh.singh@amd.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");
MODULE_DESCRIPTION("AMD SEV Guest Driver");
MODULE_ALIAS("platform:sev-guest");
