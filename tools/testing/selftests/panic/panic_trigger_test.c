// SPDX-License-Identifier: GPL-2.0
/*
 * panic_test.c - Module to test kernel panic
 */

#include <linux/module.h>
#include <linux/init.h>

static int __init panic_test_init(void)
{
    pr_info("Triggering a deliberate kernel panic now.\n");
    panic("Triggered by panic_test module.");
    return 0;
}

static void __exit panic_test_exit(void)
{
    pr_info("This should not be printed, as system panics on init.\n");
}

module_init(panic_test_init);
module_exit(panic_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vishal Parmar");
MODULE_DESCRIPTION("Module to trigger kernel panic for testing");
