// SPDX-License-Identifier: GPL-2.0
/*
 * The Kernel Fuzz Testing Framework (KFuzzTest) API for defining fuzz targets
 * for internal kernel functions.
 *
 * Copyright 2025 Google LLC
 */
#ifndef KFUZZTEST_H
#define KFUZZTEST_H

#include <linux/fs.h>
#include <linux/printk.h>
#include <linux/types.h>

#define KFUZZTEST_MAX_INPUT_SIZE (PAGE_SIZE * 16)

/* Common code for receiving inputs from userspace. */
int kfuzztest_write_cb_common(struct file *filp, const char __user *buf, size_t len, loff_t *off, void **test_buffer);

struct kfuzztest_simple_target {
	const char *name;
	ssize_t (*write_input_cb)(struct file *filp, const char __user *buf, size_t len, loff_t *off);
};

/**
 * FUZZ_TEST_SIMPLE - defines a KFuzzTest target
 *
 * @test_name: the unique identifier for the fuzz test, which is used to name
 *             the debugfs entry.
 *
 * This macro defines a fuzz target entry point that accepts raw byte buffers
 * from userspace. It registers a struct kfuzztest_simple_target which the
 * framework exposes via debugfs.
 *
 * When userspace writes to the corresponding debugfs file, the framework
 * allocates a kernel buffer, copies the user data, and passes it to the
 * logic defined in the macro body.
 *
 * User-provided Logic:
 * The developer must provide the body of the fuzz test logic within the curly
 * braces following the macro invocation. Within this scope, the framework
 * implicitly defines the following variables:
 *
 * - `char *data`: A pointer to the raw input data.
 * - `size_t datalen`: The length of the input data.
 *
 * Example Usage:
 *
 * // 1. The kernel function that we want to fuzz.
 * int process_data(const char *data, size_t datalen);
 *
 * // 2. Define a fuzz target using the FUZZ_TEST_SIMPLE macro.
 * FUZZ_TEST_SIMPLE(test_process_data)
 * {
 *	// Call the function under test using the `data` and `datalen`
 *	// variables.
 *	process_data(data, datalen);
 * }
 *
 */
#define FUZZ_TEST_SIMPLE(test_name)											\
	static ssize_t kfuzztest_simple_write_cb_##test_name(struct file *filp, const char __user *buf, size_t len,	\
							     loff_t *off);						\
	static ssize_t kfuzztest_simple_logic_##test_name(char *data, size_t datalen);					\
	static const struct kfuzztest_simple_target __fuzz_test_simple__##test_name __section(				\
		".kfuzztest_simple_target") __used = {									\
		.name = #test_name,											\
		.write_input_cb = kfuzztest_simple_write_cb_##test_name,						\
	};														\
	static ssize_t kfuzztest_simple_write_cb_##test_name(struct file *filp, const char __user *buf, size_t len,	\
							     loff_t *off)						\
	{														\
		void *buffer;												\
		int ret;												\
															\
		ret = kfuzztest_write_cb_common(filp, buf, len, off, &buffer);						\
		if (ret < 0)												\
			goto out;											\
		ret = kfuzztest_simple_logic_##test_name(buffer, len);							\
		if (ret == 0)												\
			ret = len;											\
		kfree(buffer);												\
out:															\
		return ret;												\
	}														\
	static ssize_t kfuzztest_simple_logic_##test_name(char *data, size_t datalen)

#endif /* KFUZZTEST_H */
