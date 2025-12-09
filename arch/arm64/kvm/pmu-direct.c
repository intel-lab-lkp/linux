// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Colton Lewis <coltonlewis@google.com>
 */

#include <linux/kvm_host.h>

#include <asm/kvm_pmu.h>

/**
 * kvm_pmu_partition_supported() - Determine if partitioning is possible
 *
 * Partitioning is only supported in VHE mode with PMUv3
 *
 * Return: True if partitioning is possible, false otherwise
 */
bool kvm_pmu_partition_supported(void)
{
	return has_vhe() &&
		system_supports_pmuv3();
}
