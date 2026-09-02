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

struct sbi_cppc_fb_ctrs_data {
	struct sbi_cppc_data first;
	struct sbi_cppc_data second;
};

static void cppc_ffh_csr_read_fb_ctrs(void *read_data)
{
	struct sbi_cppc_fb_ctrs_data *data = read_data;

	cppc_ffh_csr_read(&data->first);
	if (!data->first.ret.error)
		cppc_ffh_csr_read(&data->second);
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

	if (WARN_ON_ONCE(irqs_disabled()))
		return -EPERM;

	if (FFH_CPPC_TYPE(reg->address) == FFH_CPPC_SBI) {
		if (!cppc_ext_present)
			return -EINVAL;

		data.reg = FFH_CPPC_SBI_REG(reg->address);

		smp_call_function_single(cpu, sbi_cppc_read, &data, 1);

		*val = data.ret.value;

		return (data.ret.error) ? sbi_err_map_linux_errno(data.ret.error) : 0;
	} else if (FFH_CPPC_TYPE(reg->address) == FFH_CPPC_CSR) {
		data.reg = FFH_CPPC_CSR_NUM(reg->address);

		smp_call_function_single(cpu, cppc_ffh_csr_read, &data, 1);

		*val = data.ret.value;

		return data.ret.error;
	}

	return -EINVAL;
}

int cpc_read_ffh_fb_ctrs(int cpu, struct cpc_reg *reg1, u64 *val1,
			 struct cpc_reg *reg2, u64 *val2)
{
	struct sbi_cppc_fb_ctrs_data data;
	int ret;

	if (WARN_ON_ONCE(irqs_disabled()))
		return -EPERM;

	/* Only CSR counters can be paired within a single IPI. */
	if (FFH_CPPC_TYPE(reg1->address) != FFH_CPPC_CSR ||
	    FFH_CPPC_TYPE(reg2->address) != FFH_CPPC_CSR)
		return -EOPNOTSUPP;

	data.first.reg = FFH_CPPC_CSR_NUM(reg1->address);
	data.second.reg = FFH_CPPC_CSR_NUM(reg2->address);

	ret = smp_call_function_single(cpu, cppc_ffh_csr_read_fb_ctrs,
				       &data, 1);
	if (ret)
		return ret;

	if (data.first.ret.error)
		return data.first.ret.error;
	if (data.second.ret.error)
		return data.second.ret.error;

	*val1 = data.first.ret.value;
	*val2 = data.second.ret.value;

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
