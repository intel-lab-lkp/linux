// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Arm Ltd.
 *
 * Maintained by: <tpmdd-devel@lists.sourceforge.net>
 *
 * This device driver implements the TPM CRB start method
 * as defined in the TPM Service Command Response Buffer
 * Interface Over FF-A (DEN0138).
 */

#define pr_fmt(fmt) "FFA_CRB: " fmt

#include <linux/arm_ffa.h>
#include "ffa_crb.h"

/* TPM service function status codes */
#define FFA_CRB_OK			0x05000001
#define FFA_CRB_OK_RESULTS_RETURNED	0x05000002
#define FFA_CRB_NOFUNC			0x8e000001
#define FFA_CRB_NOTSUP			0x8e000002
#define FFA_CRB_INVARG			0x8e000005
#define FFA_CRB_INV_CRB_CTRL_DATA	0x8e000006
#define FFA_CRB_ALREADY			0x8e000009
#define FFA_CRB_DENIED			0x8e00000a
#define FFA_CRB_NOMEM			0x8e00000b

#define FFA_CRB_VERSION_MAJOR	1
#define FFA_CRB_VERSION_MINOR	0

/* version encoding */
#define FFA_CRB_MAJOR_VERSION_MASK  GENMASK(30, 16)
#define FFA_CRB_MINOR_VERSION_MASK  GENMASK(15, 0)
#define FFA_CRB_MAJOR_VERSION(x)    ((u16)(FIELD_GET(FFA_CRB_MAJOR_VERSION_MASK, (x))))
#define FFA_CRB_MINOR_VERSION(x)    ((u16)(FIELD_GET(FFA_CRB_MINOR_VERSION_MASK, (x))))

/*
 * Normal world sends requests with FFA_MSG_SEND_DIRECT_REQ and
 * responses are returned with FFA_MSG_SEND_DIRECT_RESP for normal
 * messages.
 *
 * All requests with FFA_MSG_SEND_DIRECT_REQ and FFA_MSG_SEND_DIRECT_RESP
 * are using the AArch32 SMC calling convention with register usage as
 * defined in FF-A specification:
 * w0:    Function ID (0x8400006F or 0x84000070)
 * w1:    Source/Destination IDs
 * w2:    Reserved (MBZ)
 * w3-w7: Implementation defined, free to be used below
 */

/*
 * Returns the version of the interface that is available
 * Call register usage:
 * w3:    Not used (MBZ)
 * w4:    TPM service function ID, FFA_CRB_GET_INTERFACE_VERSION
 * w5-w7: Reserved (MBZ)
 *
 * Return register usage:
 * w3:    Not used (MBZ)
 * w4:    TPM service function status
 * w5:    TPM service interface version
 *        Bits[31:16]: major version
 *        Bits[15:0]: minor version
 * w6-w7: Reserved (MBZ)
 *
 * Possible function status codes in register w4:
 *     CRB_FFA_OK_RESULTS_RETURNED: The version of the interface has been
 *                                  returned.
 */
#define FFA_CRB_GET_INTERFACE_VERSION 0x0f000001

/*
 * Return information on a given feature of the TPM service
 * Call register usage:
 * w3:    Not used (MBZ)
 * w4:    TPM service function ID, FFA_CRB_START
 * w5:    Start function qualifier
 *            Bits[31:8] (MBZ)
 *            Bits[7:0]
 *              0: Notifies TPM that a command is ready to be processed
 *              1: Notifies TPM that a locality request is ready to be processed
 * w6:    TPM locality, one of 0..4
 *            -If the start function qualifier is 0, identifies the locality
 *             from where the command originated.
 *            -If the start function qualifier is 1, identifies the locality
 *             of the locality request
 * w6-w7: Reserved (MBZ)
 *
 * Return register usage:
 * w3:    Not used (MBZ)
 * w4:    TPM service function status
 * w5-w7: Reserved (MBZ)
 *
 * Possible function status codes in register w4:
 *     FFA_CRB_OK: the TPM service has been notified successfully
 *     FFA_CRB_INVARG: one or more arguments are not valid
 *     FFA_CRB_INV_CRB_CTRL_DATA: CRB control data or locality control
 *         data at the given TPM locality is not valid
 *     FFA_CRB_DENIED: the TPM has previously disabled locality requests and
 *         command processing at the given locality
 */
#define FFA_CRB_START 0x0f000201

struct ffa_crb {
	struct ffa_device *ffa_dev;
	u16 major_version;
	u16 minor_version;
	struct mutex msg_data_lock;
	struct ffa_send_direct_data direct_msg_data;
};

static struct ffa_crb *ffa_crb;

static int ffa_crb_to_linux_errno(int errno)
{
	int rc;

	switch (errno) {
	case FFA_CRB_OK:
		rc = 0;
		break;
	case FFA_CRB_OK_RESULTS_RETURNED:
		rc = 0;
		break;
	case FFA_CRB_NOFUNC:
		rc = -ENOENT;
		break;
	case FFA_CRB_NOTSUP:
		rc = -EPERM;
		break;
	case FFA_CRB_INVARG:
		rc = -EINVAL;
		break;
	case FFA_CRB_INV_CRB_CTRL_DATA:
		rc = -ENOEXEC;
		break;
	case FFA_CRB_ALREADY:
		rc = -EEXIST;
		break;
	case FFA_CRB_DENIED:
		rc = -EACCES;
		break;
	case FFA_CRB_NOMEM:
		rc = -ENOMEM;
		break;
	default:
		rc = -EINVAL;
	}

	return rc;
}

int ffa_crb_init(void)
{
	if (ffa_crb == NULL)
		return -ENOENT;

	if (IS_ERR_VALUE(ffa_crb))
		return -ENODEV;

	return 0;
}
EXPORT_SYMBOL_GPL(ffa_crb_init);

static int __ffa_crb_send_recieve(unsigned long func_id,
		unsigned long a0, unsigned long a1, unsigned long a2)
{
	int ret;
	const struct ffa_msg_ops *msg_ops;

	if (ffa_crb == NULL)
		return -ENOENT;

	msg_ops = ffa_crb->ffa_dev->ops->msg_ops;

	memset(&ffa_crb->direct_msg_data, 0x00,
			sizeof(struct ffa_send_direct_data));

	ffa_crb->direct_msg_data.data1 = func_id;
	ffa_crb->direct_msg_data.data2 = a0;
	ffa_crb->direct_msg_data.data3 = a1;
	ffa_crb->direct_msg_data.data4 = a2;

	ret = msg_ops->sync_send_receive(ffa_crb->ffa_dev,
			&ffa_crb->direct_msg_data);
	if (!ret)
		ret = ffa_crb_to_linux_errno(ffa_crb->direct_msg_data.data1);

	return ret;
}

int ffa_crb_get_interface_version(uint16_t *major, uint16_t *minor)
{
	int rc;

	if (ffa_crb == NULL)
		return -ENOENT;

	if (IS_ERR_VALUE(ffa_crb))
		return -ENODEV;

	if (major == NULL || minor == NULL)
		return -EINVAL;

	guard(mutex)(&ffa_crb->msg_data_lock);

	rc = __ffa_crb_send_recieve(FFA_CRB_GET_INTERFACE_VERSION, 0x00, 0x00, 0x00);
	if (!rc) {
		*major = FFA_CRB_MAJOR_VERSION(ffa_crb->direct_msg_data.data2);
		*minor = FFA_CRB_MINOR_VERSION(ffa_crb->direct_msg_data.data2);
	}

	return rc;
}
EXPORT_SYMBOL_GPL(ffa_crb_get_interface_version);

int ffa_crb_start(int request_type, int locality)
{
	if (ffa_crb == NULL)
		return -ENOENT;

	if (IS_ERR_VALUE(ffa_crb))
		return -ENODEV;

	guard(mutex)(&ffa_crb->msg_data_lock);

	return __ffa_crb_send_recieve(FFA_CRB_START, request_type, locality, 0x00);
}
EXPORT_SYMBOL_GPL(ffa_crb_start);

static int ffa_crb_probe(struct ffa_device *ffa_dev)
{
	int rc;
	struct ffa_crb *p;

	/* only one instance of a TPM partition is supported */
	if (ffa_crb && !IS_ERR_VALUE(ffa_crb))
		return -EEXIST;

	ffa_crb = ERR_PTR(-ENODEV); // set ffa_crb so we can detect probe failure

	if (!ffa_partition_supports_direct_recv(ffa_dev)) {
		pr_err("TPM partition doesn't support direct message receive.\n");
		return -EINVAL;
	}

	p = kzalloc(sizeof(*ffa_crb), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	ffa_crb = p;

	mutex_init(&ffa_crb->msg_data_lock);
	ffa_crb->ffa_dev = ffa_dev;
	ffa_dev_set_drvdata(ffa_dev, ffa_crb);

	/* if TPM is aarch32 use 32-bit SMCs */
	if (!ffa_partition_check_property(ffa_dev, FFA_PARTITION_AARCH64_EXEC))
		ffa_dev->ops->msg_ops->mode_32bit_set(ffa_dev);

	/* verify compatibility of TPM service version number */
	rc = ffa_crb_get_interface_version(&ffa_crb->major_version,
			&ffa_crb->minor_version);
	if (rc) {
		pr_err("failed to get crb interface version. rc:%d", rc);
		goto out;
	}

	pr_info("ABI version %u.%u", ffa_crb->major_version,
		ffa_crb->minor_version);

	if ((ffa_crb->major_version != FFA_CRB_VERSION_MAJOR) ||
	    (ffa_crb->minor_version < FFA_CRB_VERSION_MINOR)) {
		pr_err("Incompatible ABI version");
		goto out;
	}

	return 0;

out:
	kfree(ffa_crb);
	ffa_crb = ERR_PTR(-ENODEV);
	return -EINVAL;
}

static void ffa_crb_remove(struct ffa_device *ffa_dev)
{
	kfree(ffa_crb);
	ffa_crb = NULL;
}

static const struct ffa_device_id ffa_crb_device_id[] = {
	/* 17b862a4-1806-4faf-86b3-089a58353861 */
	{ UUID_INIT(0x17b862a4, 0x1806, 0x4faf,
		    0x86, 0xb3, 0x08, 0x9a, 0x58, 0x35, 0x38, 0x61) },
	{}
};

static struct ffa_driver ffa_crb_driver = {
	.name = "ffa-crb",
	.probe = ffa_crb_probe,
	.remove = ffa_crb_remove,
	.id_table = ffa_crb_device_id,
};

module_ffa_driver(ffa_crb_driver);

MODULE_AUTHOR("Arm");
MODULE_DESCRIPTION("FFA CRB driver");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");
