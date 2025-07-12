// SPDX-License-Identifier: GPL-2.0

#include <linux/percpu.h>
#include <linux/smp.h>

void __percpu *rust_helper_alloc_percpu(size_t sz, size_t align)
{
	return __alloc_percpu(sz, align);
}

void rust_helper_on_each_cpu(smp_call_func_t func, void *info, int wait)
{
	on_each_cpu(func, info, wait);
}

int rust_helper_smp_processor_id(void)
{
	return smp_processor_id();
}

