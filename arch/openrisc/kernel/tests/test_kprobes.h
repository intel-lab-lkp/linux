/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Unit tests borrowed liberally from similar work of RISC-V. All
 * original copyrights apply as per the original source declaration.
 */

#ifndef TEST_KPROBES_H
#define TEST_KPROBES_H

#define KPROBE_TEST_MAGIC          0xDEADC0DE
#define KPROBE_TEST_MAGIC_LOWER    0x0000C0DE
#define KPROBE_TEST_MAGIC_UPPER    0x0000DEAD

#ifndef __ASSEMBLER__

/* array of addresses to install kprobes */
extern void *test_kprobes_addresses[];

/* array of functions that return KPROBE_TEST_MAGIC */
extern unsigned long (*test_kprobes_functions[])(void);

#endif /* __ASSEMBLER__ */

#endif /* TEST_KPROBES_H */
