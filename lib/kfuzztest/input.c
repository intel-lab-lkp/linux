/* SPDX-License-Identifier: GPL-2.0 */
/*
 * KFuzzTest input handling.
 *
 * Copyright 2025 Google LLC
 */
#include <linux/kfuzztest.h>

int kfuzztest_write_cb_common(struct file *filp, const char __user *buf, size_t len, loff_t *off, void **test_buffer)
{
	void *buffer;
	ssize_t ret;

	/*
	 * Enforce a zero-offset to ensure that all data is passed down in a
	 * single contiguous blob and not fragmented across multiple write
	 * system calls.
	 */
	if (*off)
		return -EINVAL;

	/*
	 * Taint the kernel on the first fuzzing invocation. The debugfs
	 * interface provides a high-risk entry point for userspace to
	 * call kernel functions with untrusted input.
	 */
	if (!test_taint(TAINT_TEST))
		add_taint(TAINT_TEST, LOCKDEP_STILL_OK);

	if (len > KFUZZTEST_MAX_INPUT_SIZE) {
		pr_warn("kfuzztest: user input of size %zu is too large", len);
		return -EINVAL;
	}

	buffer = kzalloc(len, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	ret = simple_write_to_buffer(buffer, len, off, buf, len);
	if (ret != len) {
		kfree(buffer);
		return -EFAULT;
	}

	*test_buffer = buffer;
	return 0;
}
