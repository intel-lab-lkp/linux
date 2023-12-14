/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_VIRTIO_H
#define __KVM_VIRTIO_H

#ifdef CONFIG_KVM_VIRTIO
int kvm_virtio_ops_init(void);
void kvm_virtio_ops_exit(void);
#else
static inline int kvm_virtio_ops_init(void)
{
	return 0;
}
static inline void kvm_virtio_ops_exit(void)
{
}
#endif

#endif
