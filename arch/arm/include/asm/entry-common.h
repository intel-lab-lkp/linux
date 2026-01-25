/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARCH_ARM_ENTRY_COMMON_H
#define ARCH_ARM_ENTRY_COMMON_H

asmlinkage __section(".entry.text")
void arm_enter_from_user_mode(void);

asmlinkage __section(".entry.text")
void arm_exit_to_user_mode_no_work_pending(void);

#endif
