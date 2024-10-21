/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __REG_CTRL_H
#define __REG_CTRL_H
#include <linux/arm-smccc.h>

#define MAX_REG_NAME_LEN	32

struct reg_desc {
	char name[MAX_REG_NAME_LEN];
	u64 (*read)(void);
	void (*write)(u64 val);
};

#define _REG_DESC_SYSREG(nm, rd, wr)					\
	{								\
		.name = __stringify(nm),				\
		.read = rd,						\
		.write = wr,						\
	}

#define REG_DESC_SYSREG_RW(nm)						\
	_REG_DESC_SYSREG(nm, sysreg_read_##nm, sysreg_write_##nm)

#define REG_DESC_SYSREG_RO(nm)						\
	_REG_DESC_SYSREG(nm, sysreg_read_##nm, NULL)

#define REG_DESC_SYSREG_S_RW(nm)					\
	_REG_DESC_SYSREG(nm, sysreg_read_s_##nm, sysreg_write_s_##nm)

#define REG_DESC_SYSREG_S_RO(nm)					\
	_REG_DESC_SYSREG(nm, sysreg_read_s_##nm, NULL)

#define REG_DESC_SYSREG_SMC_RW(nm)					\
	_REG_DESC_SYSREG(nm, sysreg_smc_read_##nm, sysreg_smc_write_##nm)

#define REG_DESC_SYSREG_SMC_RO(nm)					\
	_REG_DESC_SYSREG(nm, sysreg_smc_read_##nm, NULL)


#define FUNC_SYSREG_RW(nm)						\
static u64 sysreg_read_##nm(void)					\
{									\
	return read_sysreg(nm);						\
}									\
static void sysreg_write_##nm(u64 val)					\
{									\
	write_sysreg(val, nm);						\
}

#define FUNC_SYSREG_RO(nm)						\
static u64 sysreg_read_##nm(void)					\
{									\
	return read_sysreg(nm);						\
}

#define FUNC_SYSREG_S_RW(nm, sys)					\
static u64 sysreg_read_s_##nm(void)					\
{									\
	return read_sysreg_s(sys);					\
}									\
static void sysreg_write_s_##nm(u64 val)				\
{									\
	write_sysreg_s(val, sys);					\
}

#define FUNC_SYSREG_S_RO(nm, sys)					\
static u64 sysreg_read_s_##nm(void)					\
{									\
	return read_sysreg_s(sys);					\
}

#define FUNC_SYSREG_SMC_RW(nm, reg)					\
static u64 sysreg_smc_read_##nm(void)					\
{									\
	struct arm_smccc_res res;					\
	arm_smccc_smc(SMCCC_OEM_REG_CTRL_READ_REG, reg,			\
			0, 0, 0, 0, 0, 0, &res);			\
	return res.a0;							\
}									\
static void sysreg_smc_write_##nm(u64 val)				\
{									\
	struct arm_smccc_res res;					\
	arm_smccc_smc(SMCCC_OEM_REG_CTRL_WRITE_REG, val, reg,		\
			0, 0, 0, 0, 0, &res);				\
}


#define _REG_CTRL_ATTR(name, mode, show, store)				\
	(&((struct kobj_attribute) __ATTR(name, mode, show, store)).attr)

#define REG_CTRL_ATTR_RW(name, show, store)				\
	_REG_CTRL_ATTR(name, 0664, show, store)

#define REG_CTRL_ATTR_RO(name, show)					\
	_REG_CTRL_ATTR(name, 0444, show, NULL)


#endif /* __REG_CTRL_H */
