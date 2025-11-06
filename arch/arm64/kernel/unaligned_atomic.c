// SPDX-License-Identifier: GPL-2.0-only
/*
 * Unaligned atomic emulation by André Almeida <andrealmeid@igalia.com>
 * Derived from original work by Billy Laws <blaws05@gmail.com>
 */

#include <linux/cacheflush.h>
#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/semaphore.h>
#include <linux/uaccess.h>

#include <asm/alternative-macros.h>
#include <asm/asm-extable.h>
#include <asm/exception.h>
#include <asm/ptrace.h>
#include <asm/traps.h>

#define __LOAD_ACQUIRE_RCPC(sfx, regs...) \
	ALTERNATIVE("ldar" #sfx "\t" #regs,     \
			".arch_extension rcpc\n"    \
			"ldapr" #sfx "\t" #regs,    \
			ARM64_HAS_LDAPR)

struct fault_info {
	int error;
	u64 addr;
	u64 size;
};

#define ATOMIC_MEM_MASK 0x3b200c00
#define ATOMIC_MEM_INST 0x38200000

#define RCPC2_MASK  0x3fe00c00
#define LDAPUR_INST 0x19400000
#define STLUR_INST  0x19000000

#define LDAXR_MASK 0x3ffffc00

#define LDAXP_MASK 0xbfff8000
#define LDAXP_INST 0x887f8000

#define LDAR_INST  0x08dffc00
#define LDAPR_INST 0x38bfc000
#define STLR_INST  0x089ffc00

#define ATOMIC_ADD_OP	0x0
#define ATOMIC_CLR_OP	0x1
#define ATOMIC_EOR_OP	0x2
#define ATOMIC_SET_OP	0x3
#define ATOMIC_SWAP_OP	0x8

#define get_reg(gprs, reg) (reg == 31 ? 0 : gprs[reg])
#define set_reg(gprs, reg, val) do { if (reg != 31) gprs[reg] = val; } while (0)

#define get_addr_reg(instr) ((instr >> 5) & 0b11111)
#define get_size(instr) (1 << (instr >> 30))

static DEFINE_MUTEX(buslock);

static struct fault_info do_load_acquire_128(u64 addr, u128 *result)
{
	u64 lower, upper, orig_addr = addr;
	int ret;

	addr = (u64)__uaccess_mask_ptr((void __user *)addr);

	if (!access_ok((void __user *)orig_addr, 16))
		return (struct fault_info) {.error = -EFAULT, .addr = orig_addr, .size = 16};

	uaccess_enable_privileged();
	asm volatile("1: ldaxp %[resultlower], %[resultupper], [%[addr]]\n"
			"   clrex\n"
			"2:\n"
			_ASM_EXTABLE_UACCESS_ERR(1b, 2b, %w[ret])
			: [resultlower] "=r"(lower), [resultupper] "=r"(upper), [ret] "+r"(ret)
			: [addr] "r"(addr)
			: "memory");
	uaccess_disable_privileged();

	if (ret)
		return (struct fault_info) {.error = ret, .addr = orig_addr, .size = 16};

	*result = (u128)upper << 64 | lower;

	return (struct fault_info) {0};
}

/*
 * Do a load acquire at address `addr` and save it at `result`
 */
static struct fault_info do_load_acquire_64(u64 addr, u64 *result)
{
	u64 orig_addr = addr;
	int ret = 0;

	addr = (u64)__uaccess_mask_ptr((void __user *)addr);

	if (!access_ok((void __user *)orig_addr, 8))
		return (struct fault_info) {.error = -EFAULT, .addr = orig_addr, .size = 8};

	uaccess_enable_privileged();
	asm volatile("1: " __LOAD_ACQUIRE_RCPC(, %[result], [%[addr]]) "\n"
			"2:\n"
			_ASM_EXTABLE_UACCESS_ERR(1b, 2b, %w[ret])
			: [result] "=r"(*result), [ret] "+r"(ret)
			: [addr] "r"(addr)
			: "memory");
	uaccess_disable_privileged();

	return (struct fault_info) {.error = ret, .addr = orig_addr, .size = 8};
}

/*
 * If *expected value is found in addr, swap it for val
 * Otherwise, write the value found at addr at expected address
 */
static struct fault_info do_store_cas_64(u64 *expected, u64 val, u64 addr)
{
	u64 orig_addr = addr, tmp, oldval;
	int ret = 0;

	addr = (u64)__uaccess_mask_ptr((void __user *)addr);

	if (!access_ok((void __user *)orig_addr, 8))
		return (struct fault_info) {.error = -EFAULT, .addr = orig_addr, .size = 8};

	uaccess_enable_privileged();
	asm volatile("1: ldxr %[oldval], [%[addr]]\n"
			"   cmp  %[oldval], %[expected]\n"
			"   b.ne 3f\n"
			"2: stlxr %w[tmp], %[val], [%[addr]]\n"
			"   cbnz %w[tmp], 1b\n"
			"3:\n"
			_ASM_EXTABLE_UACCESS_ERR(1b, 3b, %w[ret])
			_ASM_EXTABLE_UACCESS_ERR(2b, 3b, %w[ret])
			: [tmp] "=&r"(tmp), [oldval] "=&r"(oldval), [ret] "+r"(ret)
			: [addr] "r"(addr), [expected] "r"(*expected), [val] "r"(val)
			: "memory", "cc");
	uaccess_disable_privileged();

	if (ret)
		return (struct fault_info) {.error = ret, .addr = orig_addr, .size = 8};

	if (oldval != *expected) {
		*expected = oldval;
		return (struct fault_info) {.error = -EAGAIN};
	}

	return (struct fault_info) {0};
}

/*
 * If possible, do one 128 bit load. Otherwise, do two 64 bit loads and combine
 * the results.
 */
static struct fault_info do_load_64(u64 addr, u64 *result)
{
	u64 alignment_mask = 0b1111, align_offset = addr & alignment_mask;
	struct fault_info fi = {0};
	u128 tmp;

	/* The address crosses a 16 byte boundary and needs two loads */
	if (align_offset > 8) {
		u64 alignment = addr & 0b111, upper_val, lower_val;

		addr &= ~0b111ULL;

		fi = do_load_acquire_64(addr + 8, &upper_val);
		if (fi.error)
			return fi;

		fi = do_load_acquire_64(addr, &lower_val);
		if (fi.error)
			return fi;

		tmp = ((u128) upper_val << 64) | lower_val;
		*result = tmp >> (alignment * 8);
	} else {
		addr &= ~alignment_mask;

		fi = do_load_acquire_128(addr, &tmp);
		if (fi.error)
			return fi;

		*result = tmp >> (align_offset * 8);
	}

	return fi;
}

static u64 do_atomic_mem_op(u8 op, u64 src_val, u64 desired)
{
	switch (op) {
	case ATOMIC_ADD_OP:
		return src_val + desired;
	case ATOMIC_CLR_OP:
		return src_val & ~desired;
	case ATOMIC_EOR_OP:
		return src_val & desired;
	case ATOMIC_SET_OP:
		return src_val ^ desired;
	case ATOMIC_SWAP_OP:
		return desired;
	default:
		BUG();
	}

	/* Unreachable */
	return 0;
}

static struct fault_info load_cas(u64 desired_src, u64 addr, u8 op, u64 alignment,
				     u128 *aux_desired, u128 *aux_expected,
				     u128 *aux_actual)
{
	u128 tmp_expected, tmp_desired, tmp_actual, mask = ~0ULL, desired, expected, neg_mask;
	u64 addr_upper, load_order_upper, load_order_lower;
	struct fault_info fi;

	mask <<= alignment * 8;
	addr_upper = addr + 8;
	neg_mask = ~mask;

	fi = do_load_acquire_64(addr_upper, &load_order_upper);
	if (fi.error)
		return fi;
	fi = do_load_acquire_64(addr, &load_order_lower);
	if (fi.error)
		return fi;

	tmp_actual = (u128)load_order_upper << 64 | load_order_lower;

	desired = do_atomic_mem_op(op, tmp_actual >> (alignment * 8), desired_src);
	expected = (tmp_actual >> (alignment * 8));

	tmp_expected = tmp_actual;
	tmp_expected &= neg_mask;
	tmp_expected |= expected << (alignment * 8);

	tmp_desired = tmp_expected;
	tmp_desired &= neg_mask;
	tmp_desired |= desired << (alignment * 8);

	*aux_desired = tmp_desired;
	*aux_expected = tmp_expected;
	*aux_actual = tmp_actual;

	return (struct fault_info) {0};
}

/*
 * After CAS failed, check if we need to try again or if we should return error.
 * Returns true if needs to retry.
 */
static bool handle_fail(u128 tmp_expected, u128 tmp_desired, u128 mask,
			u64 *result, bool retry, bool tear,
			u64 alignment)
{
	u128 neg_mask = ~mask,
	     failed_result_our_bits = tmp_expected & mask,
	     failed_result_not_our_bits = tmp_expected & neg_mask,
	     failed_desired_not_our_bits = tmp_desired & neg_mask;
	u64 failed_result = failed_result_our_bits >> (alignment * 8);

	/*
	 * If the bits changed weren't part of our regular CAS, then we retry.
	 */
	if ((failed_result_not_our_bits ^ failed_desired_not_our_bits) != 0)
		return true;

	/*
	 * This happens in the case that between load and CAS that something has
	 * store our desired in to the memory location. This means our CAS fails
	 * because what we wanted to store was already stored.
	 */
	if (retry) {
		/* If we are retrying and tearing then we can't do anything */
		if (tear) {
			*result = failed_result;
			return false;
		} else {
			return true;
		}
	} else {
		/*
		 * With we were called without retry, then we have failed
		 * regardless of tear. CAS failed but handled successfully
		 */
		*result = failed_result;
	}

	return false;
}

/*
 * This instruction reads a 64-bit doubleword from memory, and compares it
 * against the value held in a first register. If the comparison is equal, the
 * value in a second register is written to memory. If the comparison is not
 * equal, the architecture permits writing the value read from the location to
 * memory.
 *
 * To handle an unaligned CAS, the code first loads two 64-bit words. Then, if
 * what is found in the word is the same as the expected value, the code tries
 * to do two 64-bit writes in the address. If both stores works then return
 * success. If only the first store works, then the code is in a "tear" state
 * and returns with the word found in the address.
 *
 * TODO: here we threat every operation as it's a cross cache address,
 * meaning that we need two 64 bit ops to make it work. That works for
 * all cases, but it's slower and unnecessary for the cases that doesn't
 * cross it and can do a single 128 bit operation.
 */
static struct fault_info do_cas_64(bool retry, u64 desired_src, u64 expected_src,
				   u64 addr, u8 op, u64 *result)
{
	u128 tmp_expected, tmp_desired, tmp_actual, mask = ~0ULL;
	u64 alignment = addr & 0b111, addr_upper;
	struct fault_info fi;
	bool tear = false;

	mask <<= alignment * 8;
	addr &= ~0b111ULL;
	addr_upper = addr + 8;

	/*
	 * TODO: take this lock only if we need to emulate a split lock
	 */
	guard(mutex)(&buslock);

retry:
	fi = load_cas(desired_src, addr, op, alignment, &tmp_desired, &tmp_expected, &tmp_actual);
	if (fi.error)
		return fi;

	if (tmp_expected == tmp_actual) {
		u128 expected = (tmp_actual >> (alignment * 8));
		u64 tmp_expected_lower = tmp_expected,
		    tmp_expected_upper = tmp_expected >> 64,
		    tmp_desired_lower = tmp_desired,
		    tmp_desired_upper = tmp_desired >> 64;

		fi = do_store_cas_64(&tmp_expected_upper, tmp_desired_upper, addr_upper);
		if (fi.error && fi.error != -EAGAIN)
			return fi;

		if (fi.error == 0) {
			fi = do_store_cas_64(&tmp_expected_lower, tmp_desired_lower, addr);
			if (fi.error && fi.error != -EAGAIN)
				return fi;

			/* Both store() worked, CAS succeeded */
			if (fi.error == 0) {
				*result = expected;
				return fi;
			/* A partial store() happened, tear state */
			} else {
				tear = true;
			}
		}

		tmp_expected = tmp_expected_upper;
		tmp_expected <<= 64;
		tmp_expected |= tmp_expected_lower;
	} else {
		tmp_expected = tmp_actual;
	}

	if (handle_fail(tmp_expected, tmp_desired, mask, result, retry, tear, alignment))
		goto retry;

	return (struct fault_info) {0};
}

/*
 * For a giving atomic memory operation, parse it and call the desired op
 */
static struct fault_info handle_atomic_mem_op(u32 instr, u64 *gprs)
{
	u32 size = get_size(instr), result_reg = instr & 0b11111,
		   source_reg = (instr >> 16) & 0b11111,
		   addr_reg = get_addr_reg(instr);
	u64 addr = get_reg(gprs, addr_reg);
	u8 op = (instr >> 12) & 0xf;
	struct fault_info fi = {0};

	if (size == 8) {
		u64 res = 0;

		switch (op) {
		case ATOMIC_ADD_OP:
		case ATOMIC_CLR_OP:
		case ATOMIC_EOR_OP:
		case ATOMIC_SET_OP:
		case ATOMIC_SWAP_OP:
			break;
		default:
			pr_warn("Unhandled atomic mem op 0x%02x\n", op);
			return (struct fault_info) {.error = -EINVAL};
		}

		fi = do_cas_64(true, get_reg(gprs, source_reg), 0, addr, op, &res);

		/*
		 * If the operation succeeded and our dest reg is not zero, we
		 * need to update the result reg with what was in memory
		 */
		if (!fi.error)
			set_reg(gprs, result_reg, res);
	} else {
		fi.error = -EINVAL;
	}

	return fi;
}

static struct fault_info handle_atomic_load(u32 instr, u64 *gprs, s64 offset)
{
	u32 size = get_size(instr), result_reg = instr & 0b11111,
		   addr_reg = get_addr_reg(instr);
	u64 addr = get_reg(gprs, addr_reg) + offset, res;

	struct fault_info fi = {0};

	if (size == 8) {
		fi = do_load_64(addr, &res);
		if (!fi.error)
			set_reg(gprs, result_reg, res);
	} else {
		fi.error = -EINVAL;
	}

	return fi;
}

static struct fault_info handle_atomic_store(u32 instr, u64 *gprs, s64 offset)
{
	u32 size = get_size(instr), data_reg = instr & 0x1F, addr_reg = get_addr_reg(instr);
	u64 addr = get_reg(gprs, addr_reg) + offset;
	struct fault_info fi = {0};

	if (size == 8) {
		u64 res;

		fi = do_cas_64(false, get_reg(gprs, data_reg), 0, addr, ATOMIC_SWAP_OP, &res);
	} else {
		fi.error = -EINVAL;
	}

	return fi;
}

static struct fault_info decode_instruction(u32 instr, u64 *gprs)
{
	struct fault_info fi = {0};
	s32 offset = (s32)(instr) << 11 >> 23, size = get_size(instr);

	/* TODO: We only support 64-bit instructions for now */
	if (size != 8)
		goto exit;

	if ((instr & LDAXR_MASK) == LDAR_INST || (instr & LDAXR_MASK) == LDAPR_INST)
		return handle_atomic_load(instr, gprs, 0);

	if ((instr & LDAXR_MASK) == STLR_INST)
		return  handle_atomic_store(instr, gprs, 0);

	if ((instr & RCPC2_MASK) == LDAPUR_INST)
		return handle_atomic_load(instr, gprs, offset);

	if ((instr & RCPC2_MASK) == STLUR_INST)
		return handle_atomic_store(instr, gprs, offset);

	if ((instr & ATOMIC_MEM_MASK) == ATOMIC_MEM_INST)
		return handle_atomic_mem_op(instr, gprs);

	/* TODO: Handle CASAL and CASPAL as well */

exit:
	fi.error = -EINVAL;
	return fi;
}

/*
 * After a fault access happened, move the pointer to the end of the bad section
 */
static void increment_fault_address(u64 **fault_addr, int size)
{
	u64 *addr = *fault_addr;
	u8 tmp;
	int i;

	for (i = 0; i < size - 1 && !get_user(tmp, (u8 __user *)(addr + i)); i++)
		(*fault_addr)++;
}

int do_unaligned_atomic_fixup(struct pt_regs *regs, u64 *fault_addr)
{
	u32 *pc = (u32 *)regs->pc, instr;
	u64 *gprs = (u64 *)regs->regs;
	struct fault_info fi;

	if (get_user(instr, (u32 __user *)(pc)))
		return -EFAULT;

	fi = decode_instruction(instr, gprs);

	if (fi.error) {
		*fault_addr = fi.addr;
		if (fi.error == -EFAULT)
			increment_fault_address(&fault_addr, fi.size);

		return fi.error;
	}

	arm64_skip_faulting_instruction(regs, 4);
	return 0;
}
