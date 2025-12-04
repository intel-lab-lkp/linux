// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PKCS#7 parser KFuzzTest target
 *
 * Copyright 2025 Google LLC
 */
#include <crypto/pkcs7.h>
#include <linux/kfuzztest.h>

FUZZ_TEST_SIMPLE(test_pkcs7_parse_message)
{
	struct pkcs7_message *msg;

	msg = pkcs7_parse_message(data, datalen);
	if (msg && !IS_ERR(msg))
		kfree(msg);
}
