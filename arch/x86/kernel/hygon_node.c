// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hygon Family 0x18 Data Fabric node enumeration and registration
 *
 * AMD systems enumerate DF nodes at fixed PCI slots 00:18..1f on bus 0.
 * Hygon Fam18h places DF instances at platform-assigned slots with no
 * fixed relationship to node identity, so the AMD enumeration path in
 * amd_nb.c / amd_node.c cannot be used.
 *
 * This module provides:
 *
 *  - DF node enumeration: walk DF misc (F3) devices by PCI ID, read
 *    hardware identity from per-instance registers (F1x200 SystemCfg,
 *    F5x180 FabricId), and build a sorted node cache that classifies
 *    each node as CDD (compute, DFID >= 4) or IOD (I/O, DFID < 4).
 *
 *  - CPU-to-node mapping: establish a hardware-anchored bijection between
 *    per-CPU phys_node_id (CPUID 8000001E[7:0]) and dense logical node
 *    IDs (0..num_cdd-1) for O(1) lookup by EDAC, MCE decode, and ATL.
 *
 *  - NB cache and SMN root registration: register the per-node PCI
 *    config-space view via amd_nb_set_cache(), and register the
 *    per-node SMN root array independently via smn_set_roots().
 */

#define pr_fmt(fmt) "hygon_node: " fmt

#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/cpu.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pci_ids.h>
#include <linux/slab.h>
#include <linux/sort.h>

#include <asm/amd/nb.h>
#include <asm/amd/node.h>
#include <asm/hygon/node.h>
#include <asm/processor.h>
#include <asm/topology.h>

/* CPUID 8000001E[7:0] is 8-bit and globally unique. */
#define HYGON_MAX_PHYS_NID	256
/* Sentinel for unmapped entries in nid_to_logical[]. */
#define HYGON_NID_INVALID	U8_MAX

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
 *   Model 10h+:     F1x200[23:20] (MyDieId == DFID, same as Model 04h/05h)
 */
#define DF_F1_SYSTEM_CFG	0x200
#define DF_F5_FABRIC_ID		0x180

/* DF function numbers for sibling device access. */
#define HYGON_DF_F1	1	/* SystemCfg: socket and die identity */
#define HYGON_DF_F3	3	/* Miscellaneous (misc) */
#define HYGON_DF_F4	4	/* Link */
#define HYGON_DF_F5	5	/* FabricId: real DFID on Model 06h-08h */

/* DF sibling device IDs used only within this file for identity reads. */
#define PCI_DEVICE_ID_HYGON_18H_M04H_DF_F1	0x1491
#define PCI_DEVICE_ID_HYGON_18H_M05H_DF_F1	0x14b1
#define PCI_DEVICE_ID_HYGON_18H_M05H_DF_F4	0x14b4
#define PCI_DEVICE_ID_HYGON_18H_M06H_DF_F5	0x14b5
#define PCI_DEVICE_ID_HYGON_18H_M10H_DF_F4	0x14d4

/*
 * Cached identity for one DF instance.  After sorting, CDDs occupy
 * nodes[0..num_cdd-1].
 *
 * socket_id + dfid together uniquely identify a DF node in hardware.
 * Unlike AMD (fixed PCI slot == node ID), Hygon DF nodes sit at
 * platform-assigned slots, so EDAC / MCE consumers that need to map an
 * error to a physical location will eventually need both fields exposed
 * (e.g. "socket=1 dfid=5" in ECC error logs).
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
	u16			num_nodes;	/* CDD + IOD = total logical nodes */
	u16			num_cdd;	/* CDD only = EDAC instance count */
	u16			num_sockets;

	/*
	 * Direct sparse-to-dense mapping. nid_to_logical[phys_nid] gives the
	 * dense logical_node_id (0..num_cdd-1), or HYGON_NID_INVALID.
	 */
	u8			nid_to_logical[HYGON_MAX_PHYS_NID];

	bool			ready;
	bool			failed;
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
	{ PCI_DEVICE(PCI_VENDOR_ID_HYGON, PCI_DEVICE_ID_HYGON_18H_M04H_DF_F3) },  /* M04h */
	{ PCI_DEVICE(PCI_VENDOR_ID_HYGON, PCI_DEVICE_ID_HYGON_18H_M04H_DF_F3B) }, /* M04h variant */
	{ PCI_DEVICE(PCI_VENDOR_ID_HYGON, PCI_DEVICE_ID_HYGON_18H_M05H_DF_F3) },  /* M05h-08h */
	{ PCI_DEVICE(PCI_VENDOR_ID_HYGON, PCI_DEVICE_ID_HYGON_18H_M10H_DF_F3) },  /* M10h+ */
	{}
};

/* DF link (F4) device IDs, parallel to hygon_nb_misc_ids[]. */
static const struct pci_device_id hygon_nb_link_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_HYGON, PCI_DEVICE_ID_HYGON_18H_M04H_DF_F4) },  /* M04h */
	{ PCI_DEVICE(PCI_VENDOR_ID_HYGON, PCI_DEVICE_ID_HYGON_18H_M04H_DF_F4B) }, /* M04h variant */
	{ PCI_DEVICE(PCI_VENDOR_ID_HYGON, PCI_DEVICE_ID_HYGON_18H_M05H_DF_F4) }, /* M05h-08h */
	{ PCI_DEVICE(PCI_VENDOR_ID_HYGON, PCI_DEVICE_ID_HYGON_18H_M10H_DF_F4) }, /* M10h+ */
	{}
};

/*
 * Map Hygon Fam18h model ranges onto the DF sibling functions used to read
 * node identity. Model 5 may still expose the Model 4 F1 device id on mixed
 * silicon, which is handled separately in hygon_read_df_reg().
 */
static const struct hygon_df_func_ids hygon_df_table[] = {
	{ 0x04, 0x04, PCI_DEVICE_ID_HYGON_18H_M04H_DF_F1, 0 },
	{ 0x05, 0x05, PCI_DEVICE_ID_HYGON_18H_M05H_DF_F1, 0 },
	{ 0x06, 0x08, PCI_DEVICE_ID_HYGON_18H_M05H_DF_F1,
		      PCI_DEVICE_ID_HYGON_18H_M06H_DF_F5 },
	{ 0x10, 0x1f, PCI_DEVICE_ID_HYGON_18H_M05H_DF_F1, 0 },
	{}
};

static struct hygon_node_cache hygon_cache;
static DEFINE_MUTEX(hygon_mutex);

static void hygon_dump_nodes(const struct hygon_node_cache *cache, const char *phase)
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
 * Follows the pci_get_device() convention: @from is consumed (its reference
 * is dropped) and the returned device has an elevated reference count.
 */
static struct pci_dev *next_hygon_dev(struct pci_dev *from,
				      const struct pci_device_id *ids)
{
	while ((from = pci_get_device(PCI_VENDOR_ID_HYGON, PCI_ANY_ID, from))) {
		if (pci_match_id(ids, from))
			return from;
	}

	return NULL;
}

/*
 * Find the DF link (function 4) sibling of a DF misc (function 3) device.
 * Both functions share the same PCI bus and slot.
 */
static struct pci_dev *hygon_get_link(struct pci_dev *misc)
{
	struct pci_dev *link;

	link = pci_get_domain_bus_and_slot(pci_domain_nr(misc->bus),
					   misc->bus->number,
					   PCI_DEVFN(PCI_SLOT(misc->devfn), HYGON_DF_F4));
	if (!link)
		return NULL;

	if (!pci_match_id(hygon_nb_link_ids, link)) {
		pci_dev_put(link);
		return NULL;
	}

	return link;
}

/* Look up the DF sibling device IDs for the current boot CPU model. */
static const struct hygon_df_func_ids *hygon_get_df_func_ids(void)
{
	const struct hygon_df_func_ids *entry;
	u8 model = boot_cpu_data.x86_model;

	for (entry = hygon_df_table; entry->f1_id; entry++) {
		if (model >= entry->model_start && model <= entry->model_end)
			return entry;
	}

	pr_warn_once("unsupported Hygon Fam18h model 0x%x, Hygon node support disabled\n",
		     model);
	return NULL;
}

/*
 * Read a config register from a DF sibling function on the same PCI slot as
 * @misc.  Only functions 1 (F1, SystemCfg) and 5 (F5, FabricId) are
 * supported.
 */
static int hygon_read_df_reg(struct pci_dev *misc, u8 func, int offset,
			     u32 *value)
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
		 * Model 5 can expose an older mixed-silicon variant where the
		 * F1 sibling still uses the M04H device ID.
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

	if (err)
		pr_warn("error reading %04x:%02x:%02x.%u offset 0x%x\n",
			pci_domain_nr(misc->bus), misc->bus->number,
			PCI_SLOT(misc->devfn), func, offset);

	return pcibios_err_to_errno(err);
}

/*
 * Read the hardware identity for one DF misc device from its sibling
 * functions.
 *
 * All models expose F1x200 (SystemCfg): socket_id from [30:28] and a die
 * identifier (MyDieId) from [23:20].
 * On Model 06h-08h MyDieId differs from the DFID used by UMC and SMN
 * addressing, so an additional F5x180 (FabricId) read is required to
 * obtain the real DFID from [19:16].
 */
static int hygon_read_df_id(struct pci_dev *misc, struct hygon_df_id *id)
{
	u32 reg;
	int ret;

	ret = hygon_read_df_reg(misc, HYGON_DF_F1, DF_F1_SYSTEM_CFG, &reg);
	if (ret)
		return ret;

	id->socket_id = (reg >> 28) & 0x7;
	id->dfid      = (reg >> 20) & 0xf;

	/*
	 * All DF instances on a Hygon system are the same model, so
	 * boot_cpu_data.x86_model is representative for all devices.
	 */
	if (boot_cpu_data.x86_model >= 0x6 &&
	    boot_cpu_data.x86_model <= 0x8) {
		ret = hygon_read_df_reg(misc, HYGON_DF_F5, DF_F5_FABRIC_ID, &reg);
		if (ret)
			return ret;
		id->dfid = (reg >> 16) & 0xf;
	}

	return 0;
}

static void hygon_release_nodes(struct hygon_node *nodes, u16 count)
{
	u16 i;

	for (i = 0; i < count; i++) {
		pci_dev_put(nodes[i].misc);
		pci_dev_put(nodes[i].link);
	}

	kfree(nodes);
}

/*
 * Walk all DF misc (F3) devices, read hardware identity (socket_id, dfid)
 * from each instance's F1/F5 siblings, and collect them into a flat array.
 * Validate that socket IDs are dense (0..N-1).
 */
static int hygon_collect_nodes(struct hygon_node_cache *cache)
{
	struct hygon_node *nodes;
	struct pci_dev *misc;
	u16 capacity = 0, count = 0;
	u8 socket_mask = 0;
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
		nodes[count].is_cdd    = (id.dfid >= 4);
		socket_mask |= BIT(id.socket_id);
		count++;
	}

	if (!count) {
		ret = -ENODEV;
		goto fail;
	}

	cache->nodes       = nodes;
	cache->num_nodes   = count;
	cache->num_sockets = hweight8(socket_mask);

	if (!cache->num_sockets ||
	    socket_mask != GENMASK(cache->num_sockets - 1, 0)) {
		pr_warn("sparse socket IDs not supported (mask=0x%x)\n",
			socket_mask);
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

/* Sort CDD nodes before IOD nodes, then order by hardware (socket_id, dfid). */
static int hygon_node_cmp(const void *a, const void *b)
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
 * Hygon EDAC sizing assumes each socket contributes the same number of compute
 * dies, so reject topologies that do not satisfy that invariant.
 */
static int hygon_sort_and_classify(struct hygon_node_cache *cache)
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

	if (cache->num_cdd > HYGON_NID_INVALID) {
		pr_warn("CDD count %u exceeds u8 logical ID range\n",
			cache->num_cdd);
		return -EOVERFLOW;
	}

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

static int hygon_u8_cmp(const void *a, const void *b)
{
	return (int)*(const u8 *)a - (int)*(const u8 *)b;
}

/*
 * Build nid_to_logical[]: collect unique phys_node_id values from online CPUs,
 * sort them globally ascending, then write nid_to_logical[phys_nids[i]] = i.
 *
 * This works because both orderings follow the same physical progression:
 *   nodes[] CDD region: (socket_id ASC, dfid ASC) from DF registers
 *   phys_nids[]:        globally ascending; lower socket always occupies a
 *                       lower phys_nid range, and within a socket ascending
 *                       DFID order corresponds to ascending phys_nid order.
 *
 * A local bitmap (nid_seen) de-duplicates CPUs that share a node; each
 * unique phys_nid is collected exactly once into phys_nids[]. The resulting
 * runtime lookup is O(1): a direct array access by phys_nid.
 */
static int hygon_build_nid_map(struct hygon_node_cache *cache)
{
	DECLARE_BITMAP(nid_seen, HYGON_MAX_PHYS_NID) = { };
	u8 *phys_nids;
	u16 count = 0;
	u16 i;
	int cpu;

	phys_nids = kcalloc(cache->num_cdd, sizeof(*phys_nids), GFP_KERNEL);
	if (!phys_nids)
		return -ENOMEM;

	for_each_online_cpu(cpu) {
		unsigned int phys_nid = topology_amd_node_id(cpu);

		if (phys_nid >= HYGON_MAX_PHYS_NID) {
			pr_warn("cpu %u: phys_node_id %u out of range\n",
				cpu, phys_nid);
			kfree(phys_nids);
			return -ERANGE;
		}

		if (__test_and_set_bit(phys_nid, nid_seen))
			continue;

		if (count >= cache->num_cdd) {
			pr_warn("more unique phys_node_ids than CDD nodes\n");
			kfree(phys_nids);
			return -EINVAL;
		}

		phys_nids[count++] = (u8)phys_nid;
	}

	if (count != cache->num_cdd) {
		pr_warn("collected %u phys_node_ids, expected %u CDDs\n",
			count, cache->num_cdd);
		kfree(phys_nids);
		return -EINVAL;
	}

	sort(phys_nids, count, sizeof(*phys_nids), hygon_u8_cmp, NULL);

	memset(cache->nid_to_logical, HYGON_NID_INVALID,
	       sizeof(cache->nid_to_logical));

	for (i = 0; i < count; i++)
		cache->nid_to_logical[phys_nids[i]] = i;

	kfree(phys_nids);
	return 0;
}

static void hygon_destroy_cache(struct hygon_node_cache *cache)
{
	if (cache->nodes)
		hygon_release_nodes(cache->nodes, cache->num_nodes);

	cache->nodes = NULL;
	cache->num_nodes = 0;
	cache->num_cdd = 0;
	cache->num_sockets = 0;
	memset(cache->nid_to_logical, HYGON_NID_INVALID,
	       sizeof(cache->nid_to_logical));
	cache->ready = false;
}

/*
 * Build and publish the global Hygon node cache.
 *
 * Uses a double-checked locking pattern: the first smp_load_acquire() on
 * @ready provides a fast lockless path for all calls after the initial build.
 * The second check inside the mutex prevents duplicate construction if two
 * callers race through the first check simultaneously.
 *
 * On success, smp_store_release() on @ready ensures all cache writes are
 * visible to subsequent smp_load_acquire() readers without a lock.
 *
 * On failure, @failed is set under the mutex so that subsequent callers
 * return immediately without repeating the expensive PCI walk.
 */
static int hygon_build_cache(void)
{
	struct hygon_node_cache tmp;
	int err;

	/* Pairs with smp_store_release() below; fast path once cache is built. */
	if (smp_load_acquire(&hygon_cache.ready))
		return 0;

	if (boot_cpu_data.x86_vendor != X86_VENDOR_HYGON ||
	    boot_cpu_data.x86 != 0x18)
		return -ENODEV;

	guard(mutex)(&hygon_mutex);

	/* Re-check under mutex to handle concurrent builders. */
	if (smp_load_acquire(&hygon_cache.ready))
		return 0;

	if (hygon_cache.failed)
		return -ENODEV;

	memset(&tmp, 0, sizeof(tmp));
	memset(tmp.nid_to_logical, HYGON_NID_INVALID,
	       sizeof(tmp.nid_to_logical));

	err = hygon_collect_nodes(&tmp);
	if (err)
		goto fail;

	err = hygon_sort_and_classify(&tmp);
	if (err)
		goto fail;

	err = hygon_build_nid_map(&tmp);
	if (err)
		goto fail;

	hygon_cache = tmp;
	/* Pairs with smp_load_acquire() above; ensures cache is visible to all CPUs. */
	smp_store_release(&hygon_cache.ready, true);
	return 0;

fail:
	hygon_destroy_cache(&tmp);
	hygon_cache.failed = true;
	return err;
}

u8 hygon_f18h_model(void)
{
	if (boot_cpu_data.x86_vendor == X86_VENDOR_HYGON &&
	    boot_cpu_data.x86 == 0x18)
		return boot_cpu_data.x86_model;

	return 0;
}
EXPORT_SYMBOL_GPL(hygon_f18h_model);

int hygon_get_dfid(struct pci_dev *misc, u8 *dfid)
{
	struct hygon_df_id id;
	int ret;

	ret = hygon_read_df_id(misc, &id);
	if (ret)
		return ret;

	*dfid = id.dfid;
	return 0;
}
EXPORT_SYMBOL_GPL(hygon_get_dfid);

static u16 hygon_node_num(void)
{
	return hygon_build_cache() ? 0 : hygon_cache.num_nodes;
}

u16 hygon_cdd_num(void)
{
	return hygon_build_cache() ? 0 : hygon_cache.num_cdd;
}
EXPORT_SYMBOL_GPL(hygon_cdd_num);

static u16 hygon_socket_num(void)
{
	return hygon_build_cache() ? 0 : hygon_cache.num_sockets;
}

/*
 * Return the DF function device for a logical Hygon node.
 * The caller must call pci_dev_put() on the returned pointer when done.
 * Only functions 3 (misc) and 4 (link) are supported.
 */
static struct pci_dev *hygon_node_get_func(u16 node, u8 func)
{
	if (hygon_build_cache())
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

static u8 hygon_node_socket(u16 node)
{
	if (hygon_build_cache())
		return U8_MAX;

	if (node >= hygon_cache.num_nodes)
		return U8_MAX;

	return hygon_cache.nodes[node].socket_id;
}

/*
 * Translate a CPU's sparse physical node ID (CPUID 8000001E[7:0]) into a
 * dense DF node index (0..hygon_cdd_num()-1) used by NB, EDAC, MCE, and
 * ATL.  The lookup is O(1) via direct array access on nid_to_logical[].
 *
 * Returns the DF node index on success, or a negative errno on failure.
 */
int hygon_cpu_to_df_node(unsigned int cpu)
{
	unsigned int phys_nid;
	u8 logical_id;

	if (hygon_build_cache())
		return -ENODEV;

	phys_nid = topology_amd_node_id(cpu);
	if (phys_nid >= HYGON_MAX_PHYS_NID)
		return -ENODEV;

	logical_id = hygon_cache.nid_to_logical[phys_nid];
	return logical_id == HYGON_NID_INVALID ? -ENODEV : logical_id;
}
EXPORT_SYMBOL_GPL(hygon_cpu_to_df_node);

/*
 * Walk PCI host-bridge devices matching the Hygon vendor.  The SMN
 * index/data registers live in function 0 of each root complex (class
 * PCI_CLASS_BRIDGE_HOST, devfn 0).
 */
static struct pci_dev *hygon_get_next_root(struct pci_dev *root)
{
	while ((root = pci_get_class(PCI_CLASS_BRIDGE_HOST << 8, root))) {
		if (root->devfn)
			continue;
		if (root->vendor != PCI_VENDOR_ID_HYGON)
			continue;
		break;
	}
	return root;
}

/*
 * Build and register the NB cache for amd_nb consumers (EDAC, MCE, etc.).
 *
 * Allocates a struct amd_northbridge[] array, fills in the misc (F3) and
 * link (F4) PCI devices for each logical node, and hands it to
 * amd_nb_set_cache(). On success the array is owned by amd_nb; on failure
 * all PCI references are released.
 */
static int __init hygon_nb_cache_init(u16 num_nodes)
{
	struct amd_northbridge *nb;
	u16 node;
	int ret;

	nb = kzalloc_objs(struct amd_northbridge, num_nodes);
	if (!nb)
		return -ENOMEM;

	for (node = 0; node < num_nodes; node++) {
		nb[node].misc = hygon_node_get_func(node, 3);
		if (!nb[node].misc) {
			ret = -ENODEV;
			goto err_put;
		}

		nb[node].link = hygon_node_get_func(node, 4);
		if (!nb[node].link) {
			ret = -ENODEV;
			pci_dev_put(nb[node].misc);
			nb[node].misc = NULL;
			goto err_put;
		}
	}

	ret = amd_nb_set_cache(nb, num_nodes);
	if (ret)
		goto err_put;

	return 0;

err_put:
	while (node--) {
		pci_dev_put(nb[node].link);
		pci_dev_put(nb[node].misc);
	}
	kfree(nb);
	return ret;
}

/*
 * Register the Hygon NB cache independently of SMN root setup.
 *
 * The NB cache exposes the per-node misc/link PCI devices and does not
 * rely on SMN.  A successful NB registration therefore remains useful
 * even if SMN root setup later fails.
 *
 * Both hygon_nb_init() and hygon_smn_init() depend only on
 * hygon_build_cache(), so their relative fs_initcall order does not
 * matter.
 */
static int __init hygon_nb_init(void)
{
	u16 num_nodes;

	if (boot_cpu_data.x86_vendor != X86_VENDOR_HYGON ||
	    boot_cpu_data.x86 != 0x18)
		return 0;

	num_nodes = hygon_node_num();
	if (!num_nodes)
		return -ENODEV;

	return hygon_nb_cache_init(num_nodes);
}
fs_initcall(hygon_nb_init);

static void hygon_release_root_regions(struct pci_dev **roots, u16 count)
{
	u16 i;

	for (i = 0; i < count; i++)
		pci_release_config_region(roots[i], 0, PCI_CFG_SPACE_SIZE);
}

/*
 * Hygon SMN initialisation.
 *
 * On Hygon Fam18h, SMN root devices are shared per-socket: all nodes (CDD
 * and IOD) on the same socket use the same host-bridge root for SMN access.
 * These root devices are transport endpoints, not topology identifiers:
 * socket_id / DFID come from DF registers, while the selected root only
 * determines which socket-local SMN index/data pair is used by
 * amd_smn_read() / amd_smn_write(). Any root within a socket-equivalent
 * group is sufficient, but binding a node to a root from another socket
 * would route the transaction through the wrong SMN ingress.
 *
 * We therefore collect one representative root per socket and expand that
 * per-socket selection into a per-node array for smn_set_roots().
 */
static int __init hygon_smn_init(void)
{
	struct pci_dev *socket_roots[HYGON_MAX_SOCKETS] = { };
	struct pci_dev **reserved_roots, **roots, *root;
	u16 count, num_roots, roots_per_socket, node, num_nodes;
	u16 num_sockets, reserved, socket, socket_id;
	int ret;

	if (boot_cpu_data.x86_vendor != X86_VENDOR_HYGON ||
	    boot_cpu_data.x86 != 0x18)
		return 0;

	num_roots = 0;
	root = NULL;
	while ((root = hygon_get_next_root(root)))
		num_roots++;

	pr_debug("Found %u Hygon SMN root devices\n", num_roots);

	if (!num_roots)
		return -ENODEV;

	/*
	 * hygon_node_num() triggers hygon_build_cache(), which reads
	 * F1x200 / F5x180 identity registers from every DF misc device
	 * and validates the socket topology.
	 */
	num_nodes = hygon_node_num();
	if (!num_nodes)
		return -ENODEV;

	num_sockets = hygon_socket_num();
	if (!num_sockets)
		return -ENODEV;

	if (num_sockets > ARRAY_SIZE(socket_roots)) {
		pr_err("Socket count %u exceeds maximum %zu\n",
		       num_sockets, ARRAY_SIZE(socket_roots));
		return -EINVAL;
	}

	if (num_roots % num_sockets) {
		pr_err("Root count %u not divisible by socket count %u\n",
		       num_roots, num_sockets);
		return -ENODEV;
	}

	roots = kzalloc_objs(*roots, num_nodes);
	if (!roots)
		return -ENOMEM;

	reserved_roots = kcalloc(num_roots, sizeof(*reserved_roots), GFP_KERNEL);
	if (!reserved_roots) {
		kfree(roots);
		return -ENOMEM;
	}

	/*
	 * Collect one representative root per socket and skip the rest.
	 * Root devices within the same socket are redundant SMN ingress points.
	 */
	roots_per_socket = num_roots / num_sockets;
	socket = 0;
	reserved = 0;
	count = 0;
	root = NULL;
	while ((root = hygon_get_next_root(root))) {
		pci_dbg(root, "Reserving PCI config space\n");

		/*
		 * Reserve the entire PCI config space so user space cannot
		 * race with SMN index/data register access.
		 */
		if (!pci_request_config_region_exclusive(root, 0,
							 PCI_CFG_SPACE_SIZE,
							 NULL)) {
			pci_err(root, "Failed to reserve config space\n");
			ret = -EEXIST;
			goto err_release;
		}

		reserved_roots[reserved++] = root;

		if (count++ % roots_per_socket)
			continue;

		if (socket >= num_sockets) {
			ret = -ENODEV;
			goto err_release;
		}

		pci_dbg(root, "is root for Hygon socket %u\n", socket);
		socket_roots[socket++] = root;
	}

	if (socket != num_sockets) {
		ret = -ENODEV;
		goto err_release;
	}

	/* Expand socket roots to a per-node array for the SMN layer. */
	for (node = 0; node < num_nodes; node++) {
		socket_id = hygon_node_socket(node);

		if (socket_id >= num_sockets) {
			ret = -ENODEV;
			goto err_release;
		}

		pci_dbg(socket_roots[socket_id],
			"is root for Hygon node %u (socket %u)\n",
			node, socket_id);
		roots[node] = socket_roots[socket_id];
	}

	ret = smn_set_roots(roots, num_nodes);
	if (ret)
		goto err_release;

	/* roots[] is now owned by the SMN layer; do not free on error below. */
	ret = smn_activate("hygon_smn");
	kfree(reserved_roots);
	return ret;

err_release:
	hygon_release_root_regions(reserved_roots, reserved);
	kfree(reserved_roots);
	kfree(roots);
	return ret;
}
fs_initcall(hygon_smn_init);
