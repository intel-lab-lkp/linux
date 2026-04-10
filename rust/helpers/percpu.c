// SPDX-License-Identifier: GPL-2.0

#include <linux/percpu.h>

__rust_helper
void __percpu *rust_helper_alloc_percpu(size_t sz, size_t align)
{
	return __alloc_percpu(sz, align);
}

__rust_helper
void *rust_helper_per_cpu_ptr(void __percpu *ptr, unsigned int cpu)
{
	return per_cpu_ptr(ptr, cpu);
}

__rust_helper
void rust_helper_on_each_cpu(smp_call_func_t func, void *info, int wait)
{
	on_each_cpu(func, info, wait);
}
