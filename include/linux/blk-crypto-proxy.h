/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __LINUX_BLK_CRYPTO_PROXY_H
#define __LINUX_BLK_CRYPTO_PROXY_H

#include <uapi/linux/blk-crypto-proxy.h>
#include <linux/types.h>

struct blk_crypto_profile;

/**
 * struct bcp_hypervisor_ops - hypervisor VM identity operations
 *
 * Translates a hypervisor-specific VM fd to the opaque u32 vm_id used
 * throughout blk-crypto-proxy.  Register once at module init time.
 */
struct bcp_hypervisor_ops {
	/**
	 * @get_guest_id: Resolve @vm_fd to an opaque guest identifier.
	 *
	 * Verify the caller is permitted to act on behalf of the VM and write
	 * its u32 id to @guest_id_out.  The value is passed verbatim to
	 * bcp_slot_virt_ops callbacks.
	 *
	 * Returns 0 on success, -errno on failure.
	 */
	int (*get_guest_id)(int vm_fd, u32 *guest_id_out);
};

/**
 * bcp_register_hypervisor_ops() - register the hypervisor op-set
 * @ops: op-set to register; must remain valid until unregistered.
 *
 * Returns 0 on success, -EBUSY if an op-set is already registered.
 */
int bcp_register_hypervisor_ops(const struct bcp_hypervisor_ops *ops);

/**
 * bcp_unregister_hypervisor_ops() - unregister the hypervisor op-set
 * @ops: must be the pointer that was passed to bcp_register_hypervisor_ops().
 *
 * Blocks until all in-flight callers have finished, then clears the
 * registration.  Safe to call from module exit.
 */
void bcp_unregister_hypervisor_ops(const struct bcp_hypervisor_ops *ops);

/**
 * struct bcp_slot_virt_ops - ICE keyslot virtualization operations
 *
 * Per-VM ICE keyslot accounting and virtual-to-physical slot translation.
 * The implementation owns the slot allocation table and is registered once
 * at platform driver probe time.
 *
 * @profile is passed to every callback so an implementation supporting
 * multiple storage controllers can distinguish between them.
 *
 * All callbacks may be called concurrently and must not sleep (called
 * under RCU read lock).
 */
struct bcp_slot_virt_ops {
	/**
	 * @get_guest_slots: Return the number of ICE keyslots allocated to @guest_id.
	 *
	 * Returns the slot count (≥ 1) on success, -ENOKEY if @guest_id is
	 * not in the allocation table.
	 */
	int (*get_guest_slots)(struct blk_crypto_profile *profile, u32 guest_id);

	/**
	 * @vslot_to_pslot: Translate a VM-local virtual slot to a physical slot.
	 * @guest_id:       hypervisor-assigned VM identifier.
	 * @virt_slot:   0-based slot index within @guest_id's allocation.
	 * @phy_slot_out: receives the physical ICE keyslot index on success.
	 *
	 * Returns 0 on success, -ENOKEY if @guest_id is unknown, -EINVAL if
	 * @virt_slot >= the VM's allocation.
	 */
	int (*vslot_to_pslot)(struct blk_crypto_profile *profile,
			      u32 guest_id, u32 virt_slot,
			      unsigned int *phy_slot_out);
};

/**
 * bcp_register_slot_virt_ops() - register the slot-virt op-set
 * @ops: op-set to register; must remain valid until unregistered.
 *
 * Returns 0 on success, -EBUSY if an op-set is already registered.
 */
int bcp_register_slot_virt_ops(const struct bcp_slot_virt_ops *ops);

/**
 * bcp_unregister_slot_virt_ops() - unregister the slot-virt op-set
 * @ops: must be the pointer passed to bcp_register_slot_virt_ops().
 *
 * Blocks until all in-flight callers have finished, then clears the
 * registration.  Safe to call from module exit.
 */
void bcp_unregister_slot_virt_ops(const struct bcp_slot_virt_ops *ops);

#endif /* __LINUX_BLK_CRYPTO_PROXY_H */
