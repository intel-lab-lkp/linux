// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "papr-vpd: " fmt

#include <linux/build_bug.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/lockdep.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/string_helpers.h>
#include <linux/uaccess.h>
#include <asm/machdep.h>
#include <asm/papr-vpd.h>
#include <asm/rtas-work-area.h>
#include <asm/rtas.h>
#include <uapi/asm/papr-vpd.h>
#include "papr-rtas-common.h"

/*
 * Function-specific return values for ibm,get-vpd, derived from PAPR+
 * v2.13 7.3.20 "ibm,get-vpd RTAS Call".
 */
#define RTAS_IBM_GET_VPD_COMPLETE    0 /* All VPD has been retrieved. */
#define RTAS_IBM_GET_VPD_MORE_DATA   1 /* More VPD is available. */
#define RTAS_IBM_GET_VPD_START_OVER -4 /* VPD changed, restart call sequence. */

/**
 * struct rtas_ibm_get_vpd_params - Parameters (in and out) for ibm,get-vpd.
 * @loc_code:  In: Caller-provided location code buffer. Must be RTAS-addressable.
 * @work_area: In: Caller-provided work area buffer for results.
 * @sequence:  In: Sequence number. Out: Next sequence number.
 * @written:   Out: Bytes written by ibm,get-vpd to @work_area.
 * @status:    Out: RTAS call status.
 */
struct rtas_ibm_get_vpd_params {
	const struct papr_location_code *loc_code;
	struct rtas_work_area *work_area;
	u32 sequence;
	u32 written;
	s32 status;
};

/**
 * rtas_ibm_get_vpd() - Call ibm,get-vpd to fill a work area buffer.
 * @params: See &struct rtas_ibm_get_vpd_params.
 *
 * Calls ibm,get-vpd until it errors or successfully deposits data
 * into the supplied work area. Handles RTAS retry statuses. Maps RTAS
 * error statuses to reasonable errno values.
 *
 * The caller is expected to invoke rtas_ibm_get_vpd() multiple times
 * to retrieve all the VPD for the provided location code. Only one
 * sequence should be in progress at any time; starting a new sequence
 * will disrupt any sequence already in progress. Serialization of VPD
 * retrieval sequences is the responsibility of the caller.
 *
 * The caller should inspect @params.status to determine whether more
 * calls are needed to complete the sequence.
 *
 * Context: May sleep.
 * Return: -ve on error, 0 otherwise.
 */
static int rtas_ibm_get_vpd(struct rtas_ibm_get_vpd_params *params)
{
	const struct papr_location_code *loc_code = params->loc_code;
	struct rtas_work_area *work_area = params->work_area;
	u32 rets[2];
	s32 fwrc;
	int ret;

	lockdep_assert_held(&rtas_ibm_get_vpd_lock);

	do {
		fwrc = rtas_call(rtas_function_token(RTAS_FN_IBM_GET_VPD), 4, 3,
				 rets,
				 __pa(loc_code),
				 rtas_work_area_phys(work_area),
				 rtas_work_area_size(work_area),
				 params->sequence);
	} while (rtas_busy_delay(fwrc));

	switch (fwrc) {
	case RTAS_HARDWARE_ERROR:
		ret = -EIO;
		break;
	case RTAS_INVALID_PARAMETER:
		ret = -EINVAL;
		break;
	case RTAS_IBM_GET_VPD_START_OVER:
		ret = -EAGAIN;
		pr_info_ratelimited("VPD changed during retrieval, retrying\n");
		break;
	case RTAS_IBM_GET_VPD_MORE_DATA:
		params->sequence = rets[0];
		fallthrough;
	case RTAS_IBM_GET_VPD_COMPLETE:
		params->written = rets[1];
		/*
		 * Kernel or firmware bug, do not continue.
		 */
		if (WARN(params->written > rtas_work_area_size(work_area),
			 "possible write beyond end of work area"))
			ret = -EFAULT;
		else
			ret = 0;
		break;
	default:
		ret = -EIO;
		pr_err_ratelimited("unexpected ibm,get-vpd status %d\n", fwrc);
		break;
	}

	params->status = fwrc;
	return ret;
}

/*
 * Internal VPD sequence APIs. A VPD sequence is a series of calls to
 * ibm,get-vpd for a given location code. The sequence ends when an
 * error is encountered or all VPD for the location code has been
 * returned.
 */

/**
 * vpd_sequence_begin() - Begin a VPD retrieval sequence.
 * @seq:   Uninitialized sequence state.
 * @param: Location code that defines the scope of the VPD to return.
 *
 * Initializes @seq with the resources necessary to carry out a VPD
 * sequence. Callers must pass @seq to vpd_sequence_end() regardless
 * of whether the sequence succeeds.
 *
 * Context: May sleep.
 */
static void vpd_sequence_begin(struct papr_rtas_sequence *seq, void *param)
{
	struct rtas_ibm_get_vpd_params *vpd_params;
	/*
	 * Use a static data structure for the location code passed to
	 * RTAS to ensure it's in the RMA and avoid a separate work
	 * area allocation. Guarded by the function lock.
	 */
	static struct papr_location_code static_loc_code;

	/*
	 * We could allocate the work area before acquiring the
	 * function lock, but that would allow concurrent requests to
	 * exhaust the limited work area pool for no benefit. So
	 * allocate the work area under the lock.
	 */
	mutex_lock(&rtas_ibm_get_vpd_lock);
	static_loc_code = *(struct papr_location_code *)param;
	vpd_params =  (struct rtas_ibm_get_vpd_params *)seq->params;
	vpd_params->work_area = rtas_work_area_alloc(SZ_4K);
	vpd_params->loc_code = &static_loc_code;
	vpd_params->sequence = 1;
	vpd_params->status = 0;
}

/**
 * vpd_sequence_end() - Finalize a VPD retrieval sequence.
 * @seq: Sequence state.
 *
 * Releases resources obtained by vpd_sequence_begin().
 */
static void vpd_sequence_end(struct papr_rtas_sequence *seq)
{
	struct rtas_ibm_get_vpd_params *vpd_params;

	vpd_params =  (struct rtas_ibm_get_vpd_params *)seq->params;
	rtas_work_area_free(vpd_params->work_area);
	mutex_unlock(&rtas_ibm_get_vpd_lock);
}

/**
 * vpd_sequence_should_stop() - Determine whether a VPD retrieval sequence
 *                              should continue.
 * @seq: VPD sequence state.
 *
 * Examines the sequence error state and outputs of the last call to
 * ibm,get-vpd to determine whether the sequence in progress should
 * continue or stop.
 *
 * Return: True if the sequence has encountered an error or if all VPD for
 *         this sequence has been retrieved. False otherwise.
 */
static bool vpd_sequence_should_stop(const struct papr_rtas_sequence *seq)
{
	struct rtas_ibm_get_vpd_params *vpd_params;
	bool done;

	if (seq->error)
		return true;

	vpd_params =  (struct rtas_ibm_get_vpd_params *)seq->params;

	switch (vpd_params->status) {
	case 0:
		if (vpd_params->written == 0)
			done = false; /* Initial state. */
		else
			done = true; /* All data consumed. */
		break;
	case 1:
		done = false; /* More data available. */
		break;
	default:
		done = true; /* Error encountered. */
		break;
	}

	return done;
}

/*
 * Generator function to be passed to papr_rtas_blob_generate().
 */
static const char *vpd_sequence_fill_work_area(struct papr_rtas_sequence *seq,
						size_t *len)
{
	struct rtas_ibm_get_vpd_params *p;

	p = (struct rtas_ibm_get_vpd_params *)seq->params;

	if (vpd_sequence_should_stop(seq))
		return NULL;
	if (papr_rtas_sequence_set_err(seq, rtas_ibm_get_vpd(p)))
		return NULL;
	*len = p->written;
	return rtas_work_area_raw_buf(p->work_area);
}

/*
 * Higher-level VPD retrieval code below. These functions use the
 * papr_rtas_blob_* and vpd_sequence_* APIs defined above to create fd-based
 * VPD handles for consumption by user space.
 */

static ssize_t papr_vpd_handle_read(struct file *file, char __user *buf, size_t size, loff_t *off)
{
	const struct papr_rtas_blob *blob = file->private_data;

	/* bug: we should not instantiate a handle without any data attached. */
	if (!papr_rtas_blob_has_data(blob)) {
		pr_err_once("handle without data\n");
		return -EIO;
	}

	return simple_read_from_buffer(buf, size, off, blob->data, blob->len);
}

static int papr_vpd_handle_release(struct inode *inode, struct file *file)
{
	const struct papr_rtas_blob *blob = file->private_data;

	papr_rtas_blob_free(blob);

	return 0;
}

static loff_t papr_vpd_handle_seek(struct file *file, loff_t off, int whence)
{
	const struct papr_rtas_blob *blob = file->private_data;

	return fixed_size_llseek(file, off, whence, blob->len);
}


static const struct file_operations papr_vpd_handle_ops = {
	.read = papr_vpd_handle_read,
	.llseek = papr_vpd_handle_seek,
	.release = papr_vpd_handle_release,
};

/**
 * papr_vpd_create_handle() - Create a fd-based handle for reading VPD.
 * @ulc: Location code in user memory; defines the scope of the VPD to
 *       retrieve.
 *
 * Handler for PAPR_VPD_IOC_CREATE_HANDLE ioctl command. Validates
 * @ulc and instantiates an immutable VPD "blob" for it. The blob is
 * attached to a file descriptor for reading by user space. The memory
 * backing the blob is freed when the file is released.
 *
 * The entire requested VPD is retrieved by this call and all
 * necessary RTAS interactions are performed before returning the fd
 * to user space. This keeps the read handler simple and ensures that
 * the kernel can prevent interleaving of ibm,get-vpd call sequences.
 *
 * Return: The installed fd number if successful, -ve errno otherwise.
 */
static long papr_vpd_create_handle(struct papr_location_code __user *ulc)
{
	struct rtas_ibm_get_vpd_params vpd_params = {};
	struct papr_rtas_sequence seq = {};
	struct papr_location_code klc;
	int fd;

	if (copy_from_user(&klc, ulc, sizeof(klc)))
		return -EFAULT;

	if (!string_is_terminated(klc.str, ARRAY_SIZE(klc.str)))
		return -EINVAL;

	seq = (struct papr_rtas_sequence) {
		.begin = vpd_sequence_begin,
		.end = vpd_sequence_end,
		.work = vpd_sequence_fill_work_area,
	};

	seq.params = (void *)&vpd_params;

	fd = papr_rtas_setup_file_interface(&seq, &klc,
			&papr_vpd_handle_ops, "[papr-vpd]");

	return fd;
}

/*
 * Top-level ioctl handler for /dev/papr-vpd.
 */
static long papr_vpd_dev_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg)
{
	void __user *argp = (__force void __user *)arg;
	long ret;

	switch (ioctl) {
	case PAPR_VPD_IOC_CREATE_HANDLE:
		ret = papr_vpd_create_handle(argp);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}
	return ret;
}

static const struct file_operations papr_vpd_ops = {
	.unlocked_ioctl = papr_vpd_dev_ioctl,
};

static struct miscdevice papr_vpd_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "papr-vpd",
	.fops = &papr_vpd_ops,
};

static __init int papr_vpd_init(void)
{
	if (!rtas_function_implemented(RTAS_FN_IBM_GET_VPD))
		return -ENODEV;

	return misc_register(&papr_vpd_dev);
}
machine_device_initcall(pseries, papr_vpd_init);
