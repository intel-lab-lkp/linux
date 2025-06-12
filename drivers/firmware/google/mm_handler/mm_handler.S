/* SPDX-License-Identifier: GPL-2.0 */
/*
 *	Payload owned SMI handler that is placed in SMRAM (mm_loader.c) and called
 *	by coreboot's SMI handler
 *
 *	Also the general comment in arch/x86/realmode/rm/trampoline_64.S is relevant
 *	here as well.
 */

#include <linux/linkage.h>
#include <asm/pgtable_types.h>
#include <asm/page_types.h>
#include <asm/msr.h>
#include <asm/segment.h>
#include <asm/processor-flags.h>
#include <asm/realmode.h>

#include "mm_handler.h"

	.section ".text32","ax"
	.code32
	.balign 4
SYM_CODE_START(mm_entry_32)
	mov	$0x3f8, %dx
	mov	$'L', %al
	out	%al, (%dx)
	mov	$'i', %al
	out	%al, (%dx)
	mov	$'n', %al
	out	%al, (%dx)
	mov	$'u', %al
	out	%al, (%dx)
	mov	$'x', %al
	out	%al, (%dx)
	mov	$' ', %al
	out	%al, (%dx)
	mov	$'S', %al
	out	%al, (%dx)
	mov	$'M', %al
	out	%al, (%dx)
	mov	$'I', %al
	out	%al, (%dx)
	mov	$' ', %al
	out	%al, (%dx)
	mov	$'h', %al
	out	%al, (%dx)
	mov	$'a', %al
	out	%al, (%dx)
	mov	$'n', %al
	out	%al, (%dx)
	mov	$'d', %al
	out	%al, (%dx)
	mov	$'l', %al
	out	%al, (%dx)
	mov	$'e', %al
	out	%al, (%dx)
	mov	$'r', %al
	out	%al, (%dx)
	mov	$' ', %al
	out	%al, (%dx)
	mov	$'s', %al
	out	%al, (%dx)
	mov	$'t', %al
	out	%al, (%dx)
	mov	$'a', %al
	out	%al, (%dx)
	mov	$'r', %al
	out	%al, (%dx)
	mov	$'t', %al
	out	%al, (%dx)
	mov	$'i', %al
	out	%al, (%dx)
	mov	$'n', %al
	out	%al, (%dx)
	mov	$'g', %al
	out	%al, (%dx)
	mov	$'\n', %al
	out	%al, (%dx)

	push	%esp
	// ebx, esi, edi and ebp are going to be preserved
	// (see comment in smm_stub.S if target is x86_64)
	push	%ebx
	push	%esi
	push	%edi
	push	%ebp
	push	%eax

	/*
	 * Switch-case to jump to appropriate section for given functionality.
	 * Macros are defined in mm_handler.h.
	 * Short explaination of where does the magic n(%esp) came from.
	 * Calling mm_entry_32 from coreboot pushes lb_entry_context
	 * (see include/payload_mm_interface.h) to stack and
	 * increments esp by 4, so now our stack looks like this:
	 * | third arg |
	 * | second arg |
	 * | first arg |
	 * | return address |
	 * | stack pointer | <- esp
	 * Then we push all the registers (see above) and hence our stack looks like this now:
	 * | third arg |
	 * | second arg |
	 * | first arg |
	 * | return address |
	 * | esp |
	 * | ebx |
	 * | esi |
	 * | edi |
	 * | ebp |
	 * | eax |
	 * | stack pointer | <- esp
	 * So, now to get the entry we need, we do (9 * 4)(%esp) to get third
	 * argument (ACPI base address), (8 * 4)(%esp) to get second argument
	 * (PM1_CNT byte), and (7 * 4) to get the command.
	 */

	cmpl $MM_ACPI_ENABLE, 28(%esp)
	je acpi_enable32

	cmpl $MM_ACPI_DISABLE, 28(%esp)
	je acpi_disable32

	cmpl $MM_STORE, 28(%esp)
	je mm_store32

	jmp restore_cb_state

acpi_enable32:
	mov	$0x3f8, %dx
	mov	$'E', %al
	out	%al, (%dx)
	mov	$'n', %al
	out	%al, (%dx)
	mov	$'a', %al
	out	%al, (%dx)
	mov	$'b', %al
	out	%al, (%dx)
	mov	$'l', %al
	out	%al, (%dx)
	mov	$'i', %al
	out	%al, (%dx)
	mov	$'n', %al
	out	%al, (%dx)
	mov	$'g', %al
	out	%al, (%dx)
	mov	$' ', %al
	out	%al, (%dx)
	mov	$'A', %al
	out	%al, (%dx)
	mov	$'C', %al
	out	%al, (%dx)
	mov	$'P', %al
	out	%al, (%dx)
	mov	$'I', %al
	out	%al, (%dx)
	mov	$'\n', %al
	out	%al, (%dx)

	// PM1_CNT & ~SCI_EN
	mov	32(%esp), %ax
	add	$MM_ACPI_ENABLE, %ax

	// ACPI_BASE_ADDR
	mov	36(%esp), %dx

	out	%ax, %dx

	jmp	restore_cb_state

acpi_disable32:
	mov	$0x3f8, %dx
	mov	$'D', %al
	out	%al, (%dx)
	mov	$'i', %al
	out	%al, (%dx)
	mov	$'s', %al
	out	%al, (%dx)
	mov	$'a', %al
	out	%al, (%dx)
	mov	$'b', %al
	out	%al, (%dx)
	mov	$'l', %al
	out	%al, (%dx)
	mov	$'i', %al
	out	%al, (%dx)
	mov	$'n', %al
	out	%al, (%dx)
	mov	$'g', %al
	out	%al, (%dx)
	mov	$' ', %al
	out	%al, (%dx)
	mov	$'A', %al
	out	%al, (%dx)
	mov	$'C', %al
	out	%al, (%dx)
	mov	$'P', %al
	out	%al, (%dx)
	mov	$'I', %al
	out	%al, (%dx)
	mov	$'\n', %al
	out	%al, (%dx)

	// PM1_CNT | SCI_EN
	mov	32(%esp), %ax
	add	$MM_ACPI_DISABLE, %ax

	// ACPI_BASE_ADDR
	mov	36(%esp), %dx

	out	%ax, %dx

	jmp	restore_cb_state

mm_store32:
	// Not implemented yet. Probably would be better to do that in C.

restore_cb_state:
	mov	$0x3f8, %dx
	mov	$'M', %al
	out	%al, (%dx)
	mov	$'M', %al
	out	%al, (%dx)
	mov	$' ', %al
	out	%al, (%dx)
	mov	$'r', %al
	out	%al, (%dx)
	mov	$'e', %al
	out	%al, (%dx)
	mov	$'t', %al
	out	%al, (%dx)
	mov	$'u', %al
	out	%al, (%dx)
	mov	$'r', %al
	out	%al, (%dx)
	mov	$'n', %al
	out	%al, (%dx)
	mov	$'s', %al
	out	%al, (%dx)
	mov	$'\n', %al
	out	%al, (%dx)

	pop	%eax
	pop	%ebp
	pop	%edi
	pop	%esi
	pop	%ebx
	pop	%esp

	ret
SYM_CODE_END(mm_entry_32)


	.section ".text64","ax"
	.code64
	.balign 4
SYM_CODE_START(mm_entry_64)
	mov	$0x3f8, %dx
	mov	$'L', %al
	out	%al, (%dx)
	mov	$'i', %al
	out	%al, (%dx)
	mov	$'n', %al
	out	%al, (%dx)
	mov	$'u', %al
	out	%al, (%dx)
	mov	$'x', %al
	out	%al, (%dx)
	mov	$' ', %al
	out	%al, (%dx)
	mov	$'S', %al
	out	%al, (%dx)
	mov	$'M', %al
	out	%al, (%dx)
	mov	$'I', %al
	out	%al, (%dx)
	mov	$' ', %al
	out	%al, (%dx)
	mov	$'h', %al
	out	%al, (%dx)
	mov	$'a', %al
	out	%al, (%dx)
	mov	$'n', %al
	out	%al, (%dx)
	mov	$'d', %al
	out	%al, (%dx)
	mov	$'l', %al
	out	%al, (%dx)
	mov	$'e', %al
	out	%al, (%dx)
	mov	$'r', %al
	out	%al, (%dx)
	mov	$' ', %al
	out	%al, (%dx)
	mov	$'s', %al
	out	%al, (%dx)
	mov	$'t', %al
	out	%al, (%dx)
	mov	$'a', %al
	out	%al, (%dx)
	mov	$'r', %al
	out	%al, (%dx)
	mov	$'t', %al
	out	%al, (%dx)
	mov	$'i', %al
	out	%al, (%dx)
	mov	$'n', %al
	out	%al, (%dx)
	mov	$'g', %al
	out	%al, (%dx)
	mov	$'\n', %al
	out	%al, (%dx)

	pushq	%rsp

	pushq	%rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15

	movq	%cr3, %rax
	pushq	%rax

	movq	%cr4, %rbx
	pushq	%rbx
	or	$0x640, %rbx

	movq	%rbx, %cr4

	movq	%cr0, %rbx
	pushq	%rbx
	or	$0x22, %rbx

	mov	%rbx, %cr0

	movq	%rsp, %r12
	andq	$~0xF, %rsp

	subq	$0x200, %rsp
	fxsave64 (%rsp)

	/*
	 * All the macros we compare (r)di to are defined in mm_handler.h
	 * This differs a bit from what we do above, as ABI calling convention
	 * is not the same for protected and long mode. First two elements of
	 * the struct fits rdi. Command is in the first byte of the rdi,
	 * so we can just read of di.
	 */
	cmp $MM_ACPI_DISABLE, %di
	je acpi_disable

	cmp $MM_ACPI_ENABLE, %di
	je acpi_enable

	cmp $MM_STORE, %di
	je mm_store

	jmp restore_cb_state64

acpi_enable:
	mov	$0x3f8, %dx
	mov	$'E', %al
	out	%al, (%dx)
	mov	$'n', %al
	out	%al, (%dx)
	mov	$'a', %al
	out	%al, (%dx)
	mov	$'b', %al
	out	%al, (%dx)
	mov	$'l', %al
	out	%al, (%dx)
	mov	$'i', %al
	out	%al, (%dx)
	mov	$'n', %al
	out	%al, (%dx)
	mov	$'g', %al
	out	%al, (%dx)
	mov	$' ', %al
	out	%al, (%dx)
	mov	$'A', %al
	out	%al, (%dx)
	mov	$'C', %al
	out	%al, (%dx)
	mov	$'P', %al
	out	%al, (%dx)
	mov	$'I', %al
	out	%al, (%dx)
	mov	$'\n', %al
	out	%al, (%dx)

	// Stash the command from rdi.
	shr	$32, %rdi
	// PM1_CNT & ~SCI_EN
	mov	%di, %ax
	add	$MM_ACPI_ENABLE, %ax
	// si contains ACPI_BASE_ADDR
	mov	%si, %dx

	out	%ax, %dx

	jmp	restore_cb_state64

acpi_disable:
	mov	$0x3f8, %dx
	mov	$'D', %al
	out	%al, (%dx)
	mov	$'i', %al
	out	%al, (%dx)
	mov	$'s', %al
	out	%al, (%dx)
	mov	$'a', %al
	out	%al, (%dx)
	mov	$'b', %al
	out	%al, (%dx)
	mov	$'l', %al
	out	%al, (%dx)
	mov	$'i', %al
	out	%al, (%dx)
	mov	$'n', %al
	out	%al, (%dx)
	mov	$'g', %al
	out	%al, (%dx)
	mov	$' ', %al
	out	%al, (%dx)
	mov	$'A', %al
	out	%al, (%dx)
	mov	$'C', %al
	out	%al, (%dx)
	mov	$'P', %al
	out	%al, (%dx)
	mov	$'I', %al
	out	%al, (%dx)
	mov	$'\n', %al
	out	%al, (%dx)

	// Stash command
	shr	$32, %rdi
	// PM1_CNT | SCI_EN
	mov	%di, %ax
	add	$MM_ACPI_DISABLE, %ax
	// si contains ACPI_BASE_ADDR
	mov	%si, %dx

	out	%ax, %dx

	jmp	restore_cb_state64

mm_store:
	// see comment above in mm_store32

restore_cb_state64:
	mov	$0x3f8, %dx
	mov	$'M', %al
	out	%al, (%dx)
	mov	$'M', %al
	out	%al, (%dx)
	mov	$'6', %al
	out	%al, (%dx)
	mov	$'4', %al
	out	%al, (%dx)
	mov	$' ', %al
	out	%al, (%dx)
	mov	$'r', %al
	out	%al, (%dx)
	mov	$'e', %al
	out	%al, (%dx)
	mov	$'t', %al
	out	%al, (%dx)
	mov	$'u', %al
	out	%al, (%dx)
	mov	$'r', %al
	out	%al, (%dx)
	mov	$'n', %al
	out	%al, (%dx)
	mov	$'s', %al
	out	%al, (%dx)
	mov	$'\n', %al
	out	%al, (%dx)

	fxrstor64 (%rsp)
	addq	$0x200, %rsp
	movq	%r12, %rsp

	popq	%rbx
	movq	%rbx, %cr0

	popq	%rbx
	movq	%rbx, %cr4

	popq	%rax
	movq	%rax, %cr3

	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	popq	%rsp
	ret
SYM_CODE_END(mm_entry_64)

	.bss
	.balign	4
SYM_DATA(mm_signature,		.space 4)
SYM_DATA(mm_blob_size,		.space 2)
SYM_DATA(mm_entry_32_offset,	.space 4)
SYM_DATA(mm_entry_64_offset,	.space 4)
