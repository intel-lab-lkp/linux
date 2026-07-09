// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hygon Family 0x18 Data Fabric node enumeration
 *
 * AMD systems enumerate DF nodes at fixed PCI slots 00:18..1f on bus 0.
 * Hygon Fam18h places DF instances at platform-assigned slots with no
 * fixed relationship to node identity, so the AMD enumeration path in
 * amd_nb.c / amd_node.c cannot be used.
 *
 * This file provides:
 *
 *  - DF node enumeration: walk DF misc (F3) devices by PCI ID, read
 *    hardware identity from per-instance registers (F1x200 SystemCfg,
 *    F5x180 FabricId), and build a sorted node cache that classifies
 *    each node as CDD (compute, DFID >= 4) or IOD (I/O, DFID < 4).
 *
 *  - CPU-to-node mapping: map per-CPU phys_node_id (CPUID
 *    8000001E[7:0]) to a dense DF CDD index for lookup by EDAC,
 *    MCE decode, and ATL.
 *
 *  - DF function access: hygon_node_get_func() returns the misc (F3)
 *    or link (F4) pci_dev for a node, with a referenced return.
 */

#define pr_fmt(fmt) "hygon_node: " fmt

#include <linux/bitops.h>
#include <linux/cpu.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/pci_ids.h>
#include <linux/processor.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/topology.h>

#include <asm/cpuid/api.h>
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
 *   [19:16]  DFID          - real Data Fabric ID for UMC/SMN addressing
 *
 * DFID source by model:
 *   Model 04h/05h:  F1x200[23:20] (MyDieId == DFID)
 *   Model 06h-08h:  F5x180[19:16] (MyDieId != DFID, different numbering)
 */
#define DF_F1_SYSTEM_CFG	0x200
#define DF_F5_FABRIC_ID		0x180

/* DF function numbers for sibling device access (internal use). */
#define HYGON_DF_F1	1	/* SystemCfg: socket and die identity */
#define HYGON_DF_F5	5	/* FabricId: real DFID on Model 06h-08h */

/* DF sibling device IDs used only within this file for identity reads. */
#define PCI_DEVICE_ID_HYGON_18H_M04H_DF_F1	0x1491
#define PCI_DEVICE_ID_HYGON_18H_M05H_DF_F1	0x14b1
#define PCI_DEVICE_ID_HYGON_18H_M06H_DF_F5	0x14b5

/*
 * Cached identity for one DF instance.  After sorting, CDDs occupy
 * nodes[0..num_cdd-1].
 *
 * socket_id + dfid together uniquely identify a DF node in hardware.
 * Unlike AMD (fixed PCI slot == node ID), Hygon DF nodes sit at
 * platform-assigned slots, so consumers can use socket_id + dfid as
 * the hardware location of a DF node.
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
	u16			cdd_per_socket;	/* num_cdd / num_sockets */

	/*
	 * Set once the DF node cache, the identity / lookup APIs and DF
	 * function (F3/F4) PCI access are usable.  SMN read/write have a
	 * separate gate (hygon_smn_exclusive).
	 */
	bool			ready;

	/*
	 * Set when hygon_cpu_to_df_node() is supported on the running
	 * model.  Independent of @ready: the phys_node_id encoding is
	 * model-specific (supported on Hygon Fam18h Model 0x04-0x08), so
	 * other models keep the rest of the node stack working while
	 * hygon_cpu_to_df_node() returns -ENODEV.
	 */
	bool			cpu_map_supported;
};

struct hygon_df_id {
	u8	socket_id;
	u8	dfid;
};

/* Model-specific DF sibling device IDs for reading node identity. */
struct hygon_df_func_ids {
	u8	model_start;
	u8	model_end;
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

/*
 * Map Hygon Fam18h model ranges onto the DF sibling functions used to
 * read node identity.  Model 5 may still expose the Model 4 F1 device
 * id on mixed silicon, which is handled separately in
 * hygon_read_df_reg().
 */
static const struct hygon_df_func_ids hygon_df_table[] = {
	{ 0x04, 0x04, PCI_DEVICE_ID_HYGON_18H_M04H_DF_F1, 0 },
	{ 0x05, 0x05, PCI_DEVICE_ID_HYGON_18H_M05H_DF_F1, 0 },
	{ 0x06, 0x08, PCI_DEVICE_ID_HYGON_18H_M05H_DF_F1,
		      PCI_DEVICE_ID_HYGON_18H_M06H_DF_F5 },
	{}
};

/*
 * Global cache.  BSS-initialised: both ready flags are false until
 * hygon_build_cache() sets them.  Runtime APIs short-circuit on their
 * respective flag, so any caller arriving before fs_initcall sees a
 * documented failure value (0 / NULL / -ENODEV).
 */
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

/*
 * Look up the DF sibling device IDs for the current boot CPU model.
 *
 * This is the only place that decides node-layer model support: a
 * NULL return means the running CPU is not a Hygon Fam18h model the
 * node layer supports.  Range matching is kept private here; the node
 * layer does not expose a model-range helper.
 */
static const struct hygon_df_func_ids * __init hygon_get_df_func_ids(void)
{
	const struct hygon_df_func_ids *entry;
	u8 model;

	if (!is_hygon_f18h())
		return NULL;

	model = boot_cpu_data.x86_model;

	for (entry = hygon_df_table; entry->f1_id; entry++) {
		if (model >= entry->model_start && model <= entry->model_end)
			return entry;
	}

	return NULL;
}

/*
 * Read a config register from a DF sibling function on the same PCI
 * slot as @misc.  Only functions 1 (F1, SystemCfg) and 5 (F5,
 * FabricId) are supported.
 */
static int __init hygon_read_df_reg(struct pci_dev *misc, u8 func,
				    int offset, u32 *value)
{
	const struct hygon_df_func_ids *ids;
	struct pci_dev *sibling;
	u16 expected_device;
	int err;

	ids = hygon_get_df_func_ids();
	if (!ids)
		return -ENODEV;

	if (func == HYGON_DF_F1) {
		expected_device = ids->f1_id;

		/*
		 * Model 5 can expose an older mixed-silicon variant where
		 * the F1 sibling still uses the M04H device ID.
		 */
		if (boot_cpu_data.x86_model == 0x5 &&
		    misc->device != PCI_DEVICE_ID_HYGON_18H_M05H_DF_F3)
			expected_device = PCI_DEVICE_ID_HYGON_18H_M04H_DF_F1;
	} else if (func == HYGON_DF_F5) {
		expected_device = ids->f5_id;
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

	/*
	 * A successful config read can still return the all-ones PCI error
	 * response; reject it so 0xffffffff is never parsed as
	 * socket_id/dfid/SocketPresent.
	 */
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
 * addressing, so an additional F5x180 (FabricId) read is required to
 * obtain the real DFID from [19:16].
 *
 * All DF instances on a Hygon system are the same model, so
 * boot_cpu_data.x86_model is representative for all devices.
 */
static int __init hygon_read_df_id(struct pci_dev *misc,
				   struct hygon_df_id *id)
{
	const struct hygon_df_func_ids *ids = hygon_get_df_func_ids();
	u32 reg;
	int ret;

	if (!ids)
		return -ENODEV;

	/*
	 * F1x200; hygon_read_df_reg() resolves the F1 sibling device ID
	 * (including the Model 5 mixed-silicon variant) from the DF table.
	 */
	ret = hygon_read_df_reg(misc, HYGON_DF_F1, DF_F1_SYSTEM_CFG, &reg);
	if (ret)
		return ret;

	id->socket_id = (reg >> 28) & 0x7;
	id->dfid      = (reg >> 20) & 0xf;

	/*
	 * Models that carry the real DFID in F5x180 (M06-08) advertise an
	 * F5 sibling device ID in the DF table.  Use that presence rather
	 * than a raw model-number range to decide the DFID source.
	 */
	if (ids->f5_id) {
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
 * SocketPresent reports which sockets are populated; it is a platform
 * property identical for every DF instance, so one read from the first
 * DF misc device is sufficient.  It is kept separate from the
 * per-node identity in hygon_read_df_id() to avoid mixing system-wide
 * topology with per-node fields.
 *
 * A successful PCI config read yields the whole register, so on the
 * supported models -- which all provide SocketPresent -- a zero mask
 * is a hard error rather than something to tolerate.
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

		/*
		 * SocketPresent is system-wide, so sample it exactly once,
		 * from the first DF misc whose identity read succeeded.
		 */
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

	if (!count) {
		ret = -ENODEV;
		goto fail;
	}

	/*
	 * Validate enumeration completeness against the hardware-declared
	 * socket set: a mismatch means an entire socket's DF misc devices
	 * were missed.  socket_present_mask is already guaranteed non-zero
	 * by hygon_read_socket_present().
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
 * The initial dense CDD model expects each socket to contribute the
 * same number of compute dies, so reject topologies that do not
 * satisfy that invariant.
 */
static int __init hygon_sort_and_classify(struct hygon_node_cache *cache)
{
	u16 cdd_per_socket;
	u16 i;
	u8 per_sock_count[HYGON_MAX_SOCKETS] = { 0 };

	hygon_dump_nodes(cache, "before-sort");

	sort(cache->nodes, cache->num_nodes, sizeof(*cache->nodes),
	     hygon_node_cmp, NULL);

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

	cache->cdd_per_socket = cdd_per_socket;

	hygon_dump_nodes(cache, "after-sort");

	return 0;
}

/*
 * Validate that the running CPU implements the Hygon Fam18h phys_node_id
 * encoding used by hygon_phys_nid_to_df_node():
 *
 *   bits [7:4] = socket_id
 *   bits [3:0] = local CDD index within socket, in ascending DFID order
 *
 * Supported on the same models the node layer enumerates (the DF
 * table decides this); other models stay unsupported until their
 * encoding is confirmed, so hygon_cpu_to_df_node() returns -ENODEV
 * there while the rest of the node stack keeps working.
 *
 * Also requires CPUID 0x8000001E ECX[10:8] + 1 (NodesPerProcessor) to
 * match cache->cdd_per_socket, detecting a node-count mismatch between
 * CPUID and DF enumeration before any caller acts on a translated index.
 */
static bool __init hygon_cpu_map_validate(const struct hygon_node_cache *cache)
{
	unsigned int nodes_per_socket;

	if (!hygon_get_df_func_ids())
		return false;

	nodes_per_socket = ((cpuid_ecx(0x8000001e) >> 8) & 0x7) + 1;
	if (nodes_per_socket != cache->cdd_per_socket) {
		pr_warn("CPU-to-DF map disabled: CPUID nodes_per_socket=%u != cdd_per_socket=%u\n",
			nodes_per_socket, cache->cdd_per_socket);
		return false;
	}

	return true;
}

/*
 * Translate a Hygon Fam18h phys_node_id (CPUID 8000001E ECX[7:0]) to a
 * dense DF CDD index.  On supported models, the node layer uses
 * this encoding:
 *
 *   phys_node_id = (socket_id << 4) | local_cdd_index_in_dfid_order
 *
 * The cache->nodes[] CDD region is sorted by (socket_id ASC, dfid ASC),
 * so walk it and return the actual cache index of the CDD whose
 * socket_id matches and whose socket-local ordinal is @local.
 *
 * A socket or local index with no matching CDD maps to -ENODEV, which
 * also covers a model not in the supported range (cpu_map_supported ==
 * false).
 */
static int hygon_phys_nid_to_df_node(unsigned int phys_nid)
{
	unsigned int socket = phys_nid >> 4;
	unsigned int local = phys_nid & 0xf;
	unsigned int ordinal = 0;
	u16 i;

	if (!hygon_cache.cpu_map_supported)
		return -ENODEV;

	for (i = 0; i < hygon_cache.num_cdd; i++) {
		if (hygon_cache.nodes[i].socket_id != socket)
			continue;

		if (ordinal++ == local)
			return i;
	}

	return -ENODEV;
}

/*
 * Build the global DF node cache.
 *
 * Called exactly once from hygon_node_init() at fs_initcall, so no
 * locking is required.  Two independent flags:
 *
 *   cache.ready              set after collect + sort succeed.  DF
 *                            identity / lookup APIs and DF function
 *                            PCI access work from here.  SMN
 *                            read/write are gated separately on
 *                            hygon_smn_exclusive.
 *   cache.cpu_map_supported  set when the running model exposes the
 *                            phys_node_id encoding consumed by
 *                            hygon_cpu_to_df_node().  Unset leaves the
 *                            rest of the node stack working and only
 *                            disables that single API.
 *
 * Runtime APIs short-circuit on their respective flag, so pre-init
 * callers see a documented failure value (0 / NULL / -ENODEV).
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
	hygon_cache.cpu_map_supported = hygon_cpu_map_validate(&hygon_cache);

	return 0;

fail:
	hygon_release_nodes(hygon_cache.nodes, hygon_cache.num_nodes);
	hygon_cache.nodes = NULL;
	hygon_cache.num_nodes = 0;
	hygon_cache.num_cdd = 0;
	hygon_cache.num_sockets = 0;
	hygon_cache.cdd_per_socket = 0;
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

int hygon_cpu_to_df_node(unsigned int cpu)
{
	if (cpu >= nr_cpu_ids)
		return -EINVAL;

	return hygon_phys_nid_to_df_node(topology_amd_node_id(cpu));
}
EXPORT_SYMBOL_GPL(hygon_cpu_to_df_node);

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
 * Single Hygon fs_initcall: build the DF node cache, then run any
 * Hygon-specific subsequent setup.
 *
 * A failed cache build short-circuits the rest of the initcall.
 * Failures in optional subsequent steps are logged but do not fail the
 * initcall, because the DF identity and DF function access APIs
 * remain useful even when later setup fails.
 */
static int __init hygon_node_init(void)
{
	int ret;

	/*
	 * Gate on node-layer model support, not just family.  Unsupported
	 * Hygon Fam18h models (and non-Hygon CPUs) return without building
	 * the cache, instead of entering the build and failing later with
	 * a misleading "build failed".  Log unsupported Hygon models so
	 * the missing node layer is visible in dmesg.
	 */
	if (!hygon_get_df_func_ids()) {
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
