// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2026 ARM Ltd.
 */

#include <linux/kvm_host.h>

#include <asm/virt.h>

void kvm_init_rmi(void)
{
	if (kvm_get_mode() != KVM_MODE_RMM)
		return;

	/* TODO: Check if the RMI is available */

	/* Future patch will enable static branch kvm_rmi_is_available */
}
