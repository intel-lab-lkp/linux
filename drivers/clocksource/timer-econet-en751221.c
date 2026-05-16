// SPDX-License-Identifier: GPL-2.0
/*
 * Timer present on EcoNet EN75xx MIPS based SoCs.
 *
 * Copyright (C) 2025 by Caleb James DeLisle <cjd@cjdns.fr>
 */

#include <linux/io.h>
#include <linux/cpumask.h>
#include <linux/interrupt.h>
#include <linux/clockchips.h>
#include <linux/sched_clock.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/cpuhotplug.h>
#include <linux/clk.h>

#define ECONET_BITS			32
#define ECONET_MIN_DELTA		0x00001000
#define ECONET_MAX_DELTA		GENMASK(ECONET_BITS - 2, 0)
/* 34Kc hardware has 1 block and 1004Kc has 2. */
#define ECONET_NUM_BLOCKS		DIV_ROUND_UP(NR_CPUS, 2)
#define ECONET_NUM_IRQS			NR_CPUS

static struct {
	void __iomem	*membase[ECONET_NUM_BLOCKS];
	int		irqs[ECONET_NUM_IRQS];
	bool		is_percpu;
	u32		freq_hz;
} econet_timer __ro_after_init;

static DEFINE_PER_CPU(struct clock_event_device, econet_timer_pcpu);

/* Each memory block has 2 timers, the order of registers is:
 * CTL, CMR0, CNT0, CMR1, CNT1
 */
static inline void __iomem *reg_ctl(u32 timer_n)
{
	return econet_timer.membase[timer_n >> 1];
}

static inline void __iomem *reg_compare(u32 timer_n)
{
	return econet_timer.membase[timer_n >> 1] + (timer_n & 1) * 0x08 + 0x04;
}

static inline void __iomem *reg_count(u32 timer_n)
{
	return econet_timer.membase[timer_n >> 1] + (timer_n & 1) * 0x08 + 0x08;
}

static inline u32 ctl_bit_enabled(u32 timer_n)
{
	return 1U << (timer_n & 1);
}

static inline u32 ctl_bit_pending(u32 timer_n)
{
	return 1U << ((timer_n & 1) + 16);
}

static bool cevt_is_pending(int cpu_id)
{
	return ioread32(reg_ctl(cpu_id)) & ctl_bit_pending(cpu_id);
}

static irqreturn_t cevt_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *dev = this_cpu_ptr(&econet_timer_pcpu);
	int cpu = cpumask_first(dev->cpumask);

	/* Each VPE has its own events,
	 * so this will only happen on spurious interrupt.
	 */
	if (!cevt_is_pending(cpu))
		return IRQ_NONE;

	iowrite32(ioread32(reg_count(cpu)), reg_compare(cpu));
	dev->event_handler(dev);
	return IRQ_HANDLED;
}

static int cevt_set_next_event(ulong delta, struct clock_event_device *dev)
{
	u32 next;
	int cpu;

	cpu = cpumask_first(dev->cpumask);
	next = ioread32(reg_count(cpu)) + delta;
	iowrite32(next, reg_compare(cpu));

	if ((s32)(next - ioread32(reg_count(cpu))) < ECONET_MIN_DELTA / 2)
		return -ETIME;

	return 0;
}

static int cevt_init_cpu(uint cpu)
{
	struct clock_event_device *cd = &per_cpu(econet_timer_pcpu, cpu);
	u32 reg;

	if (!reg_ctl(cpu)) {
		pr_err("%s: missing address resource for CPU %d\n", cd->name,
		       cpu);
		return -EINVAL;
	}
	if (cd->irq <= 0) {
		pr_err("%s: missing IRQ for CPU %d\n", cd->name, cpu);
		return -EINVAL;
	}
	if (!econet_timer.is_percpu) {
		int ret = irq_force_affinity(cd->irq, cpumask_of(cpu));

		if (ret) {
			pr_err("%s: failed to set IRQ affinity to CPU %d: %pe\n",
			       cd->name, cpu, ERR_PTR(ret));
			return ret;
		}
	}

	pr_debug("%s: Setting up clockevent for CPU %d\n", cd->name, cpu);

	reg = ioread32(reg_ctl(cpu)) | ctl_bit_enabled(cpu);
	iowrite32(reg, reg_ctl(cpu));

	clockevents_config_and_register(cd, econet_timer.freq_hz,
					ECONET_MIN_DELTA, ECONET_MAX_DELTA);

	if (econet_timer.is_percpu)
		enable_percpu_irq(cd->irq, IRQ_TYPE_NONE);
	else
		enable_irq(cd->irq);

	return 0;
}

static u64 notrace sched_clock_read(void)
{
	/* Always read from clock zero no matter the CPU */
	return (u64)ioread32(reg_count(0));
}

/* Init */

static void __init cevt_dev_init(uint cpu)
{
	iowrite32(0, reg_count(cpu));
	iowrite32(U32_MAX, reg_compare(cpu));
}

static void __init cevt_init(struct device_node *np)
{
	int i;

	for_each_possible_cpu(i) {
		struct clock_event_device *cd = &per_cpu(econet_timer_pcpu, i);

		cd->rating		= 310;
		cd->features		= CLOCK_EVT_FEAT_ONESHOT |
					  CLOCK_EVT_FEAT_C3STOP |
					  CLOCK_EVT_FEAT_PERCPU;
		cd->set_next_event	= cevt_set_next_event;

		if (econet_timer.is_percpu)
			cd->irq = econet_timer.irqs[0];
		else
			cd->irq = econet_timer.irqs[i];

		cd->cpumask		= cpumask_of(i);
		cd->name		= np->name;

		/*
		 * Tolerate CPUs that could exist but don't.
		 * Fail in cevt_init_cpu when they try to start.
		 */
		if (reg_ctl(i))
			cevt_dev_init(i);
	}
}

static int __init timer_init(struct device_node *np)
{
	int num_blocks = of_address_count(np);
	int num_irqs = of_irq_count(np);
	struct clk *clk;
	int ret, i;

	econet_timer.is_percpu = of_device_is_compatible(np, "econet,en751221-timer");

	if (econet_timer.is_percpu && num_irqs != 1) {
		pr_err("%pOFn: EN751221 clock must have 1 IRQ not %d\n", np,
		       num_irqs);
		return -EINVAL;
	}
	if (num_irqs > ARRAY_SIZE(econet_timer.irqs)) {
		pr_err("%pOFn: Too many IRQs max %d got %d\n", np,
		       ARRAY_SIZE(econet_timer.irqs), num_irqs);
		return -EINVAL;
	}
	if (num_blocks > ARRAY_SIZE(econet_timer.membase)) {
		pr_err("%pOFn: Too many regs: max %d got %d\n", np,
		       ARRAY_SIZE(econet_timer.membase), num_blocks);
		return -EINVAL;
	}

	clk = of_clk_get(np, 0);
	if (IS_ERR(clk)) {
		pr_err("%pOFn: Failed to get CPU clock from DT %ld\n", np, PTR_ERR(clk));
		return PTR_ERR(clk);
	}

	econet_timer.freq_hz = clk_get_rate(clk);

	for (i = 0; i < num_blocks; i++) {
		econet_timer.membase[i] = of_iomap(np, i);
		if (!econet_timer.membase[i]) {
			pr_err("%pOFn: failed to map register [%d]\n", np, i);
			ret = -ENXIO;
			goto out_membase;
		}
	}

	for (i = 0; i < num_irqs; i++) {
		econet_timer.irqs[i] = irq_of_parse_and_map(np, i);
		if (econet_timer.irqs[i] <= 0) {
			pr_err("%pOFn: failed mapping irq %d\n", np, i);
			ret = -EINVAL;
			goto out_irq_mapping;
		}
	}

	for (i = 0; i < num_irqs; i++) {
		irq_set_status_flags(econet_timer.irqs[i], IRQ_NOAUTOEN);

		if (econet_timer.is_percpu)
			ret = request_percpu_irq(econet_timer.irqs[i],
						 cevt_interrupt, np->name,
						 &econet_timer_pcpu);
		else
			ret = request_irq(econet_timer.irqs[i], cevt_interrupt,
					  IRQF_TIMER | IRQF_NOBALANCING,
					  np->name, NULL);

		if (ret < 0) {
			pr_err("%pOFn: IRQ %d setup failed: %pe\n", np,
			       i, ERR_PTR(ret));
			goto out_irq_free;
		}
	}

	cevt_init(np);

	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN,
				"clockevents/econet/timer:starting",
				cevt_init_cpu, NULL);
	if (ret < 0) {
		pr_err("%pOFn: cpuhp setup failed (%d)\n", np, ret);
		goto out_irq_free;
	}

	/* Point of no return, do not attempt to tear down after this. */

	/* For clocksource purposes always read clock zero, whatever the CPU */
	ret = clocksource_mmio_init(reg_count(0), np->name,
				    econet_timer.freq_hz, 301, ECONET_BITS,
				    clocksource_mmio_readl_up);
	if (ret)
		pr_err("%pOFn: clocksource_mmio_init failed: %d\n", np, ret);

	sched_clock_register(sched_clock_read, ECONET_BITS,
			     econet_timer.freq_hz);

	pr_info("%pOFn: using %u.%03u MHz high precision timer\n", np,
		econet_timer.freq_hz / 1000000,
		(econet_timer.freq_hz / 1000) % 1000);

	return 0;

out_irq_free:
	while (--i >= 0) {
		if (econet_timer.is_percpu) {
			free_percpu_irq(econet_timer.irqs[i], &econet_timer_pcpu);
		} else {
			free_irq(econet_timer.irqs[i], NULL);
		}
	}
out_irq_mapping:
	for (i = 0; i < num_irqs; i++) {
		if (econet_timer.irqs[i] > 0)
			irq_dispose_mapping(econet_timer.irqs[i]);
	}
out_membase:
	for (i = 0; i < num_blocks; i++) {
		if (econet_timer.membase[i])
			iounmap(econet_timer.membase[i]);
	}

	return ret;
}

TIMER_OF_DECLARE(econet_timer_hpt, "econet,en751221-timer", timer_init);
TIMER_OF_DECLARE(econet_timer_en751627, "econet,en751627-timer", timer_init);
