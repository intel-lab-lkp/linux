/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ASM_VDSO_GETRANDOM_H
#define __ASM_VDSO_GETRANDOM_H

#ifndef __ASSEMBLY__

#include <asm/unistd.h>
#include <vdso/datapage.h>

/**
 * getrandom_syscall - Invoke the getrandom() syscall.
 * @buffer:	Destination buffer to fill with random bytes.
 * @len:	Size of @buffer in bytes.
 * @flags:	Zero or more GRND_* flags.
 * Returns:	The number of random bytes written to @buffer, or a negative value indicating an error.
 */
static __always_inline ssize_t getrandom_syscall(void *buffer, size_t len, unsigned int flags)
{
	register long int x8 asm ("x8") = __NR_getrandom;
	register long int x0 asm ("x0") = (long int) buffer;
	register long int x1 asm ("x1") = (long int) len;
	register long int x2 asm ("x2") = (long int) flags;

	asm ("svc 0" : "=r"(x0) : "r"(x8), "0"(x0), "r"(x1), "r"(x2));

	return x0;
}

static __always_inline const struct vdso_rng_data *__arch_get_vdso_rng_data(void)
{
	return &_vdso_rng_data;
}

/**
 * __arch_chacha20_blocks_nostack - Generate ChaCha20 stream without using the stack.
 * @dst_bytes:	Destination buffer to hold @nblocks * 64 bytes of output.
 * @key:	32-byte input key.
 * @counter:	8-byte counter, read on input and updated on return.
 * @nblocks:	Number of blocks to generate.
 *
 * Generates a given positive number of blocks of ChaCha20 output with nonce=0, and does not write
 * to any stack or memory outside of the parameters passed to it, in order to mitigate stack data
 * leaking into forked child processes.
 */
extern void __arch_chacha20_blocks_nostack(u8 *dst_bytes, const u32 *key, u32 *counter, size_t nblocks);

#endif /* !__ASSEMBLY__ */

#endif /* __ASM_VDSO_GETRANDOM_H */
