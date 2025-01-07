/* SPDX-License-Identifier: GPL-2.0 */
bool on_irq_stack(struct pt_regs *regs);
void call_on_irq_stack(void (*fn)(void *), void *arg);
