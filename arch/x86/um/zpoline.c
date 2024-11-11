// SPDX-License-Identifier: GPL-2.0
/*
 *  zpoline.c
 *
 *  Replace syscall/sysenter instructions to `call *%rax` to hook syscalls.
 *
 */
//#define DEBUG
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/elf-fdpic.h>
#include <asm/unistd.h>
#include <asm/insn.h>
#include <sysdep/syscalls.h>
#include <os.h>

/* start of trampoline code area */
static char *__zpoline_start;

static int __zpoline_translate_syscalls(struct elf_fdpic_params *params)
{
	int count = 0, loop;
	struct insn insn;
	unsigned long addr;
	struct elf_fdpic_loadseg *seg;
	struct elf_phdr *phdr;
	struct elfhdr *ehdr = (struct elfhdr *)params->elfhdr_addr;

	if (!ehdr)
		return 0;

	seg = params->loadmap->segs;
	phdr = params->phdrs;
	for (loop = 0; loop < params->hdr.e_phnum; loop++, phdr++) {
		if (phdr->p_type != PT_LOAD)
			continue;
		addr = seg->addr;
		/* skip translation of trampoline code */
		if (addr <= (unsigned long)(&__zpoline_start[0] + 0x1000 + 0x0100)) {
			pr_warn("%lx: address is in the range of trampoline", addr);
			return -EINVAL;
		}

		/* translate only segment with Executable flag */
		if (!(phdr->p_flags & PF_X)) {
			seg++;
			continue;
		}

		pr_debug("translation 0x%lx-0x%llx", addr,
			 seg->addr + seg->p_memsz);
		/* now ready to translate */
		while (addr < (seg->addr + seg->p_memsz)) {
			insn_init(&insn, (void *)addr, MAX_INSN_SIZE, 1);
			insn_get_length(&insn);

			insn_get_opcode(&insn);

			switch (insn.opcode.bytes[0]) {
			case 0xf:
				switch (insn.opcode.bytes[1]) {
				case 0x05: /* syscall */
				case 0x34: /* sysenter */
					pr_debug("%lx: found syscall/sysenter", addr);
					*(char *)addr = 0xff; // callq
					*((char *)addr + 1) = 0xd0; // *%rax
					count++;
					break;
				}
			default:
				break;
			}

			addr += insn.length;
			if (insn.length == 0) {
				pr_debug("%lx: length zero with byte %x. skip ?",
					 addr, insn.opcode.bytes[0]);
				addr += 1;
			}
		}
		seg++;
	}
	return count;
}

/**
 * elf_arch_finalize_exec() - architecture hook to translate syscall/sysenter
 *
 * translate syscall/sysenter instruction upon loading ELF binary file
 * on execve(2)&co syscall.
 *
 * suppose we have those instructions:
 *
 *    mov $sysnr, %rax
 *    syscall                 0f 05
 *
 * this will translate it with:
 *
 *    mov $sysnr, %rax        (<= untouched)
 *    call *(%rax)            ff d0
 *
 * this will finally called hook function guided by trampoline code installed
 * at setup_zpoline_trampoline().
 *
 * @exec_params: ELF meta data for executable file
 * @interp_params: ELF meta data for the interpreter file
 */
int elf_arch_finalize_exec(struct elf_fdpic_params *exec_params,
			   struct elf_fdpic_params *interp_params)
{
	int err = 0, count = 0;
	struct mm_struct *mm = current->mm;

	if (down_write_killable(&mm->mmap_lock))
		return -EINTR;

	/* translate for the executable */
	err = __zpoline_translate_syscalls(exec_params);
	if (err < 0) {
		pr_info("zpoline: xlate error %d", err);
		goto out;
	}
	count += err;
	pr_debug("zpoline: rewritten (exec) %d syscalls\n", count);

	/* translate for the interpreter */
	err = __zpoline_translate_syscalls(interp_params);
	if (err < 0) {
		pr_info("zpoline: xlate error %d", err);
		goto out;
	}
	count += err;

	err = 0;
	pr_debug("zpoline: rewritten (exec+interp) %d syscalls\n", count);

out:
	up_write(&mm->mmap_lock);
	return err;
}

/**
 * setup_zpoline_trampoline() - install trampoline code for zpoline
 *
 * setup trampoline code for syscall hooks
 *
 * the trampoline code guides to call hooked function, __kernel_vsyscall
 * in this case, via nop slides at the memory address zero (thus, zpoline).
 *
 * loaded binary by exec(2) is translated to call the function.
 */
static int __init setup_zpoline_trampoline(void)
{
	int i, ret;
	int ptr;

	/* zpoline: map area of trampoline code started from addr 0x0 */
	__zpoline_start = 0x0;

	ret = os_map_memory((void *) 0, -1, 0, PAGE_SIZE, 1, 1, 1);
	if (ret)
		panic("map failed\n NOTE: /proc/sys/vm/mmap_min_addr should be set 0\n");

	/* fill nop instructions until the trampoline code */
	for (i = 0; i < NR_syscalls; i++)
		__zpoline_start[i] = 0x90;

	/* optimization to skip old syscalls */
	/* short jmp */
	__zpoline_start[214 /* __NR_epoll_ctl_old */] = 0xeb;
	/* range of a short jmp : -128 ~ +127 */
	__zpoline_start[215 /* __NR_epoll_wait_old */] = 127;

	/**
	 * FIXME: shift red zone area to properly handle the case
	 */

	/**
	 * put code for jumping to __kernel_vsyscall.
	 *
	 * here we embed the following code.
	 *
	 * movabs [$addr],%r11
	 * jmpq   *%r11
	 *
	 */
	ptr = NR_syscalls;
	/* 49 bb [64-bit addr (8-byte)]    movabs [64-bit addr (8-byte)],%r11 */
	__zpoline_start[ptr++] = 0x49;
	__zpoline_start[ptr++] = 0xbb;
	__zpoline_start[ptr++] = ((uint64_t) __kernel_vsyscall >> (8 * 0));
	__zpoline_start[ptr++] = ((uint64_t) __kernel_vsyscall >> (8 * 1));
	__zpoline_start[ptr++] = ((uint64_t) __kernel_vsyscall >> (8 * 2));
	__zpoline_start[ptr++] = ((uint64_t) __kernel_vsyscall >> (8 * 3));
	__zpoline_start[ptr++] = ((uint64_t) __kernel_vsyscall >> (8 * 4));
	__zpoline_start[ptr++] = ((uint64_t) __kernel_vsyscall >> (8 * 5));
	__zpoline_start[ptr++] = ((uint64_t) __kernel_vsyscall >> (8 * 6));
	__zpoline_start[ptr++] = ((uint64_t) __kernel_vsyscall >> (8 * 7));

	/*
	 * pretending to be syscall instruction by putting return
	 * address in %rcx.
	 */
	/* 48 8b 0c 24               mov    (%rsp),%rcx */
	__zpoline_start[ptr++] = 0x48;
	__zpoline_start[ptr++] = 0x8b;
	__zpoline_start[ptr++] = 0x0c;
	__zpoline_start[ptr++] = 0x24;

	/* 41 ff e3                jmp    *%r11 */
	__zpoline_start[ptr++] = 0x41;
	__zpoline_start[ptr++] = 0xff;
	__zpoline_start[ptr++] = 0xe3;

	/* permission: XOM (PROT_EXEC only) */
	ret = os_protect_memory(0, PAGE_SIZE, 0, 0, 1);
	if (ret)
		panic("failed: can't configure permission on trampoline code");

	pr_info("zpoline: setting up trampoline code done\n");
	return 0;
}
arch_initcall(setup_zpoline_trampoline);
