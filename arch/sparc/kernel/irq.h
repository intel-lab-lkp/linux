/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/platform_device.h>

#include <asm/cpu_type.h>

struct irq_bucket {
        struct irq_bucket *next;
        unsigned int real_irq;
        unsigned int irq;
        unsigned int pil;
};

#define MAX_BOARD 10
#define MAX_IRQ ((MAX_BOARD + 2) << 5)

/* Map between the irq identifier used in hw to the
 * irq_bucket. The map is sufficient large to hold
 * the sun4d hw identifiers.
 */
extern struct irq_bucket *irq_map[MAX_IRQ];

unsigned int irq_alloc(unsigned int real_irq, unsigned int pil);
void irq_link(unsigned int irq);
void irq_unlink(unsigned int irq);
void handler_irq(unsigned int pil, struct pt_regs *regs);

unsigned long leon_get_irqmask(unsigned int irq);

/* leon_kernel.c */
void leon_clear_clock_irq(void);
void leon_load_profile_irq(int cpu, unsigned int limit);
u32 leon_cycles_offset(void);
void leon_nmi(struct pt_regs *regs);
