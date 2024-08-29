// SPDX-License-Identifier: GPL-2.0

typeof(__cvdso_getrandom) __kernel_getrandom;

ssize_t __kernel_getrandom(void *buffer, size_t len, unsigned int flags, void *opaque_state, size_t opaque_len)
{
	asm goto (
	ALTERNATIVE("b %[fallback]", "nop", RM64_HAS_FPSIMD) : : : : fallback);
	return __cvdso_getrandom(buffer, len, flags, opaque_state, opaque_len);

fallback:
	if (unlikely(opaque_len == ~0UL && !buffer && !len && !flags))
		return -ENOSYS;
	return getrandom_syscall(buffer, len, flags);
}
