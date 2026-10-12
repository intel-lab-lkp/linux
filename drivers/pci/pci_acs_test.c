// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for PCI ACS peer-to-peer routing decisions.
 *
 * These exercise Request and Completion routing independently of the ACS
 * settings exposed by available PCIe hardware.
 */
#include <kunit/test.h>

#include <linux/pci.h>
#include <linux/pci-p2pdma.h>
#include <linux/pci_regs.h>

#include "pci.h"

struct acs_decision_case {
	const char *desc;
	u16 ctrl;
	unsigned int tlp_flags;
	enum pci_acs_p2pdma_state expect;
};

/* Shorthands to keep the tables below readable. */
#define ACS_DIRECT	PCI_ACS_P2PDMA_DIRECT
#define ACS_REDIR	PCI_ACS_P2PDMA_REDIRECT
#define ACS_RO		PCI_P2PDMA_TLP_RELAXED_CPL
#define ACS_AT		PCI_P2PDMA_TLP_TRANSLATED
#define ACS_BLOCK	PCI_ACS_P2PDMA_BLOCKED

/* Request routing ignores Completion Redirect. */
static const struct acs_decision_case acs_request_cases[] = {
	{ "req/none", 0, 0, ACS_DIRECT },
	{ "req/rr", PCI_ACS_RR, 0, ACS_REDIR },
	{ "req/cr", PCI_ACS_CR, 0, ACS_DIRECT },
	{ "req/rr_cr", PCI_ACS_RR | PCI_ACS_CR, 0, ACS_REDIR },
	{ "req/ec", PCI_ACS_EC, 0, ACS_REDIR },
	{ "req/ec_cr", PCI_ACS_EC | PCI_ACS_CR, 0, ACS_REDIR },

	/*
	 * Direct Translated P2P overrides the redirect controls, but only for
	 * a Request that actually carries a Translated address.
	 */
	{ "req/dt", PCI_ACS_DT, 0, ACS_DIRECT },
	{ "req/dt_rr", PCI_ACS_DT | PCI_ACS_RR, 0, ACS_REDIR },
	{ "req/at", 0, ACS_AT, ACS_DIRECT },
	{ "req/at_rr", PCI_ACS_RR, ACS_AT, ACS_REDIR },
	{ "req/at_dt_rr", PCI_ACS_DT | PCI_ACS_RR, ACS_AT, ACS_DIRECT },
	{ "req/at_dt_ec", PCI_ACS_DT | PCI_ACS_EC, ACS_AT, ACS_DIRECT },

	/*
	 * Translation Blocking rejects a Translated address outright, and
	 * makes the port ignore Direct Translated P2P.
	 */
	{ "req/tb", PCI_ACS_TB, 0, ACS_DIRECT },
	{ "req/tb_rr", PCI_ACS_TB | PCI_ACS_RR, 0, ACS_REDIR },
	{ "req/at_tb", PCI_ACS_TB, ACS_AT, ACS_BLOCK },
	{ "req/at_tb_dt", PCI_ACS_TB | PCI_ACS_DT, ACS_AT, ACS_BLOCK },
};

/* Completion routing depends only on Completion Redirect. */
static const struct acs_decision_case acs_completion_cases[] = {
	{ "cpl/none", 0, 0, ACS_DIRECT },
	{ "cpl/rr", PCI_ACS_RR, 0, ACS_DIRECT },
	{ "cpl/cr", PCI_ACS_CR, 0, ACS_REDIR },
	{ "cpl/rr_cr", PCI_ACS_RR | PCI_ACS_CR, 0, ACS_REDIR },
	{ "cpl/ec", PCI_ACS_EC, 0, ACS_DIRECT },
	{ "cpl/ec_cr", PCI_ACS_EC | PCI_ACS_CR, 0, ACS_REDIR },

	/* Relaxed Ordering Completions are never redirected. */
	{ "cpl/ro", 0, ACS_RO, ACS_DIRECT },
	{ "cpl/ro_cr", PCI_ACS_CR, ACS_RO, ACS_DIRECT },
	{ "cpl/ro_rr_cr", PCI_ACS_RR | PCI_ACS_CR, ACS_RO, ACS_DIRECT },
};

#undef ACS_DIRECT
#undef ACS_REDIR
#undef ACS_RO
#undef ACS_AT
#undef ACS_BLOCK

static void acs_decision_desc(const struct acs_decision_case *c, char *desc)
{
	strscpy(desc, c->desc, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(acs_request, acs_request_cases, acs_decision_desc);
KUNIT_ARRAY_PARAM(acs_completion, acs_completion_cases, acs_decision_desc);

static void pci_acs_p2pdma_request_test(struct kunit *test)
{
	const struct acs_decision_case *c = test->param_value;

	KUNIT_EXPECT_EQ(test, pci_acs_p2pdma_request(c->ctrl, c->tlp_flags),
			c->expect);
}

static void pci_acs_p2pdma_completion_test(struct kunit *test)
{
	const struct acs_decision_case *c = test->param_value;

	KUNIT_EXPECT_EQ(test, pci_acs_p2pdma_completion(c->ctrl, c->tlp_flags),
			c->expect);
}

/* Flags an IOMMU asks for; see REQ_ACS_FLAGS in drivers/iommu/iommu.c. */
#define ACS_REQ_FLAGS	(PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF)
#define ACS_ALL_CAPS	(PCI_ACS_SV | PCI_ACS_TB | PCI_ACS_RR | PCI_ACS_CR | \
			 PCI_ACS_UF | PCI_ACS_DT)
#define ACS_TEST_CAP	0x100

struct acs_ctrl_cfg {
	unsigned int devfn;
	u16 cap;	/* Offset where the ACS capability responds */
	u16 ctrl;
	bool fail_read;
};

static int acs_ctrl_read(struct pci_bus *bus, unsigned int devfn,
			 int where, int size, u32 *val)
{
	struct acs_ctrl_cfg *cfg = bus->sysdata;

	*val = 0;
	if (cfg->fail_read)
		return PCIBIOS_DEVICE_NOT_FOUND;

	if (devfn == cfg->devfn && size == 2 &&
	    where == cfg->cap + PCI_ACS_CTRL)
		*val = cfg->ctrl;
	return PCIBIOS_SUCCESSFUL;
}

static int acs_ctrl_write(struct pci_bus *bus, unsigned int devfn,
			  int where, int size, u32 val)
{
	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops acs_ctrl_ops = {
	.read	= acs_ctrl_read,
	.write	= acs_ctrl_write,
};

struct acs_isolation_case {
	const char *desc;
	u16 ctrl;
	u16 req;
	bool expect;
};

static const struct acs_isolation_case acs_isolation_cases[] = {
	{ "all_enabled", ACS_REQ_FLAGS, ACS_REQ_FLAGS, true },
	/* Translated Requests remain isolated by their IOMMU translation. */
	{ "dt", ACS_REQ_FLAGS | PCI_ACS_DT, ACS_REQ_FLAGS, true },
	{ "rr_not_enabled", PCI_ACS_SV | PCI_ACS_CR | PCI_ACS_UF,
	  ACS_REQ_FLAGS, false },
	{ "rr_not_required", PCI_ACS_SV | PCI_ACS_CR | PCI_ACS_UF,
	  PCI_ACS_SV | PCI_ACS_CR | PCI_ACS_UF, true },
};

static void acs_isolation_desc(const struct acs_isolation_case *c, char *desc)
{
	strscpy(desc, c->desc, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(acs_isolation, acs_isolation_cases, acs_isolation_desc);

static void pci_acs_flags_enabled_test(struct kunit *test)
{
	const struct acs_isolation_case *c = test->param_value;
	struct acs_ctrl_cfg cfg = {
		.devfn = PCI_DEVFN(0, 0),
		.cap = ACS_TEST_CAP,
		.ctrl = c->ctrl,
	};
	struct pci_bus *bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	struct pci_dev *pdev = kunit_kzalloc(test, sizeof(*pdev), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, bus);
	KUNIT_ASSERT_NOT_NULL(test, pdev);

	bus->ops = &acs_ctrl_ops;
	bus->sysdata = &cfg;

	pdev->bus = bus;
	pdev->devfn = cfg.devfn;
	pdev->acs_cap = ACS_TEST_CAP;
	pdev->acs_capabilities = ACS_ALL_CAPS;

	KUNIT_EXPECT_EQ(test, pci_acs_flags_enabled(pdev, c->req), c->expect);
}

static bool acs_isolated(struct kunit *test, struct acs_ctrl_cfg *cfg,
			 u16 acs_cap, u16 acs_flags)
{
	struct pci_bus *bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	struct pci_dev *pdev = kunit_kzalloc(test, sizeof(*pdev), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, bus);
	KUNIT_ASSERT_NOT_NULL(test, pdev);

	bus->ops = &acs_ctrl_ops;
	bus->sysdata = cfg;

	pdev->bus = bus;
	pdev->devfn = cfg->devfn;
	pdev->acs_cap = acs_cap;
	pdev->acs_capabilities = ACS_ALL_CAPS;

	return pci_acs_flags_enabled(pdev, acs_flags);
}

static void pci_acs_flags_no_cap_test(struct kunit *test)
{
	struct acs_ctrl_cfg cfg = {
		.devfn = PCI_DEVFN(0, 0),
		.cap = 0,
		.ctrl = ACS_REQ_FLAGS,
	};

	KUNIT_EXPECT_FALSE(test, acs_isolated(test, &cfg, 0, ACS_REQ_FLAGS));
}

static void pci_acs_flags_read_fails_test(struct kunit *test)
{
	u16 no_rr = ACS_REQ_FLAGS & ~PCI_ACS_RR;
	struct acs_ctrl_cfg cfg = {
		.devfn = PCI_DEVFN(0, 0),
		.cap = ACS_TEST_CAP,
		.ctrl = ACS_REQ_FLAGS,
	};

	KUNIT_EXPECT_TRUE(test, acs_isolated(test, &cfg, ACS_TEST_CAP, no_rr));

	cfg.fail_read = true;
	KUNIT_EXPECT_FALSE(test, acs_isolated(test, &cfg, ACS_TEST_CAP, no_rr));
}

/*
 * Drive calc_map_type_and_dist() over a fabricated PCIe fabric matching the
 * canonical topology of two devices below one switch:
 *
 *   host bridge / root bus
 *     Root Port
 *       Switch Upstream Port
 *         Switch Downstream Port 0
 *           Nested Switch -- provider
 *         Switch Downstream Port 1
 *           Nested Switch -- client
 *
 * Fake config-space operations supply the ACS Control registers. This lets
 * the cases vary both divergence ports and controls below the divergence
 * without depending on real hardware.
 */
struct acs_port_cfg {
	u16 ctrl;
	bool fail_read;
};

struct acs_fabric {
	struct pci_dev *rootport;
	struct pci_dev *provider;
	struct pci_dev *client;
	struct pci_dev *dn0;	/* Downstream Port 0 (provider side) */
	struct pci_dev *dn1;	/* Downstream Port 1 (client side) */
	struct pci_dev *provider_leaf;
	struct pci_dev *client_leaf;
	struct acs_port_cfg dn0_cfg;
	struct acs_port_cfg dn1_cfg;
	struct acs_port_cfg provider_leaf_cfg;
	struct acs_port_cfg client_leaf_cfg;
	struct acs_port_cfg rootport_cfg;
};

static int acs_port_read(struct pci_dev *port, struct acs_port_cfg *cfg,
			 int where, int size, u32 *val)
{
	if (port->acs_cap && size == 2 &&
	    where == port->acs_cap + PCI_ACS_CTRL) {
		if (cfg->fail_read)
			return PCIBIOS_DEVICE_NOT_FOUND;
		*val = cfg->ctrl;
	}

	return PCIBIOS_SUCCESSFUL;
}

static int acs_fabric_read(struct pci_bus *bus, unsigned int devfn,
			   int where, int size, u32 *val)
{
	struct acs_fabric *f = bus->sysdata;

	*val = 0;
	if (bus == f->rootport->bus && devfn == f->rootport->devfn)
		return acs_port_read(f->rootport, &f->rootport_cfg,
				     where, size, val);
	if (bus == f->dn0->bus && devfn == f->dn0->devfn)
		return acs_port_read(f->dn0, &f->dn0_cfg, where, size, val);
	if (bus == f->dn1->bus && devfn == f->dn1->devfn)
		return acs_port_read(f->dn1, &f->dn1_cfg, where, size, val);
	if (bus == f->provider_leaf->bus &&
	    devfn == f->provider_leaf->devfn)
		return acs_port_read(f->provider_leaf, &f->provider_leaf_cfg,
				     where, size, val);
	if (bus == f->client_leaf->bus && devfn == f->client_leaf->devfn)
		return acs_port_read(f->client_leaf, &f->client_leaf_cfg,
				     where, size, val);

	return PCIBIOS_SUCCESSFUL;
}

static int acs_fabric_write(struct pci_bus *bus, unsigned int devfn,
			    int where, int size, u32 val)
{
	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops acs_fabric_ops = {
	.read	= acs_fabric_read,
	.write	= acs_fabric_write,
};

static struct pci_bus *acs_add_bus(struct kunit *test, struct pci_bus *parent,
				   struct pci_dev *self, u8 nr, void *sysdata)
{
	struct pci_bus *bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, bus);
	bus->parent = parent;
	bus->self = self;
	bus->number = nr;
	bus->ops = &acs_fabric_ops;
	bus->sysdata = sysdata;
	INIT_LIST_HEAD(&bus->devices);
	return bus;
}

static struct pci_dev *acs_add_dev(struct kunit *test, struct pci_bus *bus,
				   unsigned int devfn, int pcie_type)
{
	struct pci_dev *dev = kunit_kzalloc(test, sizeof(*dev), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, dev);
	dev->bus = bus;
	dev->devfn = devfn;
	dev->pcie_cap = 0x40;
	dev->pcie_flags_reg = (pcie_type << 4) | 0x2;
	list_add_tail(&dev->bus_list, &bus->devices);
	return dev;
}

static void acs_build_fabric(struct kunit *test, struct acs_fabric *f)
{
	struct pci_bus *bus0, *bus1, *bus2, *bus3, *bus4, *bus5, *bus6;
	struct pci_bus *bus7, *bus8;
	struct pci_dev *swup, *provider_swup, *client_swup;
	struct pci_dev *rootport;
	struct pci_host_bridge *host;

	host = kunit_kzalloc(test, sizeof(*host), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, host);

	bus0 = acs_add_bus(test, NULL, NULL, 0, f);
	/* The Root Port doubles as the whitelisted host-bridge device. */
	rootport = acs_add_dev(test, bus0, PCI_DEVFN(0, 0),
			       PCI_EXP_TYPE_ROOT_PORT);
	f->rootport = rootport;
	rootport->vendor = PCI_VENDOR_ID_GOOGLE;
	rootport->device = 0x1234;
	host->bus = bus0;
	bus0->bridge = &host->dev;

	bus1 = acs_add_bus(test, bus0, rootport, 1, f);
	swup = acs_add_dev(test, bus1, PCI_DEVFN(0, 0),
			   PCI_EXP_TYPE_UPSTREAM);

	bus2 = acs_add_bus(test, bus1, swup, 2, f);
	f->dn0 = acs_add_dev(test, bus2, PCI_DEVFN(0, 0),
			     PCI_EXP_TYPE_DOWNSTREAM);
	f->dn1 = acs_add_dev(test, bus2, PCI_DEVFN(1, 0),
			     PCI_EXP_TYPE_DOWNSTREAM);

	bus3 = acs_add_bus(test, bus2, f->dn0, 3, f);
	provider_swup = acs_add_dev(test, bus3, PCI_DEVFN(0, 0),
				    PCI_EXP_TYPE_UPSTREAM);
	bus5 = acs_add_bus(test, bus3, provider_swup, 5, f);
	f->provider_leaf = acs_add_dev(test, bus5, PCI_DEVFN(0, 0),
				       PCI_EXP_TYPE_DOWNSTREAM);
	bus7 = acs_add_bus(test, bus5, f->provider_leaf, 7, f);
	f->provider = acs_add_dev(test, bus7, PCI_DEVFN(0, 0),
				  PCI_EXP_TYPE_ENDPOINT);

	bus4 = acs_add_bus(test, bus2, f->dn1, 4, f);
	client_swup = acs_add_dev(test, bus4, PCI_DEVFN(0, 0),
				  PCI_EXP_TYPE_UPSTREAM);
	bus6 = acs_add_bus(test, bus4, client_swup, 6, f);
	f->client_leaf = acs_add_dev(test, bus6, PCI_DEVFN(0, 0),
				     PCI_EXP_TYPE_DOWNSTREAM);
	bus8 = acs_add_bus(test, bus6, f->client_leaf, 8, f);
	f->client = acs_add_dev(test, bus8, PCI_DEVFN(0, 0),
				PCI_EXP_TYPE_ENDPOINT);
}

static enum pci_p2pdma_map_type acs_walk_map(struct acs_fabric *f,
					     unsigned int tlp_flags)
{
	int dist;

	return calc_map_type_and_dist(f->provider, f->client, &dist, tlp_flags,
				      false);
}

static void acs_walk_bus_addr_test(struct kunit *test)
{
	struct acs_fabric f = {};

	acs_build_fabric(test, &f);
	KUNIT_EXPECT_EQ(test, acs_walk_map(&f, 0), PCI_P2PDMA_MAP_BUS_ADDR);
}

static void acs_walk_request_redirect_test(struct kunit *test)
{
	struct acs_fabric f = {};

	acs_build_fabric(test, &f);
	f.dn1->acs_cap = 0x100;
	f.dn1->acs_capabilities = PCI_ACS_RR;
	f.dn1_cfg.ctrl = PCI_ACS_RR;

	KUNIT_EXPECT_EQ(test, acs_walk_map(&f, 0),
			PCI_P2PDMA_MAP_THRU_HOST_BRIDGE);
}

static void acs_walk_completion_redirect_test(struct kunit *test)
{
	struct acs_fabric f = {};

	acs_build_fabric(test, &f);
	f.dn0->acs_cap = 0x100;
	f.dn0->acs_capabilities = PCI_ACS_CR;
	f.dn0_cfg.ctrl = PCI_ACS_CR;

	KUNIT_EXPECT_EQ(test, acs_walk_map(&f, 0),
			PCI_P2PDMA_MAP_THRU_HOST_BRIDGE);
}

static void acs_walk_egress_control_test(struct kunit *test)
{
	struct acs_fabric f = {};

	acs_build_fabric(test, &f);
	f.dn1->acs_cap = 0x100;
	f.dn1->acs_capabilities = PCI_ACS_EC;
	f.dn1_cfg.ctrl = PCI_ACS_EC;

	KUNIT_EXPECT_EQ(test, acs_walk_map(&f, 0),
			PCI_P2PDMA_MAP_THRU_HOST_BRIDGE);
}

static void acs_walk_asymmetric_direct_test(struct kunit *test)
{
	struct acs_fabric f = {};

	acs_build_fabric(test, &f);
	/* These controls affect only the reverse transaction directions. */
	f.dn0->acs_cap = 0x100;
	f.dn0->acs_capabilities = PCI_ACS_RR | PCI_ACS_EC;
	f.dn0_cfg.ctrl = PCI_ACS_RR | PCI_ACS_EC;
	f.dn1->acs_cap = 0x100;
	f.dn1->acs_capabilities = PCI_ACS_CR;
	f.dn1_cfg.ctrl = PCI_ACS_CR;

	KUNIT_EXPECT_EQ(test, acs_walk_map(&f, 0), PCI_P2PDMA_MAP_BUS_ADDR);
}

static void acs_walk_nested_completion_redirect_test(struct kunit *test)
{
	struct acs_fabric f = {};

	acs_build_fabric(test, &f);
	f.provider_leaf->acs_cap = 0x100;
	f.provider_leaf->acs_capabilities = PCI_ACS_CR;
	f.provider_leaf_cfg.ctrl = PCI_ACS_CR;

	KUNIT_EXPECT_EQ(test, acs_walk_map(&f, 0), PCI_P2PDMA_MAP_BUS_ADDR);
}

static void acs_walk_nested_request_redirect_test(struct kunit *test)
{
	struct acs_fabric f = {};

	acs_build_fabric(test, &f);
	f.client_leaf->acs_cap = 0x100;
	f.client_leaf->acs_capabilities = PCI_ACS_RR;
	f.client_leaf_cfg.ctrl = PCI_ACS_RR;

	KUNIT_EXPECT_EQ(test, acs_walk_map(&f, 0), PCI_P2PDMA_MAP_BUS_ADDR);
}

static void acs_walk_translation_blocking_test(struct kunit *test)
{
	struct acs_fabric f = {};

	acs_build_fabric(test, &f);
	f.client_leaf->acs_cap = 0x100;
	f.client_leaf->acs_capabilities = PCI_ACS_TB;
	f.client_leaf_cfg.ctrl = PCI_ACS_TB;

	/* Untranslated Requests are unaffected by Translation Blocking. */
	KUNIT_EXPECT_EQ(test, acs_walk_map(&f, 0), PCI_P2PDMA_MAP_BUS_ADDR);
	KUNIT_EXPECT_EQ(test, acs_walk_map(&f, PCI_P2PDMA_TLP_TRANSLATED),
			PCI_P2PDMA_MAP_NOT_SUPPORTED);
}

static void acs_walk_relaxed_completion_test(struct kunit *test)
{
	struct acs_fabric f = {};

	acs_build_fabric(test, &f);
	f.dn0->acs_cap = 0x100;
	f.dn0->acs_capabilities = PCI_ACS_CR;
	f.dn0_cfg.ctrl = PCI_ACS_CR;

	KUNIT_EXPECT_EQ(test, acs_walk_map(&f, 0),
			PCI_P2PDMA_MAP_THRU_HOST_BRIDGE);
	KUNIT_EXPECT_EQ(test, acs_walk_map(&f, PCI_P2PDMA_TLP_RELAXED_CPL),
			PCI_P2PDMA_MAP_BUS_ADDR);
}

static void acs_walk_direct_translated_test(struct kunit *test)
{
	struct acs_fabric f = {};

	acs_build_fabric(test, &f);
	f.dn1->acs_cap = 0x100;
	f.dn1->acs_capabilities = PCI_ACS_RR | PCI_ACS_DT;
	f.dn1_cfg.ctrl = PCI_ACS_RR | PCI_ACS_DT;

	KUNIT_EXPECT_EQ(test, acs_walk_map(&f, 0),
			PCI_P2PDMA_MAP_THRU_HOST_BRIDGE);
	KUNIT_EXPECT_EQ(test, acs_walk_map(&f, PCI_P2PDMA_TLP_TRANSLATED),
			PCI_P2PDMA_MAP_BUS_ADDR);
}

/*
 * The cache stores one packed value per client, so every class has to come
 * back out under the flags that selected it.
 */
static void acs_map_types_pack_test(struct kunit *test)
{
	static const enum pci_p2pdma_map_type type[PCI_P2PDMA_TLP_CLASSES] = {
		[0] = PCI_P2PDMA_MAP_THRU_HOST_BRIDGE,
		[PCI_P2PDMA_TLP_TRANSLATED] = PCI_P2PDMA_MAP_NOT_SUPPORTED,
		[PCI_P2PDMA_TLP_RELAXED_CPL] = PCI_P2PDMA_MAP_BUS_ADDR,
		[PCI_P2PDMA_TLP_TRANSLATED | PCI_P2PDMA_TLP_RELAXED_CPL] =
			PCI_P2PDMA_MAP_UNKNOWN,
	};
	unsigned long packed = pci_p2pdma_map_types_pack(type);
	unsigned int flags;

	for (flags = 0; flags < PCI_P2PDMA_TLP_CLASSES; flags++)
		KUNIT_EXPECT_EQ(test,
				pci_p2pdma_map_types_unpack(packed, flags),
				type[flags]);

	/* An absent cache entry reads back as unknown in every class. */
	for (flags = 0; flags < PCI_P2PDMA_TLP_CLASSES; flags++)
		KUNIT_EXPECT_EQ(test, pci_p2pdma_map_types_unpack(0, flags),
				PCI_P2PDMA_MAP_UNKNOWN);
}

/*
 * The provider can be an ancestor of the client, which leaves no divergence
 * to evaluate. Translation Blocking still applies to every port the Request
 * passes on its way up.
 */
static void acs_walk_ancestor_provider_tb_test(struct kunit *test)
{
	struct acs_fabric f = {};
	int dist;

	acs_build_fabric(test, &f);
	f.client_leaf->acs_cap = 0x100;
	f.client_leaf->acs_capabilities = PCI_ACS_TB;
	f.client_leaf_cfg.ctrl = PCI_ACS_TB;

	KUNIT_EXPECT_EQ(test,
			calc_map_type_and_dist(f.dn1, f.client, &dist, 0,
					       false),
			PCI_P2PDMA_MAP_BUS_ADDR);
	KUNIT_EXPECT_EQ(test,
			calc_map_type_and_dist(f.dn1, f.client, &dist,
					       PCI_P2PDMA_TLP_TRANSLATED,
					       false),
			PCI_P2PDMA_MAP_NOT_SUPPORTED);
}

/*
 * Without a common upstream bridge the Request still climbs towards the host
 * bridge, so Translation Blocking on the way withdraws the Translated classes
 * there too.
 */
static void acs_walk_no_common_bridge_tb_test(struct kunit *test)
{
	struct acs_fabric f = {}, g = {};
	int dist;

	acs_build_fabric(test, &f);
	acs_build_fabric(test, &g);
	f.client_leaf->acs_cap = 0x100;
	f.client_leaf->acs_capabilities = PCI_ACS_TB;
	f.client_leaf_cfg.ctrl = PCI_ACS_TB;

	KUNIT_EXPECT_EQ(test,
			calc_map_type_and_dist(g.provider, f.client, &dist, 0,
					       false),
			PCI_P2PDMA_MAP_THRU_HOST_BRIDGE);
	KUNIT_EXPECT_EQ(test,
			calc_map_type_and_dist(g.provider, f.client, &dist,
					       PCI_P2PDMA_TLP_TRANSLATED,
					       false),
			PCI_P2PDMA_MAP_NOT_SUPPORTED);
}

/*
 * A Request between the client and itself never leaves the device, so no port
 * is in a position to inspect its Address Type. Translation Blocking directly
 * above the client must not withdraw the Translated classes.
 */
static void acs_walk_self_dma_tb_test(struct kunit *test)
{
	struct acs_fabric f = {};
	int dist;

	acs_build_fabric(test, &f);
	f.client_leaf->acs_cap = 0x100;
	f.client_leaf->acs_capabilities = PCI_ACS_TB;
	f.client_leaf_cfg.ctrl = PCI_ACS_TB;

	KUNIT_EXPECT_EQ(test,
			calc_map_type_and_dist(f.client, f.client, &dist, 0,
					       false),
			PCI_P2PDMA_MAP_BUS_ADDR);
	KUNIT_EXPECT_EQ(test,
			calc_map_type_and_dist(f.client, f.client, &dist,
					       PCI_P2PDMA_TLP_TRANSLATED,
					       false),
			PCI_P2PDMA_MAP_BUS_ADDR);
}

/*
 * A direct route turns around at the divergence, so Translation Blocking above
 * it does not touch one. The host bridge route keeps climbing past that port,
 * and the Translated classes have to go without it.
 */
static void acs_walk_tb_above_divergence_test(struct kunit *test)
{
	struct acs_fabric f = {};
	int dist;

	acs_build_fabric(test, &f);
	f.rootport->acs_cap = 0x100;
	f.rootport->acs_capabilities = PCI_ACS_TB;
	f.rootport_cfg.ctrl = PCI_ACS_TB;

	/* Nothing redirects yet, so the Request never reaches the Root Port. */
	KUNIT_EXPECT_EQ(test,
			calc_map_type_and_dist(f.provider, f.client, &dist,
					       PCI_P2PDMA_TLP_TRANSLATED,
					       false),
			PCI_P2PDMA_MAP_BUS_ADDR);

	/* Request Redirect sends it up past the Root Port instead. */
	f.dn1->acs_cap = 0x100;
	f.dn1->acs_capabilities = PCI_ACS_RR;
	f.dn1_cfg.ctrl = PCI_ACS_RR;

	KUNIT_EXPECT_EQ(test,
			calc_map_type_and_dist(f.provider, f.client, &dist, 0,
					       false),
			PCI_P2PDMA_MAP_THRU_HOST_BRIDGE);
	KUNIT_EXPECT_EQ(test,
			calc_map_type_and_dist(f.provider, f.client, &dist,
					       PCI_P2PDMA_TLP_TRANSLATED,
					       false),
			PCI_P2PDMA_MAP_NOT_SUPPORTED);
}

static void acs_walk_unreadable_control_test(struct kunit *test)
{
	struct acs_fabric f = {};

	acs_build_fabric(test, &f);
	f.dn1->acs_cap = 0x100;
	f.dn1_cfg.fail_read = true;

	KUNIT_EXPECT_EQ(test, acs_walk_map(&f, 0),
			PCI_P2PDMA_MAP_NOT_SUPPORTED);
}

static struct kunit_case pci_acs_test_cases[] = {
	KUNIT_CASE_PARAM(pci_acs_p2pdma_request_test,
			 acs_request_gen_params),
	KUNIT_CASE_PARAM(pci_acs_p2pdma_completion_test,
			 acs_completion_gen_params),
	KUNIT_CASE_PARAM(pci_acs_flags_enabled_test,
			 acs_isolation_gen_params),
	KUNIT_CASE(pci_acs_flags_no_cap_test),
	KUNIT_CASE(pci_acs_flags_read_fails_test),
	KUNIT_CASE(acs_walk_bus_addr_test),
	KUNIT_CASE(acs_walk_request_redirect_test),
	KUNIT_CASE(acs_walk_completion_redirect_test),
	KUNIT_CASE(acs_walk_egress_control_test),
	KUNIT_CASE(acs_walk_asymmetric_direct_test),
	KUNIT_CASE(acs_walk_nested_completion_redirect_test),
	KUNIT_CASE(acs_walk_nested_request_redirect_test),
	KUNIT_CASE(acs_walk_translation_blocking_test),
	KUNIT_CASE(acs_walk_ancestor_provider_tb_test),
	KUNIT_CASE(acs_walk_no_common_bridge_tb_test),
	KUNIT_CASE(acs_walk_self_dma_tb_test),
	KUNIT_CASE(acs_walk_relaxed_completion_test),
	KUNIT_CASE(acs_walk_direct_translated_test),
	KUNIT_CASE(acs_walk_tb_above_divergence_test),
	KUNIT_CASE(acs_walk_unreadable_control_test),
	KUNIT_CASE(acs_map_types_pack_test),
	{}
};

static struct kunit_suite pci_acs_test_suite = {
	.name = "pci_acs",
	.test_cases = pci_acs_test_cases,
};
kunit_test_suite(pci_acs_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for PCI ACS peer-to-peer routing decisions");
