// SPDX-License-Identifier: GPL-2.0
/*
 * atomic32.c: 32-bit atomic_t implementation
 *
 * Copyright (C) 2004 Keith M Wesolowski
 * Copyright (C) 2007 Kyle McMartin
 * 
 * Based on asm-parisc/atomic.h Copyright (C) 2000 Philipp Rumpf
 */

#include <linux/atomic.h>
#include <linux/futex.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <asm/cpu_type.h>
#include <asm/futex.h>

#ifdef CONFIG_SMP
#define ATOMIC_HASH_SIZE	4
#define ATOMIC_HASH(a)	(&__atomic_hash[(((unsigned long)a)>>8) & (ATOMIC_HASH_SIZE-1)])

spinlock_t __atomic_hash[ATOMIC_HASH_SIZE] = {
	[0 ... (ATOMIC_HASH_SIZE-1)] = __SPIN_LOCK_UNLOCKED(__atomic_hash)
};

#else /* SMP */

static DEFINE_SPINLOCK(dummy);
#define ATOMIC_HASH_SIZE	1
#define ATOMIC_HASH(a)		(&dummy)

#endif /* SMP */

#define ATOMIC_FETCH_OP(op, c_op)					\
int arch_atomic_fetch_##op(int i, atomic_t *v)				\
{									\
	int ret;							\
	unsigned long flags;						\
	spin_lock_irqsave(ATOMIC_HASH(v), flags);			\
									\
	ret = v->counter;						\
	v->counter c_op i;						\
									\
	spin_unlock_irqrestore(ATOMIC_HASH(v), flags);			\
	return ret;							\
}									\
EXPORT_SYMBOL(arch_atomic_fetch_##op);

#define ATOMIC_OP_RETURN(op, c_op)					\
int arch_atomic_##op##_return(int i, atomic_t *v)			\
{									\
	int ret;							\
	unsigned long flags;						\
	spin_lock_irqsave(ATOMIC_HASH(v), flags);			\
									\
	ret = (v->counter c_op i);					\
									\
	spin_unlock_irqrestore(ATOMIC_HASH(v), flags);			\
	return ret;							\
}									\
EXPORT_SYMBOL(arch_atomic_##op##_return);

ATOMIC_OP_RETURN(add, +=)

ATOMIC_FETCH_OP(add, +=)
ATOMIC_FETCH_OP(and, &=)
ATOMIC_FETCH_OP(or, |=)
ATOMIC_FETCH_OP(xor, ^=)

#undef ATOMIC_FETCH_OP
#undef ATOMIC_OP_RETURN

int arch_atomic_xchg(atomic_t *v, int new)
{
	int ret;
	unsigned long flags;

	spin_lock_irqsave(ATOMIC_HASH(v), flags);
	ret = v->counter;
	v->counter = new;
	spin_unlock_irqrestore(ATOMIC_HASH(v), flags);
	return ret;
}
EXPORT_SYMBOL(arch_atomic_xchg);

int arch_atomic_cmpxchg(atomic_t *v, int old, int new)
{
	int ret;
	unsigned long flags;

	spin_lock_irqsave(ATOMIC_HASH(v), flags);
	ret = v->counter;
	if (likely(ret == old))
		v->counter = new;

	spin_unlock_irqrestore(ATOMIC_HASH(v), flags);
	return ret;
}
EXPORT_SYMBOL(arch_atomic_cmpxchg);

int arch_atomic_fetch_add_unless(atomic_t *v, int a, int u)
{
	int ret;
	unsigned long flags;

	spin_lock_irqsave(ATOMIC_HASH(v), flags);
	ret = v->counter;
	if (ret != u)
		v->counter += a;
	spin_unlock_irqrestore(ATOMIC_HASH(v), flags);
	return ret;
}
EXPORT_SYMBOL(arch_atomic_fetch_add_unless);

/* Atomic operations are already serializing */
void arch_atomic_set(atomic_t *v, int i)
{
	unsigned long flags;

	spin_lock_irqsave(ATOMIC_HASH(v), flags);
	v->counter = i;
	spin_unlock_irqrestore(ATOMIC_HASH(v), flags);
}
EXPORT_SYMBOL(arch_atomic_set);

unsigned long sp32___set_bit(unsigned long *addr, unsigned long mask)
{
	unsigned long old, flags;

	spin_lock_irqsave(ATOMIC_HASH(addr), flags);
	old = *addr;
	*addr = old | mask;
	spin_unlock_irqrestore(ATOMIC_HASH(addr), flags);

	return old & mask;
}
EXPORT_SYMBOL(sp32___set_bit);

unsigned long sp32___clear_bit(unsigned long *addr, unsigned long mask)
{
	unsigned long old, flags;

	spin_lock_irqsave(ATOMIC_HASH(addr), flags);
	old = *addr;
	*addr = old & ~mask;
	spin_unlock_irqrestore(ATOMIC_HASH(addr), flags);

	return old & mask;
}
EXPORT_SYMBOL(sp32___clear_bit);

unsigned long sp32___change_bit(unsigned long *addr, unsigned long mask)
{
	unsigned long old, flags;

	spin_lock_irqsave(ATOMIC_HASH(addr), flags);
	old = *addr;
	*addr = old ^ mask;
	spin_unlock_irqrestore(ATOMIC_HASH(addr), flags);

	return old & mask;
}
EXPORT_SYMBOL(sp32___change_bit);

#define CMPXCHG(T)						\
	T __cmpxchg_##T(volatile T *ptr, T old, T new)		\
	{							\
		unsigned long flags;				\
		T prev;						\
								\
		spin_lock_irqsave(ATOMIC_HASH(ptr), flags);	\
		if ((prev = *ptr) == old)			\
			*ptr = new;				\
		spin_unlock_irqrestore(ATOMIC_HASH(ptr), flags);\
								\
		return prev;					\
	}

CMPXCHG(u8)
CMPXCHG(u16)
CMPXCHG(u32)
CMPXCHG(u64)
EXPORT_SYMBOL(__cmpxchg_u8);
EXPORT_SYMBOL(__cmpxchg_u16);
EXPORT_SYMBOL(__cmpxchg_u32);
EXPORT_SYMBOL(__cmpxchg_u64);

unsigned long __xchg_u32(volatile u32 *ptr, u32 new)
{
	unsigned long flags;
	u32 prev;

	spin_lock_irqsave(ATOMIC_HASH(ptr), flags);
	prev = *ptr;
	*ptr = new;
	spin_unlock_irqrestore(ATOMIC_HASH(ptr), flags);

	return (unsigned long)prev;
}
EXPORT_SYMBOL(__xchg_u32);

/* Only some LEON parts implement casa. */
static bool sparc32_casa __ro_after_init;

static int __casa_user(u32 *uval, u32 __user *uaddr, u32 oldval, u32 newval)
{
	/*
	 * casa [%o0] 0xb, %o1, %o2, as a word any assembler accepts. ASI
	 * 0x0b is the supervisor address space; the emulated casa in
	 * cas_emu_32.c instead accepts the user ASIs 0x0a and 0x80.
	 */
	register u32 __user *uaddr_r asm("o0") = uaddr;
	register u32 oldval_r asm("o1") = oldval;
	register u32 newval_r asm("o2") = newval;
	int ret = 0;

	__asm__ __volatile__(
	"1:	.word	0xd5e20169\n"
	"2:\n"
	"	.section .fixup,#alloc,#execinstr\n"
	"	.align	4\n"
	"3:	sethi	%%hi(2b), %0\n"
	"	jmpl	%0 + %%lo(2b), %%g0\n"
	"	 mov	%4, %0\n"
	"	.previous\n"
	"	.section __ex_table,#alloc\n"
	"	.align	4\n"
	"	.word	1b, 3b\n"
	"	.previous\n"
	: "+r" (ret), "+r" (newval_r)
	: "r" (uaddr_r), "r" (oldval_r), "i" (-EFAULT)
	: "memory");

	*uval = newval_r;
	return ret;
}

/* Without casa the illegal_instruction trap takes the fixup above. */
static int __init sparc32_casa_probe(void)
{
	u32 word = 0, val;

	if (sparc_cpu_model == sparc_leon)
		sparc32_casa = !__casa_user(&val, (u32 __force __user *)&word,
					    0, 1);
	return 0;
}
early_initcall(sparc32_casa_probe);

/*
 * Without casa, user CAS is emulated under the same lock. ATOMIC_HASH()
 * uses page offset bits only, so every mapping of a word takes one lock.
 */
int futex_atomic_cmpxchg_inatomic(u32 *uval, u32 __user *uaddr,
				  u32 oldval, u32 newval)
{
	unsigned long flags;
	u32 val;
	int ret;

	if (!access_ok(uaddr, sizeof(u32)))
		return -EFAULT;
	if (sparc32_casa)
		return __casa_user(uval, uaddr, oldval, newval);

	/* Every caller already runs with page faults disabled. */
	spin_lock_irqsave(ATOMIC_HASH(uaddr), flags);
	ret = __get_user(val, uaddr);
	if (!ret && val == oldval)
		ret = __put_user(newval, uaddr);
	spin_unlock_irqrestore(ATOMIC_HASH(uaddr), flags);

	if (!ret)
		*uval = val;
	return ret;
}

int arch_futex_atomic_op_inuser(int op, u32 oparg, int *oval, u32 __user *uaddr)
{
	u32 oldval, newval, val;
	int ret;

	if (get_user(oldval, uaddr))
		return -EFAULT;

	for (;;) {
		switch (op) {
		case FUTEX_OP_SET:
			newval = oparg;
			break;
		case FUTEX_OP_ADD:
			newval = oldval + oparg;
			break;
		case FUTEX_OP_OR:
			newval = oldval | oparg;
			break;
		case FUTEX_OP_ANDN:
			newval = oldval & ~oparg;
			break;
		case FUTEX_OP_XOR:
			newval = oldval ^ oparg;
			break;
		default:
			return -ENOSYS;
		}

		ret = futex_atomic_cmpxchg_inatomic(&val, uaddr, oldval, newval);
		if (ret)
			return ret;
		if (val == oldval)
			break;
		oldval = val;
	}

	*oval = oldval;
	return 0;
}
