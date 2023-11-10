/* SPDX-License-Identifier: GPL-2.0 */

#ifdef CONFIG_EXITZ_SYSCALL

/*
 * Zero resource on exit flags
 */
#define EZ_NONE			0x00000000
#define EZ_MEM                  0x00000001      /* Memory pages are cleared on exit */
#define EZ_FLAGS (EZ_MEM)

/*
 * Overwrite current process memory range with zeros (end excluded).
 */
int memz_range(unsigned long start, unsigned long end);

/*
 * Overwrite all flagged resources with zeros.
 */
void exit_z(void);

/*
 * Set task_struct flags to zero flagged resources on exit.
 */
void do_exitz(int flags);

#endif
