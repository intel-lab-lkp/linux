.. SPDX-License-Identifier: GPL-2.0

===================================
The LoongArch paravirtual interface
===================================

KVM hypercalls use the HVCL instruction with code 0x100, and the hypercall
number is put in a0 and up to five arguments may be placed in a1-a5, the
return value is placed in a0.

The code for that interface can be found in arch/loongarch/kvm/*

Querying for existence
======================

To find out if we're running on KVM or not, cpucfg can be used with index
CPUCFG_KVM_BASE (0x40000000), cpucfg range between 0x40000000 - 0x400000FF
is marked as a specially reserved range. All existing and future processors
will not implement any features in this range.

When Linux is running on KVM, cpucfg with index CPUCFG_KVM_BASE (0x40000000)
returns magic string "KVM\0"

Once you determined you're running under a PV capable KVM, you can now use
hypercalls described as follows.

KVM hypercall ABI
=================

Hypercall ABI on KVM is simple, only one scratch register a0 and at most
five generic registers used as input parameter. FP register and vector register
is not used for input register and should not be modified during hypercall.
Hypercall function can be inlined since there is only one scratch register.

The parameters are as follows:

        ========	=========================	=====================
	Register	IN			        OUT
        ========	=========================	=====================
	a0		function number(Required)       Return code(Required)
	a1		1st parameter(Optional)		Keep Unmodified
	a2		2nd parameter(Optional)		Keep Unmodified
	a3		3rd parameter(Optional)		Keep Unmodified
	a4		4th parameter(Optional)		Keep Unmodified
	a5		5th parameter(Optional)		Keep Unmodified
        ========	=========================	=====================

Return codes can be as follows:

	====		=========================
	Code		Meaning
	====		=========================
	0		Success
	-1		Hypercall not implemented
	-2		Hypercall parameter error
	====		=========================

KVM Hypercalls Documentation
============================

The template for each hypercall is:
1. Hypercall name
2. Purpose

1. KVM_HCALL_FUNC_PV_IPI
------------------------

:Purpose: Send IPIs to multiple vCPUs.

- a0: KVM_HCALL_FUNC_PV_IPI
- a1: lower part of the bitmap of destination physical cpu core id
- a2: higher part of the bitmap of destination physical cpu core id
- a3: the lowest physical cpu core id in bitmap

The hypercall lets a guest send multicast IPIs, with at most 128
destinations per hypercall.  The destinations are represented by a bitmap
contained in the first two arguments (a1 and a2). Bit 0 of a1 corresponds
to the physical cpu core id in the third argument (a3), bit 1 corresponds
to the physical cpu core id a3+1, and so on.

Returns success always, it skips current cpu and continues to send ipi to
next cpu in the bitmap mask if current physical cpu core id is invalid.
