// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitmap.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/cmdline.h>
#include <linux/uaccess.h>

#include "../tools/testing/selftests/kselftest_module.h"

KSTM_MODULE_GLOBALS();

char test1_prepend[] = "prepend ";
char test1_append[] = " append";
char test1_bootloader_args[] = "console=ttyS0 log_level=3";
char test1_result[] = "prepend console=ttyS0 log_level=3 append";

char test2_append[] = " append";
char test2_bootloader_args[] = "console=ttyS0 log_level=3";
char test2_result[] = "console=ttyS0 log_level=3 append";

char test3_prepend[] = "prepend ";
char test3_bootloader_args[] = "console=ttyS0 log_level=3";
char test3_result[] = "prepend console=ttyS0 log_level=3";

char test4_bootloader_args[] = "console=ttyS0 log_level=3";
char test4_result[] = "console=ttyS0 log_level=3";

char test5_prepend[] = "prepend ";
char test5_append[] = " append";
static char test5_bootloader_args[] =
"00000000000000 011111111111111 0222222222222222 033333333333333 "
"10000000000000 111111111111111 1222222222222222 133333333333333 "
"20000000000000 211111111111111 2222222222222222 233333333333333 "
"30000000000000 311111111111111 3222222222222222 333333333333333 "
"40000000000000 411111111111111 4222222222222222 433333333333333 "
"50000000000000 511111111111111 5222222222222222 533333333333333 "
"60000000000000 611111111111111 6222222222222222 633333333333333 "
"70000000000000 711111111111111 7222222222222222 733333333333333";
static char test5_result[] =
"prepend 00000000000000 011111111111111 0222222222222222 033333333333333 "
"10000000000000 111111111111111 1222222222222222 133333333333333 "
"20000000000000 211111111111111 2222222222222222 233333333333333 "
"30000000000000 311111111111111 3222222222222222 333333333333333 "
"40000000000000 411111111111111 4222222222222222 433333333333333 "
"50000000000000 511111111111111 5222222222222222 533333333333333 "
"60000000000000 611111111111111 6222222222222222 633333333333333 "
"70000000000000 711111111111111 7222222222222222 7333333";

char test5_boot_command_line[COMMAND_LINE_SIZE];

char test_tmp[COMMAND_LINE_SIZE];

char test_boot_command_line[COMMAND_LINE_SIZE];

static void __init selftest(void)
{
	bool result;

	/* Normal operation */
	strcpy(test_boot_command_line, test1_bootloader_args);
	test_tmp[0] = '\0';
	result = __cmdline_add_builtin(test_boot_command_line, test_tmp, test1_prepend, test1_append, COMMAND_LINE_SIZE, CMDLINE_STRLEN, CMDLINE_STRLCAT);

	if (result == true && !strncmp(test_boot_command_line, test1_result, COMMAND_LINE_SIZE)) {
		pr_info("test1 success.\n");
	} else {
		pr_info("test1 failed. OUTPUT BELOW:\n");
		pr_info("\"%s\"\n", test_boot_command_line);
		failed_tests++;
	}
	total_tests++;

	/* Missing prepend */
	strcpy(test_boot_command_line, test2_bootloader_args);
	test_tmp[0] = '\0';
	result = __cmdline_add_builtin(test_boot_command_line, test_tmp, "", test2_append, COMMAND_LINE_SIZE, CMDLINE_STRLEN, CMDLINE_STRLCAT);

	if (result == true && !strncmp(test_boot_command_line, test2_result, COMMAND_LINE_SIZE)) {
		pr_info("test2 success.\n");
	} else {
		pr_info("test2 failed. OUTPUT BELOW:\n");
		pr_info("\"%s\"\n", test_boot_command_line);
		failed_tests++;
	}
	total_tests++;

	/* Missing append */
	strcpy(test_boot_command_line, test3_bootloader_args);
	test_tmp[0] = '\0';
	result = __cmdline_add_builtin(test_boot_command_line, test_tmp, test3_prepend, "", COMMAND_LINE_SIZE, CMDLINE_STRLEN, CMDLINE_STRLCAT);

	if (result == true && !strncmp(test_boot_command_line, test3_result, COMMAND_LINE_SIZE)) {
		pr_info("test3 success.\n");
	} else {
		pr_info("test3 failed. OUTPUT BELOW:\n");
		pr_info("\"%s\"\n", test_boot_command_line);
		failed_tests++;
	}
	total_tests++;

	/* Missing append and prepend */
	strcpy(test_boot_command_line, test4_bootloader_args);
	test_tmp[0] = '\0';
	result = __cmdline_add_builtin(test_boot_command_line, test_tmp, "", "", COMMAND_LINE_SIZE, CMDLINE_STRLEN, CMDLINE_STRLCAT);

	if (result == true && !strncmp(test_boot_command_line, test4_result, COMMAND_LINE_SIZE)) {
		pr_info("test4 success.\n");
	} else {
		pr_info("test4 failed. OUTPUT BELOW:\n");
		pr_info("\"%s\"\n", test_boot_command_line);
		failed_tests++;
	}
	total_tests++;

	/* Already full boot arguments */
	strcpy(test5_boot_command_line, test5_bootloader_args);
	test_tmp[0] = '\0';
	result = __cmdline_add_builtin(test5_boot_command_line, test_tmp, test5_prepend, test5_append, 512, CMDLINE_STRLEN, CMDLINE_STRLCAT);

	if (result == false && !strncmp(test5_boot_command_line, test5_result, COMMAND_LINE_SIZE)) {
		pr_info("test5 success.\n");
	} else {
		pr_info("test5 failed. OUTPUT BELOW:\n");
		pr_info("\"%s\"\n", test5_boot_command_line);
		failed_tests++;
	}
	total_tests++;
}

KSTM_MODULE_LOADERS(cmdline_test);
MODULE_AUTHOR("Daniel Walker <danielwa@cisco.com>");
MODULE_LICENSE("GPL");
