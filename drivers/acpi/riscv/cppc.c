// SPDX-License-Identifier: GPL-2.0-only
/*
 * Implement CPPC FFH helper routines for RISC-V.
 *
 * Copyright (C) 2024 Ventana Micro Systems Inc.
 */

#include <acpi/cppc_acpi.h>
#include <asm/csr.h>
#include <asm/sbi.h>

#define SBI_EXT_CPPC 0x43505043

/* CPPC interfaces defined in SBI spec */
#define SBI_CPPC_PROBE			0x0
#define SBI_CPPC_READ			0x1
#define SBI_CPPC_READ_HI		0x2
#define SBI_CPPC_WRITE			0x3

/* RISC-V FFH definitions from RISC-V FFH spec */
#define FFH_CPPC_TYPE(r)		(((r) & GENMASK_ULL(63, 60)) >> 60)
#define FFH_CPPC_SBI_REG(r)		((r) & GENMASK(31, 0))
#define FFH_CPPC_CSR_NUM(r)		((r) & GENMASK(11, 0))

#define FFH_CPPC_SBI			0x1
#define FFH_CPPC_CSR			0x2

struct sbi_cppc_data {
	u64 val;
	u32 reg;
	struct sbiret ret;
};

static bool cppc_ext_present;

static int __init sbi_cppc_init(void)
{
	if (sbi_spec_version >= sbi_mk_version(2, 0) &&
	    sbi_probe_extension(SBI_EXT_CPPC) > 0) {
		cppc_ext_present = true;
	} else {
		cppc_ext_present = false;
	}

	return 0;
}
device_initcall(sbi_cppc_init);

static void sbi_cppc_read(void *read_data)
{
	struct sbi_cppc_data *data = (struct sbi_cppc_data *)read_data;

	data->ret = sbi_ecall(SBI_EXT_CPPC, SBI_CPPC_READ,
			      data->reg, 0, 0, 0, 0, 0);
}

static void sbi_cppc_write(void *write_data)
{
	struct sbi_cppc_data *data = (struct sbi_cppc_data *)write_data;

	data->ret = sbi_ecall(SBI_EXT_CPPC, SBI_CPPC_WRITE,
			      data->reg, data->val, 0, 0, 0, 0);
}

static void cppc_ffh_csr_read(void *read_data)
{
	struct sbi_cppc_data *data = (struct sbi_cppc_data *)read_data;

	switch (data->reg) {
	/* Support only TIME CSR for now */
	case CSR_TIME:
		data->ret.value = csr_read(CSR_TIME);
		data->ret.error = 0;
		break;
	default:
		data->ret.error = -EINVAL;
		break;
	}
}

static void cppc_ffh_csr_write(void *write_data)
{
	struct sbi_cppc_data *data = (struct sbi_cppc_data *)write_data;

	data->ret.error = -EINVAL;
}

struct cppc_ffh_ctr {
	struct sbi_cppc_data data;
	u64 type;
};

struct cppc_ffh_fb_ctrs_data {
	struct cppc_ffh_ctr first;
	struct cppc_ffh_ctr second;
};

static void cppc_ffh_read_fb_ctrs(void *read_data)
{
	struct cppc_ffh_fb_ctrs_data *data = read_data;

	if (data->first.type == FFH_CPPC_SBI)
		sbi_cppc_read(&data->first.data);
	else
		cppc_ffh_csr_read(&data->first.data);

	if (data->second.type == FFH_CPPC_SBI)
		sbi_cppc_read(&data->second.data);
	else
		cppc_ffh_csr_read(&data->second.data);
}

static int cppc_ffh_ctr_errno(const struct cppc_ffh_ctr *ctr)
{
	if (!ctr->data.ret.error)
		return 0;

	return ctr->type == FFH_CPPC_SBI ?
	       sbi_err_map_linux_errno(ctr->data.ret.error) :
	       ctr->data.ret.error;
}

static int cppc_ffh_read_on_cpu(int cpu, smp_call_func_t func, void *data)
{
	if (irqs_disabled()) {
		/* Remote reads require IPIs, which are unsafe with IRQs disabled. */
		if (WARN_ON_ONCE(cpu != smp_processor_id()))
			return -EPERM;

		func(data);
		return 0;
	}

	return smp_call_function_single(cpu, func, data, 1);
}

/*
 * Refer to drivers/acpi/cppc_acpi.c for the description of the functions
 * below.
 */
bool cpc_ffh_supported(void)
{
	return true;
}

int cpc_read_ffh(int cpu, struct cpc_reg *reg, u64 *val)
{
	struct sbi_cppc_data data;
	int ret;

	if (FFH_CPPC_TYPE(reg->address) == FFH_CPPC_SBI) {
		if (!cppc_ext_present)
			return -EINVAL;

		data.reg = FFH_CPPC_SBI_REG(reg->address);

		ret = cppc_ffh_read_on_cpu(cpu, sbi_cppc_read, &data);
		if (ret)
			return ret;

		*val = data.ret.value;

		return (data.ret.error) ? sbi_err_map_linux_errno(data.ret.error) : 0;
	} else if (FFH_CPPC_TYPE(reg->address) == FFH_CPPC_CSR) {
		data.reg = FFH_CPPC_CSR_NUM(reg->address);

		ret = cppc_ffh_read_on_cpu(cpu, cppc_ffh_csr_read, &data);
		if (ret)
			return ret;

		*val = data.ret.value;

		return data.ret.error;
	}

	return -EINVAL;
}

int cpc_read_ffh_fb_ctrs(int cpu, struct cpc_reg *reg1, u64 *val1,
			 struct cpc_reg *reg2, u64 *val2)
{
	struct cppc_ffh_fb_ctrs_data data;
	int ret;

	data.first.type = FFH_CPPC_TYPE(reg1->address);
	data.second.type = FFH_CPPC_TYPE(reg2->address);

	if ((data.first.type != FFH_CPPC_SBI && data.first.type != FFH_CPPC_CSR) ||
	    (data.second.type != FFH_CPPC_SBI && data.second.type != FFH_CPPC_CSR))
		return -EINVAL;

	if ((data.first.type == FFH_CPPC_SBI || data.second.type == FFH_CPPC_SBI) &&
	    !cppc_ext_present)
		return -EINVAL;

	data.first.data.reg = data.first.type == FFH_CPPC_SBI ?
			      FFH_CPPC_SBI_REG(reg1->address) :
			      FFH_CPPC_CSR_NUM(reg1->address);
	data.second.data.reg = data.second.type == FFH_CPPC_SBI ?
			       FFH_CPPC_SBI_REG(reg2->address) :
			       FFH_CPPC_CSR_NUM(reg2->address);

	ret = cppc_ffh_read_on_cpu(cpu, cppc_ffh_read_fb_ctrs, &data);
	if (ret)
		return ret;

	ret = cppc_ffh_ctr_errno(&data.first);
	if (ret)
		return ret;
	ret = cppc_ffh_ctr_errno(&data.second);
	if (ret)
		return ret;

	*val1 = data.first.data.ret.value;
	*val2 = data.second.data.ret.value;

	return 0;
}

int cpc_write_ffh(int cpu, struct cpc_reg *reg, u64 val)
{
	struct sbi_cppc_data data;

	if (WARN_ON_ONCE(irqs_disabled()))
		return -EPERM;

	if (FFH_CPPC_TYPE(reg->address) == FFH_CPPC_SBI) {
		if (!cppc_ext_present)
			return -EINVAL;

		data.reg = FFH_CPPC_SBI_REG(reg->address);
		data.val = val;

		smp_call_function_single(cpu, sbi_cppc_write, &data, 1);

		return (data.ret.error) ? sbi_err_map_linux_errno(data.ret.error) : 0;
	} else if (FFH_CPPC_TYPE(reg->address) == FFH_CPPC_CSR) {
		data.reg = FFH_CPPC_CSR_NUM(reg->address);
		data.val = val;

		smp_call_function_single(cpu, cppc_ffh_csr_write, &data, 1);

		return data.ret.error;
	}

	return -EINVAL;
}
