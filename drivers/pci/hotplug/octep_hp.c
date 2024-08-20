// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2024 Marvell. */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pci_hotplug.h>
#include <linux/slab.h>

#define OCTEP_HP_INTR_OFFSET(x) (0x20400 + ((x) << 4))
#define OCTEP_HP_INTR_VECTOR(x) (16 + (x))
#define OCTEP_HP_DRV_NAME       "octep_hp"

/* Interrupt vectors for hotplug enable and disable events. */
enum octep_hp_vec_type {
	OCTEP_HP_VEC_ENA,
	OCTEP_HP_VEC_DIS,
};

struct octep_hp_cmd {
	struct list_head list;
	enum octep_hp_vec_type vec_type;
	u64 slot_mask;
};

struct octep_hp_slot {
	struct list_head list;
	struct hotplug_slot slot;
	u16 slot_number;
	struct pci_dev *hp_pdev;
	unsigned int hp_devfn;
	struct octep_hp_controller *ctrl;
};

struct octep_hp_controller {
	void __iomem *base;
	struct pci_dev *pdev;
	struct work_struct work;
	struct list_head slot_list;
	struct mutex slot_lock; /* Protects slot_list */
	struct list_head hp_cmd_list;
	spinlock_t hp_cmd_lock; /* Protects hp_cmd_list */
};

static void octep_hp_enable_pdev(struct octep_hp_controller *hp_ctrl, struct octep_hp_slot *hp_slot)
{
	mutex_lock(&hp_ctrl->slot_lock);
	if (hp_slot->hp_pdev) {
		dev_dbg(&hp_slot->hp_pdev->dev, "Slot %u already enabled\n", hp_slot->slot_number);
		mutex_unlock(&hp_ctrl->slot_lock);
		return;
	}

	/* Scan the device and add it to the bus */
	hp_slot->hp_pdev = pci_scan_single_device(hp_ctrl->pdev->bus, hp_slot->hp_devfn);
	pci_bus_assign_resources(hp_ctrl->pdev->bus);
	pci_bus_add_device(hp_slot->hp_pdev);

	dev_dbg(&hp_slot->hp_pdev->dev, "Enabled slot %u\n", hp_slot->slot_number);
	mutex_unlock(&hp_ctrl->slot_lock);
}

static void octep_hp_disable_pdev(struct octep_hp_controller *hp_ctrl,
				  struct octep_hp_slot *hp_slot)
{
	mutex_lock(&hp_ctrl->slot_lock);
	if (!hp_slot->hp_pdev) {
		dev_dbg(&hp_ctrl->pdev->dev, "Slot %u already disabled\n", hp_slot->slot_number);
		mutex_unlock(&hp_ctrl->slot_lock);
		return;
	}

	dev_dbg(&hp_slot->hp_pdev->dev, "Disabling slot %u\n", hp_slot->slot_number);

	/* Remove the device from the bus */
	pci_stop_and_remove_bus_device_locked(hp_slot->hp_pdev);
	hp_slot->hp_pdev = NULL;
	mutex_unlock(&hp_ctrl->slot_lock);
}

static int octep_hp_enable_slot(struct hotplug_slot *slot)
{
	struct octep_hp_slot *hp_slot = container_of(slot, struct octep_hp_slot, slot);

	octep_hp_enable_pdev(hp_slot->ctrl, hp_slot);
	return 0;
}

static int octep_hp_disable_slot(struct hotplug_slot *slot)
{
	struct octep_hp_slot *hp_slot = container_of(slot, struct octep_hp_slot, slot);

	octep_hp_disable_pdev(hp_slot->ctrl, hp_slot);
	return 0;
}

static struct hotplug_slot_ops octep_hp_slot_ops = {
	.enable_slot = octep_hp_enable_slot,
	.disable_slot = octep_hp_disable_slot,
};

#define SLOT_NAME_SIZE 16
static int octep_hp_register_slot(struct octep_hp_controller *hp_ctrl, struct pci_dev *pdev,
				  u16 slot_number)
{
	char slot_name[SLOT_NAME_SIZE];
	struct octep_hp_slot *hp_slot;
	int ret;

	hp_slot = kzalloc(sizeof(*hp_slot), GFP_KERNEL);
	if (!hp_slot)
		return -ENOMEM;

	hp_slot->ctrl = hp_ctrl;
	hp_slot->hp_pdev = pdev;
	hp_slot->hp_devfn = pdev->devfn;
	hp_slot->slot_number = slot_number;
	hp_slot->slot.ops = &octep_hp_slot_ops;

	snprintf(slot_name, SLOT_NAME_SIZE, "octep_hp_%u", slot_number);
	ret = pci_hp_register(&hp_slot->slot, hp_ctrl->pdev->bus, PCI_SLOT(pdev->devfn), slot_name);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register hotplug slot %u\n", slot_number);
		kfree(hp_slot);
		return ret;
	}

	octep_hp_disable_pdev(hp_ctrl, hp_slot);
	list_add_tail(&hp_slot->list, &hp_ctrl->slot_list);

	return 0;
}

static bool octep_hp_slot(struct octep_hp_controller *hp_ctrl, struct pci_dev *pdev)
{
	/* Check if the PCI device can be hotplugged */
	return pdev != hp_ctrl->pdev && pdev->bus == hp_ctrl->pdev->bus &&
		PCI_SLOT(pdev->devfn) == PCI_SLOT(hp_ctrl->pdev->devfn);
}

static void octep_hp_cmd_handler(struct octep_hp_controller *hp_ctrl, struct octep_hp_cmd *hp_cmd)
{
	struct octep_hp_slot *hp_slot;

	/* Enable or disable the slots based on the slot mask */
	list_for_each_entry(hp_slot, &hp_ctrl->slot_list, list) {
		if (hp_cmd->slot_mask & BIT(hp_slot->slot_number)) {
			if (hp_cmd->vec_type == OCTEP_HP_VEC_ENA)
				octep_hp_enable_pdev(hp_ctrl, hp_slot);
			else
				octep_hp_disable_pdev(hp_ctrl, hp_slot);
		}
	}
}

static void octep_hp_work_handler(struct work_struct *work)
{
	struct octep_hp_controller *hp_ctrl = container_of(work, struct octep_hp_controller, work);
	struct octep_hp_cmd *hp_cmd;
	unsigned long flags;

	/* Process all the hotplug commands */
	spin_lock_irqsave(&hp_ctrl->hp_cmd_lock, flags);
	while (!list_empty(&hp_ctrl->hp_cmd_list)) {
		hp_cmd = list_first_entry(&hp_ctrl->hp_cmd_list, struct octep_hp_cmd, list);
		list_del(&hp_cmd->list);
		spin_unlock_irqrestore(&hp_ctrl->hp_cmd_lock, flags);

		octep_hp_cmd_handler(hp_ctrl, hp_cmd);
		kfree(hp_cmd);

		spin_lock_irqsave(&hp_ctrl->hp_cmd_lock, flags);
	}
	spin_unlock_irqrestore(&hp_ctrl->hp_cmd_lock, flags);
}

static irqreturn_t octep_hp_intr_handler(int irq, void *data)
{
	struct octep_hp_controller *hp_ctrl = data;
	struct pci_dev *pdev = hp_ctrl->pdev;
	enum octep_hp_vec_type vec_type;
	struct octep_hp_cmd *hp_cmd;
	u64 slot_mask;

	vec_type = pci_irq_vector(pdev, OCTEP_HP_INTR_VECTOR(OCTEP_HP_VEC_ENA)) == irq ?
		OCTEP_HP_VEC_ENA : OCTEP_HP_VEC_DIS;

	slot_mask = readq(hp_ctrl->base + OCTEP_HP_INTR_OFFSET(vec_type));
	if (!slot_mask) {
		dev_err(&pdev->dev, "Invalid slot mask %llx\n", slot_mask);
		return IRQ_HANDLED;
	}

	hp_cmd = kzalloc(sizeof(*hp_cmd), GFP_ATOMIC);
	if (!hp_cmd)
		return IRQ_HANDLED;

	hp_cmd->slot_mask = slot_mask;
	hp_cmd->vec_type = vec_type;

	/* Add the command to the list and schedule the work */
	spin_lock(&hp_ctrl->hp_cmd_lock);
	list_add_tail(&hp_cmd->list, &hp_ctrl->hp_cmd_list);
	spin_unlock(&hp_ctrl->hp_cmd_lock);
	schedule_work(&hp_ctrl->work);

	/* Clear the interrupt */
	writeq(slot_mask, hp_ctrl->base + OCTEP_HP_INTR_OFFSET(vec_type));

	return IRQ_HANDLED;
}

static void octep_hp_deregister_slots(struct octep_hp_controller *hp_ctrl)
{
	struct octep_hp_slot *hp_slot, *tmp;

	/* Deregister all the hotplug slots */
	list_for_each_entry_safe(hp_slot, tmp, &hp_ctrl->slot_list, list) {
		pci_hp_deregister(&hp_slot->slot);
		octep_hp_enable_pdev(hp_ctrl, hp_slot);
		list_del(&hp_slot->list);
		kfree(hp_slot);
	}
}

static void octep_hp_controller_cleanup(void *data)
{
	struct octep_hp_controller *hp_ctrl = data;

	pci_free_irq_vectors(hp_ctrl->pdev);
	flush_work(&hp_ctrl->work);
	octep_hp_deregister_slots(hp_ctrl);
}

static int octep_hp_controller_setup(struct pci_dev *pdev, struct octep_hp_controller *hp_ctrl)
{
	struct device *dev = &pdev->dev;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret) {
		dev_err(dev, "Failed to enable PCI device\n");
		return ret;
	}

	ret = pcim_iomap_regions(pdev, BIT(0), OCTEP_HP_DRV_NAME);
	if (ret) {
		dev_err(dev, "Failed to request MMIO region\n");
		return ret;
	}

	pci_set_master(pdev);
	pci_set_drvdata(pdev, hp_ctrl);

	INIT_LIST_HEAD(&hp_ctrl->slot_list);
	INIT_LIST_HEAD(&hp_ctrl->hp_cmd_list);
	mutex_init(&hp_ctrl->slot_lock);
	spin_lock_init(&hp_ctrl->hp_cmd_lock);
	INIT_WORK(&hp_ctrl->work, octep_hp_work_handler);

	hp_ctrl->pdev = pdev;
	hp_ctrl->base = pcim_iomap_table(pdev)[0];
	if (!hp_ctrl->base) {
		dev_err(dev, "Failed to get device resource map\n");
		return -ENODEV;
	}

	ret = pci_alloc_irq_vectors(pdev, 1, OCTEP_HP_INTR_VECTOR(OCTEP_HP_VEC_DIS) + 1,
				    PCI_IRQ_MSIX);
	if (ret < 0) {
		dev_err(dev, "Failed to alloc MSI-X vectors");
		return ret;
	}

	ret = devm_add_action(dev, octep_hp_controller_cleanup, hp_ctrl);
	if (ret) {
		dev_err(dev, "Failed to add action for controller cleanup\n");
		return ret;
	}

	ret = devm_request_irq(dev, pci_irq_vector(pdev, OCTEP_HP_INTR_VECTOR(OCTEP_HP_VEC_ENA)),
			       octep_hp_intr_handler, 0, "octep_hp_ena", hp_ctrl);
	if (ret) {
		dev_err(dev, "Failed to register slot enable interrupt handle\n");
		return ret;
	}

	ret = devm_request_irq(dev, pci_irq_vector(pdev, OCTEP_HP_INTR_VECTOR(OCTEP_HP_VEC_DIS)),
			       octep_hp_intr_handler, 0, "octep_hp_dis", hp_ctrl);
	if (ret) {
		dev_err(dev, "Failed to register slot disable interrupt handle\n");
		return ret;
	}

	return 0;
}

static int octep_hp_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct octep_hp_controller *hp_ctrl;
	struct pci_dev *tmp_pdev = NULL;
	u16 slot_number = 0;
	int ret;

	hp_ctrl = devm_kzalloc(&pdev->dev, sizeof(*hp_ctrl), GFP_KERNEL);
	if (!hp_ctrl)
		return -ENOMEM;

	ret = octep_hp_controller_setup(pdev, hp_ctrl);
	if (ret) {
		dev_err(&pdev->dev, "Failed to setup octep controller\n");
		return ret;
	}

	/*
	 * Register all hotplug slots. Hotplug controller is the first function of the PCI device.
	 * The hotplug slots are the remaining functions of the PCI device. They are removed from
	 * the bus and are added back when the hotplug event is triggered.
	 */
	for_each_pci_dev(tmp_pdev) {
		if (octep_hp_slot(hp_ctrl, tmp_pdev)) {
			ret = octep_hp_register_slot(hp_ctrl, tmp_pdev, slot_number++);
			if (ret) {
				dev_err(&pdev->dev, "Failed to register hotplug slots.\n");
				return ret;
			}
		}
	}

	return 0;
}

#define OCTEP_DEVID_HP_CONTROLLER 0xa0e3
static struct pci_device_id octep_hp_pci_map[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_CAVIUM, OCTEP_DEVID_HP_CONTROLLER) },
	{ 0 },
};

static struct pci_driver octep_hp = {
	.name     = OCTEP_HP_DRV_NAME,
	.id_table = octep_hp_pci_map,
	.probe    = octep_hp_pci_probe,
};

module_pci_driver(octep_hp);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Marvell");
MODULE_DESCRIPTION("OCTEON PCIe device hotplug controller driver");
