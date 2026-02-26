// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 Bytedance, Inc.
 */
#define pr_fmt(fmt) "riscv-sys-error: " fmt

#include <linux/kernel.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>
#include <linux/percpu.h>
#include <linux/module.h>
#include <asm/irq.h>
#include <linux/cpuhotplug.h>
#include <asm/csr.h>

static unsigned int riscv_sys_error_irq;
static DEFINE_PER_CPU_READ_MOSTLY(int, sys_error_dummy_dev);

static irqreturn_t sys_error_irq_handler(int irq, void *dev)
{
	panic("RISC-V System Error Interrupt - System Error Detected");
	return IRQ_HANDLED;
}

static int riscv_serror_starting_cpu(unsigned int cpu)
{
	csr_set(CSR_IE, BIT(RV_IRQ_SYS_ERROR));
	enable_percpu_irq(riscv_sys_error_irq, irq_get_trigger_type(riscv_sys_error_irq));
	return 0;
}

static int riscv_serror_dying_cpu(unsigned int cpu)
{
	csr_clear(CSR_IE, BIT(RV_IRQ_SYS_ERROR));
	disable_percpu_irq(riscv_sys_error_irq);
	return 0;
}

static int __init sys_error_init(void)
{
	int ret;
	struct irq_domain *domain = NULL;

	domain = irq_find_matching_fwnode(riscv_get_intc_hwnode(),
					  DOMAIN_BUS_ANY);
	if (!domain) {
		pr_err("Failed to find INTC IRQ root domain\n");
		return -ENODEV;
	}

	riscv_sys_error_irq = irq_create_mapping(domain, RV_IRQ_SYS_ERROR);
	if (!riscv_sys_error_irq) {
		pr_err("Failed to map PMU interrupt for node\n");
		return -ENODEV;
	}

	ret = request_percpu_irq(riscv_sys_error_irq, sys_error_irq_handler,
				 "riscv-syserror", &sys_error_dummy_dev);
	if (ret) {
		pr_err("registering percpu irq failed [%d]\n", ret);
		return ret;
	}

	ret = cpuhp_setup_state(CPUHP_AP_RISCV_SERROR_STARTING,
			 "riscv/sys_error:starting",
			 riscv_serror_starting_cpu, riscv_serror_dying_cpu);
	if (ret) {
		pr_err("cpuhp setup state failed [%d]\n", ret);
		goto fail_free_irq;
	}

	return 0;

fail_free_irq:
	free_percpu_irq(riscv_sys_error_irq, &sys_error_dummy_dev);
	return ret;
}

arch_initcall(sys_error_init)
