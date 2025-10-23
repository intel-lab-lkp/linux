// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)

#include <linux/irq.h>
#include <linux/module.h>
#include <linux/ntb.h>
#include <linux/msi.h>
#include <linux/pci.h>

struct ntb_msi {
	u64 base_addr;
	u64 end_addr;

	void (*desc_changed)(void *ctx);

	u32 __iomem *peer_mws[];
};

/**
 * ntb_msi_init() - Initialize the MSI context
 * @ntb:	NTB device context
 *
 * This function must be called before any other ntb_msi function.
 * It initializes the context for MSI operations and maps
 * the peer memory windows.
 *
 * This function reserves the last N outbound memory windows (where N
 * is the number of peers).
 *
 * Return: Zero on success, otherwise a negative error number.
 */
static int ntb_msi_init(struct ntb_dev *ntb,
			void (*desc_changed)(void *ctx))
{
	phys_addr_t mw_phys_addr;
	resource_size_t mw_size;
	struct ntb_msi *msi;
	int peer_widx;
	int peers;
	int ret;
	int i;

	peers = ntb_peer_port_count(ntb);
	if (peers <= 0)
		return -EINVAL;

	msi = devm_kzalloc(&ntb->dev, struct_size(msi, peer_mws, peers),
				GFP_KERNEL);
	if (!msi)
		return -ENOMEM;

	msi->desc_changed = desc_changed;

	for (i = 0; i < peers; i++) {
		peer_widx = ntb_peer_mw_count(ntb) - 1 - i;

		ret = ntb_peer_mw_get_addr(ntb, peer_widx, &mw_phys_addr,
					   &mw_size);
		if (ret)
			goto unroll;

		msi->peer_mws[i] = devm_ioremap(&ntb->dev, mw_phys_addr,
						     mw_size);
		if (!msi->peer_mws[i]) {
			ret = -EFAULT;
			goto unroll;
		}
	}

	ntb->intr_priv = msi;

	return 0;

unroll:
	for (i = 0; i < peers; i++)
		if (msi->peer_mws[i])
			devm_iounmap(&ntb->dev, msi->peer_mws[i]);

	devm_kfree(&ntb->dev, msi);
	return ret;
}

/**
 * ntb_msi_setup_mws() - Initialize the MSI inbound memory windows
 * @ntb:	NTB device context
 *
 * This function sets up the required inbound memory windows. It should be
 * called from a work function after a link up event.
 *
 * Over the entire network, this function will reserves the last N
 * inbound memory windows for each peer (where N is the number of peers).
 *
 * ntb_msi_init() must be called before this function.
 *
 * Return: Zero on success, otherwise a negative error number.
 */
static int ntb_msi_setup_mws(struct ntb_dev *ntb)
{
	struct msi_desc *desc;
	u64 addr;
	int peer, peer_widx;
	resource_size_t addr_align, size_align, offset;
	resource_size_t mw_size = SZ_32K;
	resource_size_t mw_min_size = mw_size;
	struct ntb_msi *msi = ntb->intr_priv;
	int i;
	int ret;

	if (!msi)
		return -EINVAL;

	if (msi->base_addr)
		return 0;

	scoped_guard (msi_descs_lock, &ntb->pdev->dev) {
		desc = msi_first_desc(&ntb->pdev->dev, MSI_DESC_ASSOCIATED);
		addr = desc->msg.address_lo + ((uint64_t)desc->msg.address_hi << 32);
	}

	for (peer = 0; peer < ntb_peer_port_count(ntb); peer++) {
		peer_widx = ntb_peer_highest_mw_idx(ntb, peer);
		if (peer_widx < 0)
			return peer_widx;

		ret = ntb_mw_get_align(ntb, peer, peer_widx, &addr_align,
				       NULL, NULL, NULL);
		if (ret)
			return ret;

		addr &= ~(addr_align - 1);
	}

	for (peer = 0; peer < ntb_peer_port_count(ntb); peer++) {
		peer_widx = ntb_peer_highest_mw_idx(ntb, peer);
		if (peer_widx < 0) {
			ret = peer_widx;
			goto error_out;
		}

		ret = ntb_mw_get_align(ntb, peer, peer_widx, NULL,
				       &size_align, NULL, &offset);
		if (ret)
			goto error_out;

		mw_size = round_up(mw_size, size_align);
		if (mw_size < mw_min_size)
			mw_min_size = mw_size;

		ret = ntb_mw_set_trans(ntb, peer, peer_widx,
				       addr, mw_size, offset);
		if (ret)
			goto error_out;
	}

	msi->base_addr = addr;
	msi->end_addr = addr + mw_min_size;

	return 0;

error_out:
	for (i = 0; i < peer; i++) {
		peer_widx = ntb_peer_highest_mw_idx(ntb, peer);
		if (peer_widx < 0)
			continue;

		ntb_mw_clear_trans(ntb, i, peer_widx);
	}

	return ret;
}

/**
 * ntb_msi_clear_mws() - Clear all inbound memory windows
 * @ntb:	NTB device context
 *
 * This function tears down the resources used by ntb_msi_setup_mws().
 */
static void ntb_msi_clear_mws(struct ntb_dev *ntb)
{
	int peer;
	int peer_widx;

	for (peer = 0; peer < ntb_peer_port_count(ntb); peer++) {
		peer_widx = ntb_peer_highest_mw_idx(ntb, peer);
		if (peer_widx < 0)
			continue;

		ntb_mw_clear_trans(ntb, peer, peer_widx);
	}
}

struct ntb_msi_devres {
	struct ntb_dev *ntb;
	struct msi_desc *entry;
	struct ntb_intr_desc *intr_desc;
};

static int ntb_msi_set_desc(struct ntb_dev *ntb, struct msi_desc *entry,
			    struct ntb_intr_desc *intr_desc, u16 vector_offset)
{
	struct ntb_msi *msi = ntb->intr_priv;
	u64 addr;

	addr = entry->msg.address_lo +
		((uint64_t)entry->msg.address_hi << 32);

	if (addr < msi->base_addr || addr >= msi->end_addr) {
		dev_warn_once(&ntb->dev,
			      "IRQ %d: MSI Address not within the memory window (%llx, [%llx %llx])\n",
			      entry->irq, addr, msi->base_addr,
			      msi->end_addr);
		return -EFAULT;
	}

	intr_desc->addr_offset = addr - msi->base_addr;
	intr_desc->data = entry->msg.data + vector_offset;
	intr_desc->vector_offset = vector_offset;

	return 0;
}

static void ntb_msi_write_msg(struct msi_desc *entry, void *data)
{
	struct ntb_msi_devres *dr = data;
	struct ntb_msi *msi = dr->ntb->intr_priv;

	WARN_ON(ntb_msi_set_desc(dr->ntb, entry, dr->intr_desc,
				 dr->intr_desc->vector_offset));

	if (msi->desc_changed)
		msi->desc_changed(dr->ntb->ctx);
}

static void ntbm_msi_callback_release(struct device *dev, void *res)
{
	struct ntb_msi_devres *dr = res;

	dr->entry->write_msi_msg = NULL;
	dr->entry->write_msi_msg_data = NULL;
}

static int ntbm_msi_setup_callback(struct ntb_dev *ntb, struct msi_desc *entry,
				   struct ntb_intr_desc *intr_desc)
{
	struct ntb_msi_devres *dr;

	dr = devres_alloc(ntbm_msi_callback_release,
			  sizeof(struct ntb_msi_devres), GFP_KERNEL);
	if (!dr)
		return -ENOMEM;

	dr->ntb = ntb;
	dr->entry = entry;
	dr->intr_desc = intr_desc;

	devres_add(&ntb->dev, dr);

	dr->entry->write_msi_msg = ntb_msi_write_msg;
	dr->entry->write_msi_msg_data = dr;

	return 0;
}

/**
 * ntb_msi_request_irq() - allocate an MSI interrupt
 * @ntb:	NTB device context
 * @handler:	Function to be called when the IRQ occurs
 * @name:	An ascii name for the claiming device, dev_name(dev) if NULL
 * @dev_id:	A cookie passed back to the handler function
 * @intr_desc:	Generic interrupt descriptor
 *
 * This function assigns an interrupt handler to an unused
 * MSI interrupt and returns the descriptor used to trigger
 * it. The descriptor can then be sent to a peer to trigger
 * the interrupt.
 *
 * The interrupt resource is managed with devres so it will
 * be automatically freed when the NTB device is torn down.
 *
 * If an IRQ allocated with this function needs to be freed
 * separately, ntbm_free_irq() must be used.
 *
 * Return: IRQ number assigned on success, otherwise a negative error number.
 */
static int ntb_msi_request_irq(struct ntb_dev *ntb, irq_handler_t handler,
			       const char *name, void *dev_id,
			       struct ntb_intr_desc *intr_desc)
{
	struct device *dev = &ntb->pdev->dev;
	struct msi_desc *entry;
	unsigned int virq;
	int ret, i;

	guard(msi_descs_lock)(dev);
	msi_for_each_desc(entry, dev, MSI_DESC_ASSOCIATED) {
		for (i = 0; i < entry->nvec_used; i++) {
			virq = entry->irq + i;
			if (irq_has_action(virq))
				continue;

			ret = devm_request_irq(&ntb->dev, virq, handler,
					       0, name, dev_id);
			if (ret)
				continue;

			if (ntb_msi_set_desc(ntb, entry, intr_desc, i)) {
				devm_free_irq(&ntb->dev, virq, dev_id);
				continue;
			}

			ret = ntbm_msi_setup_callback(ntb, entry, intr_desc);
			if (ret) {
				devm_free_irq(&ntb->dev, virq, dev_id);
				return ret;
			}
			return virq;
		}
	}
	return -ENODEV;
}

/**
 * ntb_msi_free_irq() - free an MSI interrupt
 * @ntb:	NTB device context
 * @irq:	IRQ number assigned
 * @dev_id:	A cookie passed back to the handler function
 * @desc:	Generic interrupt descriptor
 *
 * Free an IRQ assigned by ntb_msi_request_irq().
 *
 * Return: void
 */
static void ntb_msi_free_irq(struct ntb_dev *ntb, int irq, void *dev_id,
			     struct ntb_intr_desc *desc)
{
	devm_free_irq(&ntb->dev, irq, dev_id);
}

/**
 * ntb_msi_peer_trigger() - Trigger an interrupt handler on a peer
 * @ntb:	NTB device context
 * @peer:	Peer index
 * @desc:	MSI descriptor data which triggers the interrupt
 *
 * This function triggers an interrupt on a peer. It requires
 * the descriptor structure to have been passed from that peer
 * by some other means.
 *
 * Return: Zero on success, otherwise a negative error number.
 */
static int ntb_msi_peer_trigger(struct ntb_dev *ntb, int peer,
				struct ntb_intr_desc *desc)
{
	struct ntb_msi *msi = ntb->intr_priv;
	int idx;

	idx = desc->addr_offset / sizeof(*msi->peer_mws[peer]);

	iowrite32(desc->data, &msi->peer_mws[peer][idx]);

	return 0;
}

static const struct ntb_intr_backend ntb_intr_backend_msi = {
	.name = "msi",
	.init = ntb_msi_init,
	.setup_mws = ntb_msi_setup_mws,
	.clear_mws = ntb_msi_clear_mws,
	.request_irq = ntb_msi_request_irq,
	.free_irq = ntb_msi_free_irq,
	.peer_trigger = ntb_msi_peer_trigger,
};

const struct ntb_intr_backend *ntb_intr_msi_backend(void)
{
	return &ntb_intr_backend_msi;
}
