// SPDX-License-Identifier: GPL-2.0

#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/irqchip/irq-goldfish-pic.h>

#include <asm/irq.h>
#include <asm/irq_regs.h>
#include <asm/processor.h>
#include <asm/virt.h>

static struct resource picres[6];
static const char *picname[6] = {
	"goldfish_pic.0",
	"goldfish_pic.1",
	"goldfish_pic.2",
	"goldfish_pic.3",
	"goldfish_pic.4",
	"goldfish_pic.5"
};

/*
 * 6 goldfish-pic for CPU IRQ #1 to IRQ #6
 * CPU IRQ #1 -> PIC #1
 *               IRQ #1 to IRQ #31 -> unused
 *               IRQ #32 -> goldfish-tty
 * CPU IRQ #2 -> PIC #2
 *               IRQ #1 to IRQ #32 -> virtio-mmio from 1 to 32
 * CPU IRQ #3 -> PIC #3
 *               IRQ #1 to IRQ #32 -> virtio-mmio from 33 to 64
 * CPU IRQ #4 -> PIC #4
 *               IRQ #1 to IRQ #32 -> virtio-mmio from 65 to 96
 * CPU IRQ #5 -> PIC #5
 *               IRQ #1 to IRQ #32 -> virtio-mmio from 97 to 128
 * CPU IRQ #6 -> PIC #6
 *               IRQ #1 -> goldfish-timer
 *               IRQ #2 -> goldfish-rtc
 *               IRQ #3 to IRQ #32 -> unused
 * CPU IRQ #7 -> NMI
 */

static irqreturn_t virt_nmi_handler(int irq, void *dev_id)
{
	static int in_nmi;

	if (READ_ONCE(in_nmi))
		return IRQ_HANDLED;
	WRITE_ONCE(in_nmi, 1);

	pr_warn("Non-Maskable Interrupt\n");
	show_registers(get_irq_regs());

	WRITE_ONCE(in_nmi, 0);
	return IRQ_HANDLED;
}

void __init virt_init_IRQ(void)
{
	unsigned int i;
	int ret;

	for (i = 0; i < 6; i++) {
		picres[i] = (struct resource)
		    DEFINE_RES_MEM_NAMED(virt_bi_data.pic.mmio + i * 0x1000,
					 0x1000, picname[i]);
		if (request_resource(&iomem_resource, &picres[i])) {
			pr_err("Cannot allocate %s resource\n", picname[i]);
			return;
		}

		ret = goldfish_pic_init((void __iomem *)(virt_bi_data.pic.mmio + i * 0x1000),
					virt_bi_data.pic.irq + i,
					IRQ_USER + i * 32, NULL);
		if (ret) {
			pr_err("Failed to initialize %s\n", picname[i]);
			return;
		}
	}

	if (request_irq(IRQ_AUTO_7, virt_nmi_handler, 0, "NMI",
			virt_nmi_handler))
		pr_err("Couldn't register NMI\n");
}
