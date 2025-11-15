// SPDX-License-Identifier: GPL-2.0

#include <asm/barrier.h>

void rust_helper_smp_mb(void)
{
	smp_mb();
}

void rust_helper_smp_wmb(void)
{
	smp_wmb();
}

void rust_helper_smp_rmb(void)
{
	smp_rmb();
}

void rust_helper_smp_store_release(void *ptr, u64 val, size_t size)
{
	switch (size) {
	case 1:
		smp_store_release((u8 *)ptr, (u8)val);
		break;
	case 2:
		smp_store_release((u16 *)ptr, (u16)val);
		break;
	}
}

void rust_helper_smp_load_acquire(void *ptr, void *val, size_t size)
{
	switch (size) {
	case 1:
		*(u8 *)val = smp_load_acquire((u8 *)ptr);
		break;
	case 2:
		*(u16 *)val = smp_load_acquire((u16 *)ptr);
		break;
	}
}
