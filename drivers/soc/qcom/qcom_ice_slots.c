// SPDX-License-Identifier: GPL-2.0-only
/*
 * qcom_ice_slots.c - Qualcomm ICE keyslot partitioning for guest VMs
 *
 * Implements bcp_slot_virt_ops: translates a (guest_id, virtual-slot) pair to
 * a physical ICE keyslot index using a per-VM allocation table parsed from
 * the device-tree node with compatible = "qcom,ice-keyslot-map".
 *
 * Device-tree layout:
 *
 *   ice_keyslot_map: ice-keyslot-map {
 *       compatible = "qcom,ice-keyslot-map";
 *       #address-cells = <1>;
 *       #size-cells = <0>;
 *
 *       vm@3  { reg = <3>;  qcom,max-ice-slots = <16>; qcom,ice-slot-offset = <0>;  };
 *       vm@52 { reg = <52>; qcom,max-ice-slots = <32>; qcom,ice-slot-offset = <16>; };
 *   };
 *
 * Each child entry maps a guest (reg = guest_id) to a contiguous physical keyslot
 * range [slot_offset .. slot_offset + max_ice_slots).
 *
 * Entry 0 is always the host's own reservation.  Entries 1+ are guest
 * reservations.  The host's entry is used by ufs-qcom to size its
 * blk_crypto_profile; it is excluded from the guest-facing translation table
 * so that blk-crypto-proxy can never accidentally route a guest request into
 * the host's physical keyslots.
 *
 * The ufs-qcom driver reads the host slot info and validates all entries
 * against the hardware slot count directly via the OF API, with no symbol
 * dependency on this module.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/blk-crypto-profile.h>
#include <linux/blk-crypto-proxy.h>

#define QCOM_ICE_SLOTS_MAX_ENTRIES	8

struct qcom_ice_slot_entry {
	u32 guest_id;
	u32 max_slots;
	u32 slot_offset;
};

struct qcom_ice_slots {
	struct qcom_ice_slot_entry entries[QCOM_ICE_SLOTS_MAX_ENTRIES];
	unsigned int num_entries;
};

/*
 * There is at most one qcom,ice-keyslot-map platform node per SoC.  A single
 * global pointer is set at probe time and cleared at remove time.  The
 * hot-path read (from bcp_slot_virt_ops callbacks) is protected by RCU;
 * probe/remove serialise via the platform driver guarantee.
 */
static struct qcom_ice_slots __rcu *g_ice_slots;

static struct qcom_ice_slots *virt_lookup(struct blk_crypto_profile *profile)
{
	/* Single UFS controller: profile argument is not needed. */
	return rcu_dereference(g_ice_slots);
}

static int qcom_ice_slots_get_guest_slots(struct blk_crypto_profile *profile,
					  u32 guest_id)
{
	struct qcom_ice_slots *virt = virt_lookup(profile);
	unsigned int i;

	if (!virt)
		return -ENOKEY;

	/* entries[0] is the host; guest entries start at index 1. */
	for (i = 1; i < virt->num_entries; i++) {
		if (virt->entries[i].guest_id == guest_id)
			return virt->entries[i].max_slots;
	}
	return -ENOKEY;
}

static int qcom_ice_slots_vslot_to_pslot(struct blk_crypto_profile *profile,
					 u32 guest_id, u32 virt_slot,
					 unsigned int *phy_slot_out)
{
	struct qcom_ice_slots *virt = virt_lookup(profile);
	unsigned int i;

	if (!virt)
		return -ENOKEY;

	for (i = 1; i < virt->num_entries; i++) {
		if (virt->entries[i].guest_id != guest_id)
			continue;
		if (virt_slot >= virt->entries[i].max_slots)
			return -EINVAL;
		*phy_slot_out = virt->entries[i].slot_offset + virt_slot;
		return 0;
	}
	return -ENOKEY;
}

static const struct bcp_slot_virt_ops qcom_slot_virt_ops = {
	.get_guest_slots = qcom_ice_slots_get_guest_slots,
	.vslot_to_pslot = qcom_ice_slots_vslot_to_pslot,
};

static int qcom_ice_slots_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *child;
	struct qcom_ice_slots *virt;
	unsigned int idx = 0, total_slots = 0;
	int ret = 0;

	virt = devm_kzalloc(dev, sizeof(*virt), GFP_KERNEL);
	if (!virt)
		return -ENOMEM;

	for_each_child_of_node(dev->of_node, child) {
		u32 guest_id, max_slots, slot_offset;
		unsigned int j;

		if (idx >= QCOM_ICE_SLOTS_MAX_ENTRIES) {
			dev_err(dev, "too many vm entries (> %u)\n",
				QCOM_ICE_SLOTS_MAX_ENTRIES);
			ret = -EINVAL;
			of_node_put(child);
			goto err_free;
		}

		if (of_property_read_u32(child, "reg", &guest_id))
			continue;
		if (of_property_read_u32(child, "qcom,max-ice-slots", &max_slots))
			continue;
		if (of_property_read_u32(child, "qcom,ice-slot-offset", &slot_offset)) {
			dev_err(dev, "missing qcom,ice-slot-offset for guest_id=%u\n",
				guest_id);
			ret = -EINVAL;
			of_node_put(child);
			goto err_free;
		}

		if (idx > 0 &&
		    slot_offset <
		    virt->entries[idx - 1].slot_offset +
		    virt->entries[idx - 1].max_slots) {
			dev_err(dev, "slot overlap: guest_id=%u overlaps guest_id=%u\n",
				guest_id, virt->entries[idx - 1].guest_id);
			ret = -EINVAL;
			of_node_put(child);
			goto err_free;
		}

		for (j = 0; j < idx; j++) {
			if (virt->entries[j].guest_id == guest_id) {
				dev_err(dev, "duplicate guest_id=%u\n", guest_id);
				ret = -EINVAL;
				of_node_put(child);
				goto err_free;
			}
		}

		virt->entries[idx].guest_id = guest_id;
		virt->entries[idx].max_slots = max_slots;
		virt->entries[idx].slot_offset = slot_offset;
		total_slots += max_slots;
		idx++;
	}

	if (idx == 0) {
		dev_err(dev, "no VM entries found in qcom,ice-keyslot-map\n");
		ret = -EINVAL;
		goto err_free;
	}

	virt->num_entries = idx;

	/*
	 * Publish the singleton.  From this point on, bcp_slot_virt_ops
	 * callbacks can resolve virt via rcu_dereference(g_ice_slots).
	 */
	rcu_assign_pointer(g_ice_slots, virt);

	ret = bcp_register_slot_virt_ops(&qcom_slot_virt_ops);
	if (ret) {
		dev_err(dev, "failed to register slot_virt_ops: %d\n", ret);
		goto err_free;
	}

	dev_info(dev, "registered: %u VMs, %u total ICE slots\n",
		 idx, total_slots);
	return 0;

err_free:
	return ret;
}

static void qcom_ice_slots_remove(struct platform_device *pdev)
{
	bcp_unregister_slot_virt_ops(&qcom_slot_virt_ops);
	/*
	 * Clear the singleton under RCU so that any concurrent ioctl that
	 * already took the read lock and is mid-lookup sees either the old
	 * valid pointer or NULL, never a freed pointer.
	 */
	rcu_assign_pointer(g_ice_slots, NULL);
	synchronize_rcu();
}

static const struct of_device_id qcom_ice_slots_of_match[] = {
	{ .compatible = "qcom,ice-keyslot-map" },
	{}
};
MODULE_DEVICE_TABLE(of, qcom_ice_slots_of_match);

static struct platform_driver qcom_ice_slots_driver = {
	.probe  = qcom_ice_slots_probe,
	.remove = qcom_ice_slots_remove,
	.driver = {
		.name           = "qcom-ice-slots",
		.of_match_table = qcom_ice_slots_of_match,
	},
};
module_platform_driver(qcom_ice_slots_driver);

MODULE_DESCRIPTION("Qualcomm ICE keyslot partitioning for guest VMs");
MODULE_LICENSE("GPL");
