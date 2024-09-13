#include <linux/string.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/kmsan-checks.h>
#include <asm/uaccess.h>

#define movs(type,to,from) \
	asm volatile("movs" type:"=&D" (to), "=&S" (from):"0" (to), "1" (from):"memory")

/* Originally from i386/string.h */
static __always_inline void rep_movs(void *to, const void *from, size_t n)
{
	unsigned long d0, d1, d2;
	asm volatile("rep ; movsl\n\t"
		     "testb $2,%b4\n\t"
		     "je 1f\n\t"
		     "movsw\n"
		     "1:\ttestb $1,%b4\n\t"
		     "je 2f\n\t"
		     "movsb\n"
		     "2:"
		     : "=&c" (d0), "=&D" (d1), "=&S" (d2)
		     : "0" (n / 4), "q" (n), "1" ((long)to), "2" ((long)from)
		     : "memory");
}

static void string_memcpy_fromio(void *to, const volatile void __iomem *from, size_t n)
{
	const void *orig_to = to;
	const size_t orig_n = n;

	if (unlikely(!n))
		return;

	/* Align any unaligned source IO */
	if (unlikely(1 & (unsigned long)from)) {
		movs("b", to, from);
		n--;
	}
	if (n > 1 && unlikely(2 & (unsigned long)from)) {
		movs("w", to, from);
		n-=2;
	}
	rep_movs(to, (const void *)from, n);
	/* KMSAN must treat values read from devices as initialized. */
	kmsan_unpoison_memory(orig_to, orig_n);
}

static void string_memcpy_toio(volatile void __iomem *to, const void *from, size_t n)
{
	if (unlikely(!n))
		return;

	/* Make sure uninitialized memory isn't copied to devices. */
	kmsan_check_memory(from, n);
	/* Align any unaligned destination IO */
	if (unlikely(1 & (unsigned long)to)) {
		movs("b", to, from);
		n--;
	}
	if (n > 1 && unlikely(2 & (unsigned long)to)) {
		movs("w", to, from);
		n-=2;
	}
	rep_movs((void *)to, (const void *) from, n);
}

static void unrolled_memcpy_fromio(void *to, const volatile void __iomem *from, size_t n)
{
	const volatile char __iomem *in = from;
	char *out = to;
	int i;

	for (i = 0; i < n; ++i)
		out[i] = readb(&in[i]);
}

static void unrolled_memcpy_toio(volatile void __iomem *to, const void *from, size_t n)
{
	volatile char __iomem *out = to;
	const char *in = from;
	int i;

	for (i = 0; i < n; ++i)
		writeb(in[i], &out[i]);
}

static void unrolled_memset_io(volatile void __iomem *a, int b, size_t c)
{
	volatile char __iomem *mem = a;
	int i;

	for (i = 0; i < c; ++i)
		writeb(b, &mem[i]);
}

void memcpy_fromio(void *to, const volatile void __iomem *from, size_t n)
{
	if (cc_platform_has(CC_ATTR_GUEST_UNROLL_STRING_IO))
		unrolled_memcpy_fromio(to, from, n);
	else
		string_memcpy_fromio(to, from, n);
}
EXPORT_SYMBOL(memcpy_fromio);

void memcpy_toio(volatile void __iomem *to, const void *from, size_t n)
{
	if (cc_platform_has(CC_ATTR_GUEST_UNROLL_STRING_IO))
		unrolled_memcpy_toio(to, from, n);
	else
		string_memcpy_toio(to, from, n);
}
EXPORT_SYMBOL(memcpy_toio);

void memset_io(volatile void __iomem *a, int b, size_t c)
{
	if (cc_platform_has(CC_ATTR_GUEST_UNROLL_STRING_IO)) {
		unrolled_memset_io(a, b, c);
	} else {
		/*
		 * TODO: memset can mangle the IO patterns quite a bit.
		 * perhaps it would be better to use a dumb one:
		 */
		memset((void *)a, b, c);
	}
}
EXPORT_SYMBOL(memset_io);

int __get_iomem(char *src, char *buf, size_t size)
{
	/*
	 * This function uses __get_user() independent of whether kernel or user
	 * memory is accessed. This works fine because __get_user() does no
	 * sanity checks of the pointer being accessed. All that it does is
	 * to report when the access failed.
	 *
	 * The access can't be done via copy_from_user() here because
	 * __get_iomem() must not use string instructions to access unsafe
	 * memory. The reason is that MOVS is emulated by the exception handler
	 * for SEV and TDX by splitting the move up into a read and a write
	 * opetations and taking a nested exception on whatever of them is the
	 * MMIO access. Using string instructions here would cause infinite
	 * nesting.
	 */
	switch (size) {
	case 1: {
		u8 d1, __user *s = (u8 __user *)src;

		if (__get_user(d1, s))
			return -EFAULT;
		memcpy(buf, &d1, 1);
		break;
	}
	case 2: {
		u16 d2, __user *s = (u16 __user *)src;

		if (__get_user(d2, s))
			return -EFAULT;
		memcpy(buf, &d2, 2);
		break;
	}
	case 4: {
		u32 d4, __user *s = (u32 __user *)src;

		if (__get_user(d4, s))
			return -EFAULT;
		memcpy(buf, &d4, 4);
		break;
	}
	case 8: {
		u64 d8, __user *s = (u64 __user *)src;

		if (__get_user(d8, s))
			return -EFAULT;
		memcpy(buf, &d8, 8);
		break;
	}
	default:
		WARN_ONCE(1, "%s: Invalid size: %zu\n", __func__, size);
		return -EIO;
	}

	return 0;
}

int __put_iomem(char *dst, char *buf, size_t size)
{
	/*
	 * This function uses __put_user() independent of whether kernel or user
	 * memory is accessed. This works fine because __put_user() does no
	 * sanity checks of the pointer being accessed. All that it does is
	 * to report when the access failed.
	 *
	 * The access can't be done via copy_to_user() here because
	 * __put_iomem() must not use string instructions to access unsafe
	 * memory. The reason is that MOVS is emulated by the exception handler
	 * for SEV and TDX by splitting the move up into a read and a write
	 * opetations and taking a nested exception on whatever of them is the
	 * MMIO access. Using string instructions here would cause infinite
	 * nesting.
	 */
	switch (size) {
	case 1: {
		u8 d1, __user *target = (u8 __user *)dst;

		memcpy(&d1, buf, 1);
		if (__put_user(d1, target))
			return -EFAULT;
		break;
	}
	case 2: {
		u16 d2, __user *target = (u16 __user *)dst;

		memcpy(&d2, buf, 2);
		if (__put_user(d2, target))
			return -EFAULT;
		break;
	}
	case 4: {
		u32 d4, __user *target = (u32 __user *)dst;

		memcpy(&d4, buf, 4);
		if (__put_user(d4, target))
			return -EFAULT;
		break;
	}
	case 8: {
		u64 d8, __user *target = (u64 __user *)dst;

		memcpy(&d8, buf, 8);
		if (__put_user(d8, target))
			return -EFAULT;
		break;
	}
	default:
		WARN_ONCE(1, "%s: Invalid size: %zu\n", __func__, size);
		return -EIO;
	}

	return 0;
}
