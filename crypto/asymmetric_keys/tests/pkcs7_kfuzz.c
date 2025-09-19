// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PKCS#7 parser KFuzzTest target
 *
 * Copyright 2025 Google LLC
 */
#include <crypto/pkcs7.h>
#include <linux/kfuzztest.h>

struct pkcs7_parse_message_arg {
	const void *data;
	size_t datalen;
};

FUZZ_TEST(test_pkcs7_parse_message, struct pkcs7_parse_message_arg)
{
	struct pkcs7_message *msg;

	KFUZZTEST_EXPECT_NOT_NULL(pkcs7_parse_message_arg, data);
	KFUZZTEST_ANNOTATE_ARRAY(pkcs7_parse_message_arg, data);
	KFUZZTEST_ANNOTATE_LEN(pkcs7_parse_message_arg, datalen, data);

	msg = pkcs7_parse_message(arg->data, arg->datalen);
	if (msg && !IS_ERR(msg))
		kfree(msg);
}
