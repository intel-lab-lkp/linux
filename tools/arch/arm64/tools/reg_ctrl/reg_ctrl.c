// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <asm/sysreg.h>
#include <linux/string.h>

#include "reg_ctrl.h"
#include "sysreg.h"

#define SMCCC_OEM_REG_CTRL_READ_REG 0xC3000002
#define SMCCC_OEM_REG_CTRL_WRITE_REG 0xC3000003

static struct kobject *reg_kobj;
static struct kobject *system_kobj;

static ssize_t reg_show(struct kobject *kobj, struct kobj_attribute *attr,
		char *buf);

static ssize_t reg_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count);

FUNC_SYSREG_S_RW(VNCR_EL2, SYS_VNCR_EL2);

FUNC_SYSREG_S_RO(CCSIDR_EL1, SYS_CCSIDR_EL1);
FUNC_SYSREG_S_RO(CLIDR_EL1, SYS_CLIDR_EL1);
FUNC_SYSREG_S_RO(CSSELR_EL1, SYS_CSSELR_EL1);
FUNC_SYSREG_S_RO(CTR_EL0, SYS_CTR_EL0);
FUNC_SYSREG_S_RO(DCZID_EL0, SYS_DCZID_EL0);
FUNC_SYSREG_S_RO(ID_AA64AFR0_EL1, SYS_ID_AA64AFR0_EL1);
FUNC_SYSREG_S_RO(ID_AA64AFR1_EL1, SYS_ID_AA64AFR1_EL1);
FUNC_SYSREG_S_RO(ID_AA64DFR0_EL1, SYS_ID_AA64DFR0_EL1);
FUNC_SYSREG_S_RO(ID_AA64DFR1_EL1, SYS_ID_AA64DFR1_EL1);
FUNC_SYSREG_S_RO(ID_AA64ISAR0_EL1, SYS_ID_AA64ISAR0_EL1);
FUNC_SYSREG_S_RO(ID_AA64ISAR1_EL1, SYS_ID_AA64ISAR1_EL1);
FUNC_SYSREG_S_RO(ID_AA64MMFR0_EL1, SYS_ID_AA64MMFR0_EL1);
FUNC_SYSREG_S_RO(ID_AA64MMFR1_EL1, SYS_ID_AA64MMFR1_EL1);
FUNC_SYSREG_S_RO(ID_AA64PFR0_EL1, SYS_ID_AA64PFR0_EL1);
FUNC_SYSREG_S_RO(ID_AA64PFR1_EL1, SYS_ID_AA64PFR1_EL1);

FUNC_SYSREG_SMC_RW(RMR_EL3, SYS_RMR_EL3);

FUNC_SYSREG_S_RW(IMP_CPUECTLR_EL1, SYS_IMP_CPUECTLR_EL1);
FUNC_SYSREG_SMC_RW(IMP_CPUACTLR_EL3, SYS_IMP_CPUACTLR_EL3);
FUNC_SYSREG_SMC_RW(IMP_CPUPPMCR_EL3, SYS_IMP_CPUPPMCR_EL3);
FUNC_SYSREG_SMC_RW(IMP_CPUPPMCR2_EL3, SYS_IMP_CPUPPMCR2_EL3);
FUNC_SYSREG_SMC_RW(IMP_CPUPPMCR4_EL3, SYS_IMP_CPUPPMCR4_EL3);
FUNC_SYSREG_SMC_RW(IMP_CPUPPMCR5_EL3, SYS_IMP_CPUPPMCR5_EL3);
FUNC_SYSREG_SMC_RW(IMP_CPUPPMCR6_EL3, SYS_IMP_CPUPPMCR6_EL3);

// System registers
static struct reg_desc system_regs[] = {
	/* CONTROL */
	REG_DESC_SYSREG_S_RW(VNCR_EL2),

	/* ID */
	REG_DESC_SYSREG_S_RO(CCSIDR_EL1),
	REG_DESC_SYSREG_S_RO(CLIDR_EL1),
	REG_DESC_SYSREG_S_RO(CSSELR_EL1),
	REG_DESC_SYSREG_S_RO(CTR_EL0),
	REG_DESC_SYSREG_S_RO(DCZID_EL0),
	REG_DESC_SYSREG_S_RO(ID_AA64AFR0_EL1),
	REG_DESC_SYSREG_S_RO(ID_AA64AFR1_EL1),
	REG_DESC_SYSREG_S_RO(ID_AA64DFR0_EL1),
	REG_DESC_SYSREG_S_RO(ID_AA64DFR1_EL1),
	REG_DESC_SYSREG_S_RO(ID_AA64ISAR0_EL1),
	REG_DESC_SYSREG_S_RO(ID_AA64ISAR1_EL1),
	REG_DESC_SYSREG_S_RO(ID_AA64MMFR0_EL1),
	REG_DESC_SYSREG_S_RO(ID_AA64MMFR1_EL1),
	REG_DESC_SYSREG_S_RO(ID_AA64PFR0_EL1),
	REG_DESC_SYSREG_S_RO(ID_AA64PFR1_EL1),

	/* reset */
	REG_DESC_SYSREG_SMC_RW(RMR_EL3),

	/* implementation defined */
	REG_DESC_SYSREG_S_RW(IMP_CPUECTLR_EL1),
	REG_DESC_SYSREG_SMC_RW(IMP_CPUACTLR_EL3),
	REG_DESC_SYSREG_SMC_RW(IMP_CPUPPMCR_EL3),
	REG_DESC_SYSREG_SMC_RW(IMP_CPUPPMCR2_EL3),
	REG_DESC_SYSREG_SMC_RW(IMP_CPUPPMCR4_EL3),
	REG_DESC_SYSREG_SMC_RW(IMP_CPUPPMCR5_EL3),
	REG_DESC_SYSREG_SMC_RW(IMP_CPUPPMCR6_EL3),
};

static struct attribute *id_attrs[] = {
	REG_CTRL_ATTR_RO(CCSIDR_EL1, reg_show),
	REG_CTRL_ATTR_RO(CLIDR_EL1, reg_show),
	REG_CTRL_ATTR_RO(CSSELR_EL1, reg_show),
	REG_CTRL_ATTR_RO(CTR_EL0, reg_show),
	REG_CTRL_ATTR_RO(DCZID_EL0, reg_show),
	REG_CTRL_ATTR_RO(ID_AA64AFR0_EL1, reg_show),
	REG_CTRL_ATTR_RO(ID_AA64AFR1_EL1, reg_show),
	REG_CTRL_ATTR_RO(ID_AA64DFR0_EL1, reg_show),
	REG_CTRL_ATTR_RO(ID_AA64DFR1_EL1, reg_show),
	REG_CTRL_ATTR_RO(ID_AA64ISAR0_EL1, reg_show),
	REG_CTRL_ATTR_RO(ID_AA64ISAR1_EL1, reg_show),
	REG_CTRL_ATTR_RO(ID_AA64MMFR0_EL1, reg_show),
	REG_CTRL_ATTR_RO(ID_AA64MMFR1_EL1, reg_show),
	REG_CTRL_ATTR_RO(ID_AA64PFR0_EL1, reg_show),
	REG_CTRL_ATTR_RO(ID_AA64PFR1_EL1, reg_show),
	NULL,
};

static struct attribute_group id_attr_group = {
	.attrs = id_attrs,
	.name = "id"
};

static struct attribute *control_attrs[] = {
	REG_CTRL_ATTR_RW(VNCR_EL2, reg_show, reg_store),
	NULL,
};

static struct attribute_group control_attr_group = {
	.attrs = control_attrs,
	.name = "control"
};

static struct attribute *reset_attrs[] = {
	REG_CTRL_ATTR_RW(RMR_EL3, reg_show, reg_store),
	NULL,
};

static struct attribute_group reset_attr_group = {
	.attrs = reset_attrs,
	.name = "reset"
};

static struct attribute *implementation_defined_attrs[] = {
	REG_CTRL_ATTR_RW(IMP_CPUECTLR_EL1, reg_show, reg_store),
	REG_CTRL_ATTR_RW(IMP_CPUACTLR_EL3, reg_show, reg_store),
	REG_CTRL_ATTR_RW(IMP_CPUPPMCR_EL3, reg_show, reg_store),
	REG_CTRL_ATTR_RW(IMP_CPUPPMCR2_EL3, reg_show, reg_store),
	REG_CTRL_ATTR_RW(IMP_CPUPPMCR4_EL3, reg_show, reg_store),
	REG_CTRL_ATTR_RW(IMP_CPUPPMCR5_EL3, reg_show, reg_store),
	REG_CTRL_ATTR_RW(IMP_CPUPPMCR6_EL3, reg_show, reg_store),
	NULL,
};

static struct attribute_group implementation_defined_attr_group = {
	.attrs = implementation_defined_attrs,
	.name = "implementation_defined"
};

static const struct attribute_group *system_attr_groups[] = {
	&id_attr_group,
	&control_attr_group,
	&reset_attr_group,
	&implementation_defined_attr_group,
	NULL,
};

static struct reg_desc *get_reg_desc(const char *group, const char *name)
{
	struct reg_desc *regs = NULL;
	int size = 0, i;

	if (strcmp(group, "system") == 0) {
		regs = system_regs;
		size = ARRAY_SIZE(system_regs);
	}

	if (regs) {
		for (i = 0; i < size; i++) {
			if (strcmp(name, regs[i].name) == 0)
				return &regs[i];
		}
	}

	return NULL;
}

static ssize_t reg_show(struct kobject *kobj, struct kobj_attribute *attr,
	char *buf)
{
	struct reg_desc *reg = NULL;

	reg = get_reg_desc(kobject_name(kobj), attr->attr.name);

	if (reg && reg->read != NULL)
		return sprintf(buf, "0x%llx\n", reg->read());

	return -EINVAL;
}

static ssize_t reg_store(struct kobject *kobj, struct kobj_attribute *attr,
	const char *buf, size_t count)
{
	struct reg_desc *reg = NULL;
	u64 val;

	reg = get_reg_desc(kobject_name(kobj), attr->attr.name);

	if (reg && reg->write != NULL && !kstrtoull(buf, 0, &val)) {
		reg->write(val);
		return count;
	}
	return -EINVAL;
}

static int __init reg_init(void)
{
	int retval = -1;

	reg_kobj = kobject_create_and_add("reg_ctrl", kernel_kobj);
	if (!reg_kobj)
		return -ENOMEM;

	system_kobj = kobject_create_and_add("system", reg_kobj);
	if (!system_kobj)
		goto fail_system;

	retval = sysfs_create_groups(system_kobj, system_attr_groups);
	if (retval)
		goto fail_system_attr_groups;

	return 0;

fail_system_attr_groups:
	if (system_kobj)
		kobject_put(system_kobj);
fail_system:
	if (reg_kobj)
		kobject_put(reg_kobj);
	return retval;
}

static void __exit reg_exit(void)
{
	if (system_kobj) {
		sysfs_remove_groups(system_kobj, system_attr_groups);
		kobject_put(system_kobj);
	}

	if (reg_kobj)
		kobject_put(reg_kobj);
}

module_init(reg_init);
module_exit(reg_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("jaguarmicro");
MODULE_DESCRIPTION("reg_ctrl is a tool to read/write ARM64 system registers");
