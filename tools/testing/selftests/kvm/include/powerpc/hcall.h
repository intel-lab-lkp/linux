/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * powerpc hcall defines
 */
#ifndef SELFTEST_KVM_HCALL_H
#define SELFTEST_KVM_HCALL_H

#include <linux/compiler.h>

/* Ucalls use unimplemented PAPR hcall 0 which exits KVM */
#define H_UCALL	0

int64_t hcall0(uint64_t token);
int64_t hcall1(uint64_t token, uint64_t arg1);
int64_t hcall2(uint64_t token, uint64_t arg1, uint64_t arg2);

#endif
