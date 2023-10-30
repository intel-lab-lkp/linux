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
#include <crypto/gcm.h>
#include <linux/psp-sev.h>
#include <uapi/linux/sev-guest.h>
#include <uapi/linux/psp-sev.h>

#include <asm/svm.h>
#include <asm/sev.h>
#include <asm/sev-guest.h>

#define DEVICE_NAME	"sev-guest"

static u32 vmpck_id;
module_param(vmpck_id, uint, 0444);
MODULE_PARM_DESC(vmpck_id, "The VMPCK ID to use when communicating with the PSP.");

/* Mutex to serialize the shared buffer access and command handling. */
static DEFINE_MUTEX(snp_cmd_mutex);

static inline struct snp_guest_dev *to_snp_dev(struct file *file)
{
	struct miscdevice *dev = file->private_data;

	return container_of(dev, struct snp_guest_dev, misc);
}

static int get_report(struct snp_guest_dev *snp_dev, struct snp_guest_request_ioctl *arg)
{
	struct snp_guest_req guest_req = {0};
	struct snp_report_resp *resp;
	struct snp_report_req req;
	int rc, resp_len;

	lockdep_assert_held(&snp_dev->cmd_mutex);

	if (!arg->req_data || !arg->resp_data)
		return -EINVAL;

	if (copy_from_user(&req, (void __user *)arg->req_data, sizeof(req)))
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
	guest_req.req_buf = &req;
	guest_req.req_sz = sizeof(req);
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
	struct snp_derived_key_resp resp = {0};
	struct snp_guest_req guest_req = {0};
	struct snp_derived_key_req req;
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

	if (copy_from_user(&req, (void __user *)arg->req_data, sizeof(req)))
		return -EFAULT;

	guest_req.msg_version = arg->msg_version;
	guest_req.msg_type = SNP_MSG_KEY_REQ;
	guest_req.vmpck_id = vmpck_id;
	guest_req.req_buf = &req;
	guest_req.req_sz = sizeof(req);
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

static int get_ext_report(struct snp_guest_dev *snp_dev, struct snp_guest_request_ioctl *arg)
{
	struct snp_guest_req guest_req = {0};
	struct snp_ext_report_req req;
	struct snp_report_resp *resp;
	int ret, resp_len;
	size_t npages = 0;

	lockdep_assert_held(&snp_dev->cmd_mutex);

	if (!arg->req_data || !arg->resp_data)
		return -EINVAL;

	if (copy_from_user(&req, (void __user *)arg->req_data, sizeof(req)))
		return -EFAULT;

	/* userspace does not want certificate data */
	if (!req.certs_len || !req.certs_address)
		goto cmd;

	if (req.certs_len > SEV_FW_BLOB_MAX_SIZE ||
	    !IS_ALIGNED(req.certs_len, PAGE_SIZE))
		return -EINVAL;

	if (!access_ok((const void __user *)req.certs_address, req.certs_len))
		return -EFAULT;

	/*
	 * Initialize the intermediate buffer with all zeros. This buffer
	 * is used in the guest request message to get the certs blob from
	 * the host. If host does not supply any certs in it, then copy
	 * zeros to indicate that certificate data was not provided.
	 */
	memset(snp_dev->certs_data, 0, req.certs_len);
	npages = req.certs_len >> PAGE_SHIFT;
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
	guest_req.req_buf = &req.data;
	guest_req.req_sz = sizeof(req.data);
	guest_req.resp_buf = resp->data;
	guest_req.resp_sz = resp_len;
	guest_req.exit_code = SVM_VMGEXIT_EXT_GUEST_REQUEST;
	guest_req.data = snp_dev->certs_data;
	guest_req.data_npages = &npages;

	ret = snp_send_guest_request(snp_dev, &guest_req, arg);

	/* If certs length is invalid then copy the returned length */
	if (arg->vmm_error == SNP_GUEST_VMM_ERR_INVALID_LEN) {
		req.certs_len = npages << PAGE_SHIFT;

		if (copy_to_user((void __user *)arg->req_data, &req, sizeof(req)))
			ret = -EFAULT;
	}

	if (ret)
		goto e_free;

	if (npages && req.certs_len &&
	    copy_to_user((void __user *)req.certs_address, snp_dev->certs_data,
			 req.certs_len)) {
		ret = -EFAULT;
		goto e_free;
	}

	if (copy_to_user((void __user *)arg->resp_data, resp, sizeof(*resp)))
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
		ret = get_ext_report(snp_dev, &input);
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
		dev_err(dev, "invalid vmpck id %d\n", vmpck_id);
		ret = -EINVAL;
		goto e_free_snpdev;
	}

	if (snp_setup_psp_messaging(snp_dev)) {
		dev_err(dev, "Unable to setup PSP messaging vmpck id %d\n", snp_dev->vmpck_id);
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

	ret =  misc_register(misc);
	if (ret)
		goto e_free_cert_data;

	dev_info(dev, "Initialized SEV guest driver (using vmpck_id %d)\n", snp_dev->vmpck_id);
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
