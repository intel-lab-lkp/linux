// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * RSA key extract helper KFuzzTest targets
 *
 * Copyright 2025 Google LLC
 */
#include <linux/kfuzztest.h>
#include <crypto/internal/rsa.h>

FUZZ_TEST_SIMPLE(test_rsa_parse_pub_key)
{
	struct rsa_key out;
	rsa_parse_pub_key(&out, data, datalen);
}

FUZZ_TEST_SIMPLE(test_rsa_parse_priv_key)
{
	struct rsa_key out;
	rsa_parse_priv_key(&out, data, datalen);
}
