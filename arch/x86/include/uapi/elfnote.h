/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _X86_UAPI_ELFNOTE_H_
#define _X86_UAPI_ELFNOTE_H_

/*
 * "x86" namespaced ELF note structures to communicate features
 * supported by the kernel binary to external utilities which need that
 * info in order to do additional preparatory work based on the target
 * kernel image.
 */

/*
 * Used by the microcode loader to communicate support to external
 * initrd generators like dracut.
 */
#define X86_ELFNOTE_MICROCODE	0

#endif /* _X86_UAPI_ELFNOTE_H_ */
