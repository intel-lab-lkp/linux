// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hygon Family 0x18 Data Fabric node enumeration
 *
 * A DF instance exposes sibling PCI functions in one slot. Function 3
 * is the misc device used for enumeration, function 4 is the link
 * device, and functions 1 and 5 provide the socket and DF identity.
 *
 * Enumerate F3 devices by PCI ID, validate their F4 siblings, read each
 * node's socket ID and DFID, and validate the enumerated socket set
 * against SocketPresent. Sort the nodes into a dense cache with Core
 * and DDR Dies (CDDs) before I/O Dies (IODs), then by socket ID and
 * DFID.
 */

#define pr_fmt(fmt) "hygon_node: " fmt

#include <linux/bitops.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/pci_ids.h>
#include <linux/processor.h>
#include <linux/slab.h>
#include <linux/sort.h>

#include <asm/cpu_device_id.h>
#include <asm/hygon/node.h>

/* Maximum socket count this implementation supports. */
#define HYGON_MAX_SOCKETS	8

/*
 * DF register offsets used for node identity discovery.
 *
 * F1x200 (SystemCfg) -- present on all models:
 *   [30:28]  MySocketId    - hardware socket ID
 *   [23:20]  MyDieId       - die ID (equals DFID on some models, see below)
 *
 * F5x180 (FabricBlockInstanceInformation3_CS) -- Model 06h-08h only:
 *   [19:16]  DFID          - Data Fabric ID for UMC/SMN addressing
 *
 * DFID source by model:
 *   Model 04h/05h:  F1x200[23:20] (MyDieId == DFID)
 *   Model 06h-08h:  F5x180[19:16] (MyDieId != DFID, different numbering)
 */
#define DF_F1_SYSTEM_CFG	0x200
#define DF_F5_FABRIC_ID		0x180

/* DF function numbers for sibling device access (internal use). */
#define HYGON_DF_F1	1	/* SystemCfg: socket and die identity */
#define HYGON_DF_F5	5	/* FabricId: DFID on Model 06h-08h */

/* DF sibling device IDs used only within this file for identity reads. */
#define PCI_DEVICE_ID_HYGON_18H_M04H_DF_F1	0x1491
#define PCI_DEVICE_ID_HYGON_18H_M05H_DF_F1	0x14b1
#define PCI_DEVICE_ID_HYGON_18H_M06H_DF_F5	0x14b5

/*
 * Cached identity for one DF instance.  After sorting, CDDs occupy
 * nodes[0..num_cdd-1].
 *
 * The PCI BDF is the access point for a DF node, not its identity.
 * socket_id and dfid, read from DF registers, together identify the
 * node in hardware.
 */
struct hygon_node {
	struct pci_dev	*misc;		/* DF function 3 */
	struct pci_dev	*link;		/* DF function 4 */
	u8		socket_id;	/* F1x200[30:28] */
	u8		dfid;		/* model-dependent DFID */
	bool		is_cdd;		/* DFID >= 4 */
};

struct hygon_node_cache {
	struct hygon_node	*nodes;		/* sorted: CDD first, then IOD */
	u16			num_nodes;	/* CDD + IOD = total */
	u16			num_cdd;	/* CDD only */
	u16			num_sockets;

	/* Set after DF node collection, sorting and validation complete. */
	bool			ready;
};

struct hygon_df_id {
	u8	socket_id;
	u8	dfid;
};

/* DF sibling device IDs used to read node identity. */
struct hygon_df_cfg {
	u16	f1_id;
	u16	f5_id;			/* 0 = not available */
};

/* DF misc (F3) device IDs for all supported Hygon Family 0x18 models. */
static const struct pci_device_id hygon_nb_misc_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_HYGON, PCI_DEVICE_ID_HYGON_18H_M04H_DF_F3) },
	{ PCI_DEVICE(PCI_VENDOR_ID_HYGON, PCI_DEVICE_ID_HYGON_18H_M04H_DF_F3B) },
	{ PCI_DEVICE(PCI_VENDOR_ID_HYGON, PCI_DEVICE_ID_HYGON_18H_M05H_DF_F3) },
	{}
};

/* DF link (F4) device IDs, parallel to hygon_nb_misc_ids[]. */
static const struct pci_device_id hygon_nb_link_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_HYGON, PCI_DEVICE_ID_HYGON_18H_M04H_DF_F4) },
	{ PCI_DEVICE(PCI_VENDOR_ID_HYGON, PCI_DEVICE_ID_HYGON_18H_M04H_DF_F4B) },
	{ PCI_DEVICE(PCI_VENDOR_ID_HYGON, PCI_DEVICE_ID_HYGON_18H_M05H_DF_F4) },
	{}
};

static const struct hygon_df_cfg hygon_m04_df_cfg __initconst = {
	.f1_id	= PCI_DEVICE_ID_HYGON_18H_M04H_DF_F1,
};

static const struct hygon_df_cfg hygon_m05_df_cfg __initconst = {
	.f1_id	= PCI_DEVICE_ID_HYGON_18H_M05H_DF_F1,
};

static const struct hygon_df_cfg hygon_m06_m08_df_cfg __initconst = {
	.f1_id	= PCI_DEVICE_ID_HYGON_18H_M05H_DF_F1,
	.f5_id	= PCI_DEVICE_ID_HYGON_18H_M06H_DF_F5,
};

static const struct x86_cpu_id hygon_df_cpuids[] __initconst = {
	X86_MATCH_VENDOR_FAM_MODEL(HYGON, 0x18, 0x04, &hygon_m04_df_cfg),
	X86_MATCH_VENDOR_FAM_MODEL(HYGON, 0x18, 0x05, &hygon_m05_df_cfg),
	X86_MATCH_VENDOR_FAM_MODEL(HYGON, 0x18, 0x06, &hygon_m06_m08_df_cfg),
	X86_MATCH_VENDOR_FAM_MODEL(HYGON, 0x18, 0x07, &hygon_m06_m08_df_cfg),
	X86_MATCH_VENDOR_FAM_MODEL(HYGON, 0x18, 0x08, &hygon_m06_m08_df_cfg),
	{}
};

static struct hygon_node_cache hygon_cache;

static void __init hygon_dump_nodes(const struct hygon_node_cache *cache,
				    const char *phase)
{
	u16 i;

	pr_debug("%s: %u nodes\n", phase, cache->num_nodes);

	for (i = 0; i < cache->num_nodes; i++) {
		const struct hygon_node *node = &cache->nodes[i];

		pr_debug("%s: node[%u] %04x:%02x:%02x.%u socket=%u dfid=%u type=%s\n",
			 phase, i, pci_domain_nr(node->misc->bus),
			 node->misc->bus->number,
			 PCI_SLOT(node->misc->devfn),
			 PCI_FUNC(node->misc->devfn),
			 node->socket_id, node->dfid,
			 node->is_cdd ? "CDD" : "IOD");
	}
}

/*
 * Iterate Hygon PCI devices, returning the next one that matches @ids.
 * Follows the pci_get_device() convention: @from is consumed (its
 * reference is dropped) and the returned device has an elevated
 * reference count.
 */
static struct pci_dev * __init next_hygon_dev(struct pci_dev *from,
					      const struct pci_device_id *ids)
{
	while ((from = pci_get_device(PCI_VENDOR_ID_HYGON, PCI_ANY_ID, from))) {
		if (pci_match_id(ids, from))
			return from;
	}

	return NULL;
}

/*
 * Find the DF link (function 4) sibling of a DF misc (function 3)
 * device.  Both functions share the same PCI bus and slot.
 */
static struct pci_dev * __init hygon_get_link(struct pci_dev *misc)
{
	struct pci_dev *link;

	link = pci_get_domain_bus_and_slot(pci_domain_nr(misc->bus),
					   misc->bus->number,
					   PCI_DEVFN(PCI_SLOT(misc->devfn),
						     HYGON_DF_F4));
	if (!link)
		return NULL;

	if (!pci_match_id(hygon_nb_link_ids, link)) {
		pci_dev_put(link);
		return NULL;
	}

	return link;
}

/* Find the DF configuration for the boot CPU. */
static const struct hygon_df_cfg * __init hygon_get_df_cfg(void)
{
	const struct x86_cpu_id *id = x86_match_cpu(hygon_df_cpuids);

	return id ? (const void *)id->driver_data : NULL;
}

/*
 * Read a config register from a DF sibling function on the same PCI
 * slot as @misc.  Only functions 1 (F1, SystemCfg) and 5 (F5,
 * FabricId) are supported.
 */
static int __init hygon_read_df_reg(struct pci_dev *misc, u8 func,
				    int offset, u32 *value)
{
	const struct hygon_df_cfg *cfg;
	struct pci_dev *sibling;
	u16 expected_device;
	int err;

	cfg = hygon_get_df_cfg();
	if (!cfg)
		return -ENODEV;

	if (func == HYGON_DF_F1) {
		expected_device = cfg->f1_id;

		/*
		 * Model 5 can expose an older mixed-silicon variant where
		 * the F1 sibling still uses the M04H device ID.
		 */
		if (boot_cpu_data.x86_model == 0x5 &&
		    misc->device != PCI_DEVICE_ID_HYGON_18H_M05H_DF_F3)
			expected_device = PCI_DEVICE_ID_HYGON_18H_M04H_DF_F1;
	} else if (func == HYGON_DF_F5) {
		expected_device = cfg->f5_id;
	} else {
		return -EINVAL;
	}

	if (!expected_device)
		return -ENODEV;

	sibling = pci_get_domain_bus_and_slot(pci_domain_nr(misc->bus),
					      misc->bus->number,
					      PCI_DEVFN(PCI_SLOT(misc->devfn),
							func));
	if (!sibling)
		return -ENODEV;

	if (sibling->vendor != PCI_VENDOR_ID_HYGON ||
	    sibling->device != expected_device) {
		pci_dev_put(sibling);
		return -ENODEV;
	}

	err = pci_read_config_dword(sibling, offset, value);
	pci_dev_put(sibling);

	if (err) {
		pr_warn("error reading %04x:%02x:%02x.%u offset 0x%x\n",
			pci_domain_nr(misc->bus), misc->bus->number,
			PCI_SLOT(misc->devfn), func, offset);
		return pcibios_err_to_errno(err);
	}

	/* Do not parse a PCI error response as DF identity. */
	if (PCI_POSSIBLE_ERROR(*value)) {
		pr_warn("error response reading %04x:%02x:%02x.%u offset 0x%x\n",
			pci_domain_nr(misc->bus), misc->bus->number,
			PCI_SLOT(misc->devfn), func, offset);
		return -ENODEV;
	}

	return 0;
}

/*
 * Read the hardware identity for one DF misc device from its sibling
 * functions.
 *
 * All models expose F1x200 (SystemCfg): socket_id from [30:28] and a
 * die identifier (MyDieId) from [23:20].
 * On Model 06h-08h MyDieId differs from the DFID used by UMC and SMN
 * addressing, so an additional F5x180 (FabricId) read obtains the DFID
 * from [19:16].
 *
 * All DF instances on a Hygon system are the same model, so
 * boot_cpu_data.x86_model is representative for all devices.
 */
static int __init hygon_read_df_id(struct pci_dev *misc,
				   struct hygon_df_id *id)
{
	const struct hygon_df_cfg *cfg = hygon_get_df_cfg();
	u32 reg;
	int ret;

	if (!cfg)
		return -ENODEV;

	ret = hygon_read_df_reg(misc, HYGON_DF_F1, DF_F1_SYSTEM_CFG, &reg);
	if (ret)
		return ret;

	id->socket_id = (reg >> 28) & 0x7;
	id->dfid      = (reg >> 20) & 0xf;

	/* Read DFID from F5x180 on models that provide an F5 sibling. */
	if (cfg->f5_id) {
		ret = hygon_read_df_reg(misc, HYGON_DF_F5, DF_F5_FABRIC_ID,
					&reg);
		if (ret)
			return ret;
		id->dfid = (reg >> 16) & 0xf;
	}

	return 0;
}

/*
 * Read the system-wide SocketPresent mask from F1x200[7:0].
 *
 * SocketPresent is identical for every DF instance. Read it once from
 * the first DF misc device. A zero mask is invalid on supported models.
 */
static int __init hygon_read_socket_present(struct pci_dev *misc,
					    u8 *socket_present)
{
	u32 reg;
	int ret;

	ret = hygon_read_df_reg(misc, HYGON_DF_F1, DF_F1_SYSTEM_CFG, &reg);
	if (ret)
		return ret;

	*socket_present = reg & 0xff;
	if (!*socket_present) {
		pr_warn("SocketPresent is zero\n");
		return -EINVAL;
	}

	return 0;
}

static void __init hygon_release_nodes(struct hygon_node *nodes, u16 count)
{
	u16 i;

	for (i = 0; i < count; i++) {
		pci_dev_put(nodes[i].misc);
		pci_dev_put(nodes[i].link);
	}

	kfree(nodes);
}

/*
 * Walk all DF misc (F3) devices and read per-node identity (socket_id,
 * dfid) from each, collecting them into a flat array.  The system-wide
 * SocketPresent mask is sampled once, and the enumerated socket set is
 * validated against it; socket IDs must also be dense (0..N-1).
 */
static int __init hygon_collect_nodes(struct hygon_node_cache *cache)
{
	struct hygon_node *nodes;
	struct pci_dev *misc;
	u16 capacity = 0, count = 0;
	u8 observed_socket_mask = 0;
	u8 socket_present_mask = 0;
	int ret;

	misc = NULL;
	while ((misc = next_hygon_dev(misc, hygon_nb_misc_ids)))
		capacity++;

	if (!capacity)
		return -ENODEV;

	nodes = kcalloc(capacity, sizeof(*nodes), GFP_KERNEL);
	if (!nodes)
		return -ENOMEM;

	misc = NULL;
	while ((misc = next_hygon_dev(misc, hygon_nb_misc_ids))) {
		struct hygon_df_id id;
		struct pci_dev *link;

		link = hygon_get_link(misc);
		if (!link) {
			pci_dev_put(misc);
			ret = -ENODEV;
			goto fail;
		}

		ret = hygon_read_df_id(misc, &id);
		if (ret) {
			pci_dev_put(link);
			pci_dev_put(misc);
			goto fail;
		}

		/* SocketPresent is system-wide, so sample it once. */
		if (!count) {
			ret = hygon_read_socket_present(misc,
							&socket_present_mask);
			if (ret) {
				pci_dev_put(link);
				pci_dev_put(misc);
				goto fail;
			}
		}

		if (count >= capacity) {
			pci_dev_put(link);
			pci_dev_put(misc);
			ret = -ENODEV;
			goto fail;
		}

		pr_debug("collect: %04x:%02x:%02x.%u socket=%u dfid=%u\n",
			 pci_domain_nr(misc->bus), misc->bus->number,
			 PCI_SLOT(misc->devfn), PCI_FUNC(misc->devfn),
			 id.socket_id, id.dfid);

		nodes[count].misc      = pci_dev_get(misc);
		nodes[count].link      = link;
		nodes[count].socket_id = id.socket_id;
		nodes[count].dfid      = id.dfid;
		nodes[count].is_cdd    = (id.dfid >= HYGON_CDD_DFID_BASE);
		count++;

		observed_socket_mask |= BIT(id.socket_id);
	}

	if (count != capacity) {
		pr_warn("DF enumeration changed: expected %u nodes, got %u\n",
			capacity, count);
		ret = -ENODEV;
		goto fail;
	}

	/*
	 * Every populated socket must contribute at least one enumerated DF
	 * misc device. socket_present_mask is guaranteed to be non-zero.
	 */
	if (observed_socket_mask != socket_present_mask) {
		pr_warn("SocketPresent mismatch: observed=0x%x present=0x%x\n",
			observed_socket_mask, socket_present_mask);
		ret = -EINVAL;
		goto fail;
	}

	cache->nodes       = nodes;
	cache->num_nodes   = count;
	cache->num_sockets = hweight8(socket_present_mask);

	if (socket_present_mask != GENMASK(cache->num_sockets - 1, 0)) {
		pr_warn("sparse socket IDs not supported (SocketPresent=0x%x)\n",
			socket_present_mask);
		ret = -EINVAL;
		goto fail;
	}

	return 0;

fail:
	hygon_release_nodes(nodes, count);
	cache->nodes = NULL;
	cache->num_nodes = 0;
	return ret;
}

/*
 * Sort CDD nodes before IOD nodes, then order by hardware
 * (socket_id, dfid).
 */
static int __init hygon_node_cmp(const void *a, const void *b)
{
	const struct hygon_node *left = a;
	const struct hygon_node *right = b;

	if (left->is_cdd != right->is_cdd)
		return right->is_cdd - left->is_cdd;

	if (left->socket_id != right->socket_id)
		return (int)left->socket_id - right->socket_id;

	return (int)left->dfid - (int)right->dfid;
}

/*
 * Classify the sorted node array and validate the CDD layout.
 *
 * A dense CDD mapping requires every socket to contribute the same
 * number of compute dies.
 */
static int __init hygon_sort_and_classify(struct hygon_node_cache *cache)
{
	u16 cdd_per_socket;
	u16 i;
	u8 per_sock_count[HYGON_MAX_SOCKETS] = { 0 };

	hygon_dump_nodes(cache, "before-sort");

	sort(cache->nodes, cache->num_nodes, sizeof(*cache->nodes),
	     hygon_node_cmp, NULL);

	for (i = 1; i < cache->num_nodes; i++) {
		const struct hygon_node *prev = &cache->nodes[i - 1];
		const struct hygon_node *node = &cache->nodes[i];

		if (node->socket_id == prev->socket_id &&
		    node->dfid == prev->dfid) {
			pr_warn("duplicate DF node: socket=%u dfid=%u\n",
				node->socket_id, node->dfid);
			return -EINVAL;
		}
	}

	for (i = 0; i < cache->num_nodes; i++) {
		if (!cache->nodes[i].is_cdd)
			break;
	}

	cache->num_cdd = i;

	if (!cache->num_cdd)
		return -ENODEV;

	if (cache->num_cdd % cache->num_sockets) {
		pr_warn("CDD count %u not divisible by %u sockets\n",
			cache->num_cdd, cache->num_sockets);
		return -EINVAL;
	}

	cdd_per_socket = cache->num_cdd / cache->num_sockets;
	if (!cdd_per_socket)
		return -EINVAL;

	for (i = 0; i < cache->num_cdd; i++) {
		u8 socket_id = cache->nodes[i].socket_id;

		if (socket_id >= cache->num_sockets)
			return -EINVAL;
		per_sock_count[socket_id]++;
	}

	for (i = 0; i < cache->num_sockets; i++) {
		if (per_sock_count[i] != cdd_per_socket) {
			pr_warn("socket %u: %u CDDs, expected %u\n",
				i, per_sock_count[i], cdd_per_socket);
			return -EINVAL;
		}
	}

	hygon_dump_nodes(cache, "after-sort");

	return 0;
}

/*
 * Build the global DF node cache.
 *
 * Called once from hygon_node_init() at fs_initcall, so no locking is
 * required. Set ready after node collection and sorting.
 */
static int __init hygon_build_cache(void)
{
	int err;

	err = hygon_collect_nodes(&hygon_cache);
	if (err)
		return err;

	err = hygon_sort_and_classify(&hygon_cache);
	if (err)
		goto fail;

	hygon_cache.ready = true;

	return 0;

fail:
	hygon_release_nodes(hygon_cache.nodes, hygon_cache.num_nodes);
	hygon_cache.nodes = NULL;
	hygon_cache.num_nodes = 0;
	hygon_cache.num_cdd = 0;
	hygon_cache.num_sockets = 0;
	return err;
}

u16 hygon_node_num(void)
{
	return hygon_cache.ready ? hygon_cache.num_nodes : 0;
}
EXPORT_SYMBOL_GPL(hygon_node_num);

u16 hygon_cdd_num(void)
{
	return hygon_cache.ready ? hygon_cache.num_cdd : 0;
}
EXPORT_SYMBOL_GPL(hygon_cdd_num);

int hygon_node_get_info(u16 node, struct hygon_node_info *info)
{
	const struct hygon_node *n;

	if (!info)
		return -EINVAL;

	if (!hygon_cache.ready)
		return -ENODEV;

	if (node >= hygon_cache.num_nodes)
		return -EINVAL;

	n = &hygon_cache.nodes[node];
	info->socket_id = n->socket_id;
	info->dfid      = n->dfid;
	info->type      = n->is_cdd ? HYGON_NODE_CDD : HYGON_NODE_IOD;
	return 0;
}
EXPORT_SYMBOL_GPL(hygon_node_get_info);

struct pci_dev *hygon_node_get_func(u16 node, u8 func)
{
	if (!hygon_cache.ready)
		return NULL;

	if (node >= hygon_cache.num_nodes)
		return NULL;

	switch (func) {
	case HYGON_DF_F3:
		return pci_dev_get(hygon_cache.nodes[node].misc);
	case HYGON_DF_F4:
		return pci_dev_get(hygon_cache.nodes[node].link);
	default:
		return NULL;
	}
}
EXPORT_SYMBOL_GPL(hygon_node_get_func);

int hygon_pci_dev_to_df_node(struct pci_dev *pdev)
{
	u16 i;

	if (!pdev)
		return -EINVAL;

	if (!hygon_cache.ready)
		return -ENODEV;

	for (i = 0; i < hygon_cache.num_nodes; i++) {
		struct pci_dev *misc = hygon_cache.nodes[i].misc;

		if (pci_domain_nr(misc->bus) == pci_domain_nr(pdev->bus) &&
		    misc->bus->number       == pdev->bus->number      &&
		    PCI_SLOT(misc->devfn)   == PCI_SLOT(pdev->devfn))
			return i;
	}

	return -ENODEV;
}
EXPORT_SYMBOL_GPL(hygon_pci_dev_to_df_node);

/*
 * Build the Hygon DF node cache at fs_initcall.
 */
static int __init hygon_node_init(void)
{
	int ret;

	if (!hygon_get_df_cfg()) {
		if (is_hygon_f18h())
			pr_info("Hygon Fam18h model 0x%x is not supported by the node layer\n",
				boot_cpu_data.x86_model);
		return 0;
	}

	ret = hygon_build_cache();
	if (ret) {
		pr_warn("DF node cache build failed: %d\n", ret);
		return ret;
	}

	return 0;
}
fs_initcall(hygon_node_init);
