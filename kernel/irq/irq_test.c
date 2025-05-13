// SPDX-License-Identifier: LGPL-2.1+

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irqdomain.h>
#include <linux/nodemask.h>
#include <kunit/test.h>

#include "internals.h"

static irqreturn_t noop_handler(int, void *)
{
	return IRQ_HANDLED;
}

static void noop(struct irq_data *data) { }
static unsigned int noop_ret(struct irq_data *data) { return 0; }

static int noop_affinity(struct irq_data *data, const struct cpumask *dest, bool force)
{
	irq_data_update_effective_affinity(data, dest);

	return 0;
}

static struct irq_chip fake_irq_chip = {
	.name           = "fake",
	.irq_startup    = noop_ret,
	.irq_shutdown   = noop,
	.irq_enable     = noop,
	.irq_disable    = noop,
	.irq_ack        = noop,
	.irq_mask       = noop,
	.irq_unmask     = noop,
	.irq_set_affinity = noop_affinity,
	.flags          = IRQCHIP_SKIP_SET_WAKE,
};

static void irq_disable_depth_test(struct kunit *test)
{
	struct irq_desc *desc;
	int virq, ret;

	virq = irq_domain_alloc_descs(-1 /*virq*/, 1 /*nr_irqs*/, 0/*hwirq*/, first_online_node/*node*/, NULL);
	KUNIT_ASSERT_GE(test, virq, 0);

	irq_set_chip_and_handler(virq, &dummy_irq_chip, handle_simple_irq);

	desc = irq_to_desc(virq);
	KUNIT_ASSERT_PTR_NE(test, desc, NULL);

	ret = request_irq(virq, noop_handler, 0, "test_irq", NULL);
	KUNIT_EXPECT_EQ(test, ret, 0);

	KUNIT_EXPECT_EQ(test, desc->depth, 0);

	disable_irq(virq);
	KUNIT_EXPECT_EQ(test, desc->depth, 1);

	enable_irq(virq);
	KUNIT_EXPECT_EQ(test, desc->depth, 0);

	/* TODO: free virq? */
}

static void irq_shutdown_depth_test(struct kunit *test)
{
	struct irq_desc *desc;
	int virq, ret;

	virq = irq_domain_alloc_descs(-1 /*virq*/, 1 /*nr_irqs*/, 0/*hwirq*/, first_online_node/*node*/, NULL);
	KUNIT_ASSERT_GE(test, virq, 0);

	irq_set_chip_and_handler(virq, &dummy_irq_chip, handle_simple_irq);

	desc = irq_to_desc(virq);
	KUNIT_ASSERT_PTR_NE(test, desc, NULL);

	ret = request_irq(virq, noop_handler, 0, "test_irq", NULL);
	KUNIT_EXPECT_EQ(test, ret, 0);

	KUNIT_EXPECT_EQ(test, desc->depth, 0);

	disable_irq(virq);
	KUNIT_EXPECT_EQ(test, desc->depth, 1);

	irq_shutdown_and_deactivate(desc);
	KUNIT_EXPECT_EQ(test, irq_activate_and_startup(desc, IRQ_NORESEND), 0);

	KUNIT_EXPECT_EQ(test, desc->depth, 1);

	enable_irq(virq);
	KUNIT_EXPECT_EQ(test, desc->depth, 0);

	/* TODO: free virq? */
}

static void irq_cpuhotplug_test(struct kunit *test)
{
	struct irq_desc *desc;
	struct irq_data *data;
	int virq, ret;
	struct irq_affinity_desc affinity = {
		.is_managed = 1,
	};

	cpumask_copy(&affinity.mask, cpumask_of(1));
	KUNIT_ASSERT_PTR_NE(test, get_cpu_device(1), NULL);
	KUNIT_ASSERT_TRUE(test, cpu_is_hotpluggable(1));

	virq = irq_domain_alloc_descs(-1 /*virq*/, 1 /*nr_irqs*/, 0/*hwirq*/, first_online_node/*node*/, &affinity);
	KUNIT_ASSERT_GE(test, virq, 0);

	irq_set_chip_and_handler(virq, &fake_irq_chip, handle_simple_irq);

	desc = irq_to_desc(virq);
	KUNIT_ASSERT_PTR_NE(test, desc, NULL);

	data = irq_desc_get_irq_data(desc);
	KUNIT_ASSERT_PTR_NE(test, data, NULL);

	ret = request_irq(virq, noop_handler, 0, "test_irq", NULL);
	KUNIT_EXPECT_EQ(test, ret, 0);

	KUNIT_EXPECT_TRUE(test, irqd_is_activated(data));
	KUNIT_EXPECT_TRUE(test, irqd_is_started(data));
	KUNIT_EXPECT_TRUE(test, irqd_affinity_is_managed(data));

	KUNIT_EXPECT_EQ(test, desc->depth, 0);

	disable_irq(virq);
	KUNIT_EXPECT_EQ(test, desc->depth, 1);

	KUNIT_EXPECT_EQ(test, remove_cpu(1), 0);
	KUNIT_EXPECT_EQ(test, add_cpu(1), 0);

	KUNIT_EXPECT_EQ(test, desc->depth, 1);

	enable_irq(virq);
	KUNIT_EXPECT_EQ(test, desc->depth, 0);

	/* TODO: free virq? */
}

static struct kunit_case irq_test_cases[] = {
	KUNIT_CASE_SLOW(irq_disable_depth_test),
	KUNIT_CASE_SLOW(irq_shutdown_depth_test),
	KUNIT_CASE_SLOW(irq_cpuhotplug_test),
	{}
};

static struct kunit_suite irq_test_suite = {
	.name = "irq_test_cases",
	.test_cases = irq_test_cases,
};

kunit_test_suite(irq_test_suite);
MODULE_DESCRIPTION("IRQ unit test suite");
MODULE_LICENSE("GPL");
