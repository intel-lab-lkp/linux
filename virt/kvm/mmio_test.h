/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_MMIO_TEST_H
#define __KVM_MMIO_TEST_H

#ifdef CONFIG_KVM_MMIO_TEST
int kvm_mmio_test_ops_init(void);
void kvm_mmio_test_ops_exit(void);
#else
static inline int kvm_mmio_test_ops_init(void)
{
	return 0;
}
static inline void kvm_mmio_test_ops_exit(void)
{
}
#endif

#endif
