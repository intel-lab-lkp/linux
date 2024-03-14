/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SELFTEST_KVM_UTIL_TYPES_H
#define SELFTEST_KVM_UTIL_TYPES_H

/*
 * Provide a version of static_assert() that is guaranteed to have an optional
 * message param.  If _ISOC11_SOURCE is defined, glibc (/usr/include/assert.h)
 * #undefs and #defines static_assert() as a direct alias to _Static_assert(),
 * i.e. effectively makes the message mandatory.  Many KVM selftests #define
 * _GNU_SOURCE for various reasons, and _GNU_SOURCE implies _ISOC11_SOURCE.  As
 * a result, static_assert() behavior is non-deterministic and may or may not
 * require a message depending on #include order.
 */
#define __kvm_static_assert(expr, msg, ...) _Static_assert(expr, msg)
#define kvm_static_assert(expr, ...) __kvm_static_assert(expr, ##__VA_ARGS__, #expr)

typedef uint64_t vm_paddr_t; /* Virtual Machine (Guest) physical address */
typedef uint64_t vm_vaddr_t; /* Virtual Machine (Guest) virtual address */

#endif /* SELFTEST_KVM_UTIL_TYPES_H */
