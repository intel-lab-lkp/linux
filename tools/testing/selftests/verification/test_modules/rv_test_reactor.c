// SPDX-License-Identifier: GPL-2.0
/*
 * Loadable RV reactor for the verification selftests. Register a
 * reactor with owner = THIS_MODULE so the selftests can exercise the
 * module pinning: unloading is refused while a monitor is attached
 * to the reactor.
 */

#include <linux/module.h>
#include <linux/rv.h>

__printf(1, 0) static void rv_test_reaction(const char *msg, va_list args)
{
}

static struct rv_reactor rv_test_reactor = {
	.name = "test_reactor",
	.description = "selftest reactor: exercise module-based reactors.",
	.react = rv_test_reaction,
	.owner = THIS_MODULE,
};

static int __init rv_test_reactor_init(void)
{
	return rv_register_reactor(&rv_test_reactor);
}

static void __exit rv_test_reactor_exit(void)
{
	rv_unregister_reactor(&rv_test_reactor);
}

module_init(rv_test_reactor_init);
module_exit(rv_test_reactor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Loadable RV reactor for verification selftests");
