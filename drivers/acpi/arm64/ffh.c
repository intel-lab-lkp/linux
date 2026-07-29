// SPDX-License-Identifier: GPL-2.0-only
#include <linux/acpi.h>
#include <linux/arm-smccc.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/uuid.h>

/*
 * Implements ARM64 specific callbacks to support ACPI FFH Operation Region as
 * specified in https://developer.arm.com/docs/den0048/latest
 */
struct acpi_ffh_data {
	struct acpi_ffh_info info;
	void (*invoke_ffh_fn)(unsigned long a0, unsigned long a1,
			      unsigned long a2, unsigned long a3,
			      unsigned long a4, unsigned long a5,
			      unsigned long a6, unsigned long a7,
			      struct arm_smccc_res *args,
			      struct arm_smccc_quirk *res);
	void (*invoke_ffh64_fn)(const struct arm_smccc_1_2_regs *args,
				struct arm_smccc_1_2_regs *res);
};

int acpi_ffh_address_space_arch_setup(void *handler_ctxt, void **region_ctxt)
{
	enum arm_smccc_conduit conduit;
	struct acpi_ffh_data *ffh_ctxt;

	if (arm_smccc_get_version() < ARM_SMCCC_VERSION_1_2)
		return -EOPNOTSUPP;

	conduit = arm_smccc_1_1_get_conduit();
	if (conduit == SMCCC_CONDUIT_NONE) {
		pr_err("%s: invalid SMCCC conduit\n", __func__);
		return -EOPNOTSUPP;
	}

	ffh_ctxt = kzalloc_obj(*ffh_ctxt);
	if (!ffh_ctxt)
		return -ENOMEM;

	if (conduit == SMCCC_CONDUIT_SMC) {
		ffh_ctxt->invoke_ffh_fn = __arm_smccc_smc;
		ffh_ctxt->invoke_ffh64_fn = arm_smccc_1_2_smc;
	} else {
		ffh_ctxt->invoke_ffh_fn = __arm_smccc_hvc;
		ffh_ctxt->invoke_ffh64_fn = arm_smccc_1_2_hvc;
	}

	memcpy(ffh_ctxt, handler_ctxt, sizeof(ffh_ctxt->info));

	*region_ctxt = ffh_ctxt;
	return AE_OK;
}

static bool acpi_ffh_smccc_owner_allowed(u32 fid)
{
	int owner = ARM_SMCCC_OWNER_NUM(fid);

	if (owner == ARM_SMCCC_OWNER_STANDARD ||
	    owner == ARM_SMCCC_OWNER_SIP || owner == ARM_SMCCC_OWNER_OEM)
		return true;

	return false;
}

/*
 * FFH Operation Regions declared with an Offset of 0x2 trigger an
 * FFA_MSG_SEND_DIRECT_REQ2 call, as described in Arm DEN0048D (Functional
 * Fixed Hardware Specification v1.3) section 2.3.1.2. Every 64-bit field of
 * the region maps to one register, ordered from X0:
 *
 *   X0		status, one of the ACPI_FFH_FFA_* codes below, populated by
 *		OSPM on return
 *   X1		Bits[15:0] hold the receiver endpoint ID, or zero to have OSPM
 *		resolve it from the service UUID
 *   X2-X3	service UUID of the callee partition, written by ACPI platform
 *		firmware with the ToUUID() ASL operator
 *   X4-X17	message payload, X4 is always present
 *
 * The region Length is "32 + 8 * N" bytes with 1 <= N <= 14, which is X0-X4
 * at minimum and X0-X17 at most.
 */
#define ACPI_FFH_FFA_HDR_REGS		4	/* X0 - X3 */
#define ACPI_FFH_FFA_MAX_PAYLOAD_REGS	14	/* X4 - X17 */
#define ACPI_FFH_FFA_UUID_OFFSET	(2 * sizeof(u64))
#define ACPI_FFH_FFA_MIN_LENGTH		((ACPI_FFH_FFA_HDR_REGS + 1) * sizeof(u64))
#define ACPI_FFH_FFA_MAX_LENGTH		\
	((ACPI_FFH_FFA_HDR_REGS + ACPI_FFH_FFA_MAX_PAYLOAD_REGS) * sizeof(u64))

/* DEN0048D table 3, FFH Operation Region status codes for FFA calls */
#define ACPI_FFH_FFA_CALL_FAILED		1
#define ACPI_FFH_FFA_SUCCESS			0
#define ACPI_FFH_FFA_NOT_SUPPORTED		(-1)
#define ACPI_FFH_FFA_INVALID_PARAMETERS		(-2)
#define ACPI_FFH_FFA_OUT_OF_MEMORY		(-3)
#define ACPI_FFH_FFA_UNSPECIFIED_ERROR		(-4)

static const struct acpi_ffh_ffa_ops *ffa_ops;
static DECLARE_RWSEM(ffa_ops_sem);

int acpi_ffh_ffa_register(const struct acpi_ffh_ffa_ops *ops)
{
	int ret = 0;

	if (!ops || !ops->partition_id || !ops->direct_req2)
		return -EINVAL;

	down_write(&ffa_ops_sem);
	if (ffa_ops)
		ret = -EBUSY;
	else
		ffa_ops = ops;
	up_write(&ffa_ops_sem);

	return ret;
}
EXPORT_SYMBOL_GPL(acpi_ffh_ffa_register);

void acpi_ffh_ffa_unregister(const struct acpi_ffh_ffa_ops *ops)
{
	down_write(&ffa_ops_sem);
	if (ffa_ops == ops)
		ffa_ops = NULL;
	up_write(&ffa_ops_sem);
}
EXPORT_SYMBOL_GPL(acpi_ffh_ffa_unregister);

/*
 * ToUUID() emits the UUID in mixed-endian (EFI GUID) byte order whereas FF-A
 * expects the RFC4122 layout, which is exactly a guid_t to uuid_t conversion.
 */
static void acpi_ffh_ffa_uuid(uuid_t *uuid, const u8 *aml_buf)
{
	int i;

	for (i = 0; i < UUID_SIZE; i++)
		uuid->b[i] = aml_buf[guid_index[i]];
}

static int acpi_ffh_ffa_status(int err)
{
	switch (err) {
	case 0:
		return ACPI_FFH_FFA_SUCCESS;
	case -EIO:
	case -EPROTO:
		return ACPI_FFH_FFA_CALL_FAILED;
	case -EOPNOTSUPP:
		return ACPI_FFH_FFA_NOT_SUPPORTED;
	case -EINVAL:
	case -ENOENT:
	case -ENODEV:
	case -ENOTUNIQ:
		return ACPI_FFH_FFA_INVALID_PARAMETERS;
	case -ENOMEM:
		return ACPI_FFH_FFA_OUT_OF_MEMORY;
	default:
		return ACPI_FFH_FFA_UNSPECIFIED_ERROR;
	}
}

static bool acpi_ffh_ffa_length_valid(u64 length)
{
	return length >= ACPI_FFH_FFA_MIN_LENGTH &&
	       length <= ACPI_FFH_FFA_MAX_LENGTH &&
	       !(length % sizeof(u64));
}

static void acpi_ffh_ffa_handler(struct acpi_ffh_info *info, void *value)
{
	int status = ACPI_FFH_FFA_INVALID_PARAMETERS;
	u64 resp_regs[3] = {};
	unsigned int nr_payload;
	u64 *regs = value;
	uuid_t uuid;
	u16 dst_id;
	int ret;

	if (!acpi_ffh_ffa_length_valid(info->length))
		goto out;

	nr_payload = info->length / sizeof(u64) - ACPI_FFH_FFA_HDR_REGS;

	acpi_ffh_ffa_uuid(&uuid, (u8 *)value + ACPI_FFH_FFA_UUID_OFFSET);

	down_read(&ffa_ops_sem);
	if (!ffa_ops) {
		status = ACPI_FFH_FFA_NOT_SUPPORTED;
		goto out_unlock;
	}

	/*
	 * A zero receiver endpoint ID means ACPI platform firmware expects
	 * OSPM to derive it from the service UUID.
	 */
	dst_id = regs[1] & GENMASK(15, 0);
	if (!dst_id) {
		/*
		 * A null UUID means "every partition" to
		 * FFA_PARTITION_INFO_GET, so reject it here rather than let a
		 * bare read of the Operation Region, which arrives as a zeroed
		 * buffer, resolve to an arbitrary endpoint.
		 */
		if (uuid_is_null(&uuid))
			goto out_unlock;

		ret = ffa_ops->partition_id(&uuid, &dst_id);
		if (ret) {
			status = acpi_ffh_ffa_status(ret);
			goto out_unlock;
		}
	}

	ret = ffa_ops->direct_req2(dst_id, &uuid, regs + ACPI_FFH_FFA_HDR_REGS,
				   nr_payload, resp_regs);
	status = acpi_ffh_ffa_status(ret);

	/*
	 * DEN0048D asks for the response registers to be copied back. That also
	 * takes care of table 3: FFA_ERROR reports the FF-A error code in X2,
	 * which is exactly where FFH_FFA_CALL_FAILED wants it. -EIO and -EPROTO
	 * both mean the call completed, so the registers hold the callee's
	 * response and not AML's own request.
	 */
	if (!ret || ret == -EIO || ret == -EPROTO) {
		regs[1] = resp_regs[0];
		regs[2] = resp_regs[1];
		regs[3] = resp_regs[2];
	}

out_unlock:
	up_read(&ffa_ops_sem);
out:
	/*
	 * DEN0048D describes this field as 64 bits wide and gives the table 3
	 * codes as signed values, so sign extend rather than write a narrower
	 * quantity. 0xfffe or 0xfffffffe would read back as a positive number
	 * in a 64-bit AML comparison.
	 */
	regs[0] = (u64)(s64)status;
}

int acpi_ffh_address_space_arch_handler(acpi_integer *value, void *region_context)
{
	int ret = 0;
	struct acpi_ffh_data *ffh_ctxt = region_context;

	if (ffh_ctxt->info.offset == 0) {
		/* SMC/HVC 32bit call */
		struct arm_smccc_res res;
		u32 a[8] = { 0 }, *ptr = (u32 *)value;

		if (!ARM_SMCCC_IS_FAST_CALL(*ptr) || ARM_SMCCC_IS_64(*ptr) ||
		    !acpi_ffh_smccc_owner_allowed(*ptr) ||
		    ffh_ctxt->info.length > 32) {
			ret = AE_ERROR;
		} else {
			int idx, len = ffh_ctxt->info.length >> 2;

			for (idx = 0; idx < len; idx++)
				a[idx] = *(ptr + idx);

			ffh_ctxt->invoke_ffh_fn(a[0], a[1], a[2], a[3], a[4],
						a[5], a[6], a[7], &res, NULL);
			memcpy(value, &res, sizeof(res));
		}

	} else if (ffh_ctxt->info.offset == 1) {
		/* SMC/HVC 64bit call */
		struct arm_smccc_1_2_regs *r = (struct arm_smccc_1_2_regs *)value;

		if (!ARM_SMCCC_IS_FAST_CALL(r->a0) || !ARM_SMCCC_IS_64(r->a0) ||
		    !acpi_ffh_smccc_owner_allowed(r->a0) ||
		    ffh_ctxt->info.length > sizeof(*r)) {
			ret = AE_ERROR;
		} else {
			ffh_ctxt->invoke_ffh64_fn(r, r);
			memcpy(value, r, ffh_ctxt->info.length);
		}
	} else if (ffh_ctxt->info.offset == 2) {
		/* FFA_MSG_SEND_DIRECT_REQ2 call */
		if (ffh_ctxt->info.length < sizeof(u64))
			ret = AE_ERROR;
		else
			acpi_ffh_ffa_handler(&ffh_ctxt->info, value);
	} else {
		ret = AE_ERROR;
	}

	return ret;
}
