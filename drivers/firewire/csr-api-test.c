// SPDX-License-Identifier: GPL-2.0-only
//
// csr-api-test.c - An application of Kunit to test implementation for CSR operation.
//
// Copyright (c) 2023 Takashi Sakamoto
//
// This file can not be built independently since it is intentionally included in core-device.c.

#include <kunit/test.h>

static struct kunit_case csr_api_test_cases[] = {
	{}
};

static struct kunit_suite csr_api_test_suite = {
	.name = "firewire-csr-api",
	.test_cases = csr_api_test_cases,
};
kunit_test_suite(csr_api_test_suite);
