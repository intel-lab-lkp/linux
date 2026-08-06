// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for PCI ACS peer-to-peer routing decision logic.
 *
 * These exercise the pure helpers factored out of the ACS Egress Control
 * handling (PCIe r7.0, sec 6.12.3, table 6-11).  They cover the code paths
 * that require an ACS Egress Control Vector, which cannot be reached on the
 * peer-to-peer hardware commonly available for testing.
 */
#include <kunit/test.h>

#include <linux/pci.h>
#include <linux/pci-p2pdma.h>
#include <linux/pci_regs.h>

#include "pci.h"

/* pci_acs_p2pdma_decision(): the table 6-11 truth table. */

struct acs_decision_case {
	const char *desc;
	u16 ctrl;
	bool has_target;
	int egress;
	enum pci_acs_p2pdma_state expect;
};

/* Shorthands to keep the table below readable. */
#define ACS_DIRECT	PCI_ACS_P2PDMA_DIRECT
#define ACS_REDIR	PCI_ACS_P2PDMA_REDIRECT
#define ACS_NO_P2P	PCI_ACS_P2PDMA_NOT_SUPPORTED

static const struct acs_decision_case acs_decision_cases[] = {
	/* No target known: Egress Control is ignored, RR/CR decide. */
	{ "no_target/none", 0, false, 0, ACS_DIRECT },
	{ "no_target/rr", PCI_ACS_RR, false, 0, ACS_REDIR },
	{ "no_target/cr", PCI_ACS_CR, false, 0, ACS_REDIR },
	{ "no_target/ec_only", PCI_ACS_EC, false, 0, ACS_DIRECT },

	/* Target known but EC clear: RR/CR decide, egress not consulted. */
	{ "ec_clear/none", 0, true, 0, ACS_DIRECT },
	{ "ec_clear/rr", PCI_ACS_RR, true, 0, ACS_REDIR },
	{ "ec_clear/cr", PCI_ACS_CR, true, 0, ACS_REDIR },
	{ "ec_clear/rr_cr", PCI_ACS_RR | PCI_ACS_CR, true, 0, ACS_REDIR },

	/* EC set but vector unreadable: never a usable P2P route. */
	{ "ec/eopnotsupp", PCI_ACS_EC | PCI_ACS_RR, true, -EOPNOTSUPP, ACS_NO_P2P },
	{ "ec/erange", PCI_ACS_EC | PCI_ACS_CR, true, -ERANGE, ACS_NO_P2P },

	/* EC set, vector bit set: redirect iff RR, else ACS Violation. */
	{ "ec/vec_set/none", PCI_ACS_EC, true, 1, ACS_NO_P2P },
	{ "ec/vec_set/cr", PCI_ACS_EC | PCI_ACS_CR, true, 1, ACS_NO_P2P },
	{ "ec/vec_set/rr", PCI_ACS_EC | PCI_ACS_RR, true, 1, ACS_REDIR },
	{ "ec/vec_set/rr_cr", PCI_ACS_EC | PCI_ACS_RR | PCI_ACS_CR, true, 1,
	  ACS_REDIR },

	/* EC set, vector bit clear: direct unless CR redirects. */
	{ "ec/vec_clear/none", PCI_ACS_EC, true, 0, ACS_DIRECT },
	{ "ec/vec_clear/rr", PCI_ACS_EC | PCI_ACS_RR, true, 0, ACS_DIRECT },
	{ "ec/vec_clear/cr", PCI_ACS_EC | PCI_ACS_CR, true, 0, ACS_REDIR },
	{ "ec/vec_clear/rr_cr", PCI_ACS_EC | PCI_ACS_RR | PCI_ACS_CR, true, 0,
	  ACS_REDIR },
};

#undef ACS_DIRECT
#undef ACS_REDIR
#undef ACS_NO_P2P

static void acs_decision_desc(const struct acs_decision_case *c, char *desc)
{
	strscpy(desc, c->desc, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(acs_decision, acs_decision_cases, acs_decision_desc);

static void pci_acs_p2pdma_decision_test(struct kunit *test)
{
	const struct acs_decision_case *c = test->param_value;

	KUNIT_EXPECT_EQ(test,
			pci_acs_p2pdma_decision(c->ctrl, c->has_target, c->egress),
			c->expect);
}

/* pci_acs_egress_port_valid(): the Egress Control Vector Size rule. */

struct egress_valid_case {
	const char *desc;
	u16 acs_caps;
	u8 target_port;
	bool expect;
};

static const struct egress_valid_case egress_valid_cases[] = {
	/* A Vector Size of 0 encodes 256 bits, so every port is addressable. */
	{ "size0/port0",	0x0000,	  0, true },
	{ "size0/port255",	0x0000,	255, true },
	/* Vector Size N (bits 15:8): ports [0, N) are addressable. */
	{ "size1/port0",	0x0100,	  0, true },
	{ "size1/port1",	0x0100,	  1, false },
	{ "size8/port7",	0x0800,	  7, true },
	{ "size8/port8",	0x0800,	  8, false },
	{ "size255/port254",	0xff00,	254, true },
	{ "size255/port255",	0xff00,	255, false },
};

static void egress_valid_desc(const struct egress_valid_case *c, char *desc)
{
	strscpy(desc, c->desc, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(egress_valid, egress_valid_cases, egress_valid_desc);

static void pci_acs_egress_port_valid_test(struct kunit *test)
{
	const struct egress_valid_case *c = test->param_value;

	KUNIT_EXPECT_EQ(test,
			pci_acs_egress_port_valid(c->acs_caps, c->target_port),
			c->expect);
}

/*
 * pci_acs_flags_enabled(): Direct Translated P2P and Egress Control both let a
 * peer request reach the peer without Request Redirect, so neither may report
 * isolation.  Translation Blocking rejects a Translated Request before it is
 * routed, which restores the Request Redirect guarantee.  A fake pci_ops
 * supplies the ACS Control register.
 */

/* Flags an IOMMU asks for; see REQ_ACS_FLAGS in drivers/iommu/iommu.c. */
#define ACS_REQ_FLAGS	(PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF)
#define ACS_ALL_CAPS	(PCI_ACS_SV | PCI_ACS_TB | PCI_ACS_RR | PCI_ACS_CR | \
			 PCI_ACS_UF | PCI_ACS_EC | PCI_ACS_DT)
#define ACS_TEST_CAP	0x100
/* Flags pci_enable_pasid() asks for; see drivers/pci/ats.c. */
#define ACS_PASID_FLAGS	(PCI_ACS_RR | PCI_ACS_UF)

struct acs_ctrl_cfg {
	unsigned int	devfn;
	u16		cap;		/* offset the ACS capability answers at */
	u16		ctrl;
	bool		fail_read;
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
	u16	ctrl;			/* ACS Control register */
	u16	req;			/* flags the caller asks for */
	enum pci_acs_scope scope;	/* Requests the answer must cover */
	bool	expect;			/* isolation reported? */
};

static const struct acs_isolation_case acs_isolation_cases[] = {
	{ "plain_rr", ACS_REQ_FLAGS, ACS_REQ_FLAGS, PCI_ACS_SCOPE_ALL, true },
	/* Direct Translated P2P bypasses Request Redirect ... */
	{ "dt", ACS_REQ_FLAGS | PCI_ACS_DT, ACS_REQ_FLAGS,
	  PCI_ACS_SCOPE_ALL, false },
	/* ... unless Translation Blocking rejects the Translated Request. */
	{ "dt_tb", ACS_REQ_FLAGS | PCI_ACS_DT | PCI_ACS_TB, ACS_REQ_FLAGS,
	  PCI_ACS_SCOPE_ALL, true },
	{ "tb_only", ACS_REQ_FLAGS | PCI_ACS_TB, ACS_REQ_FLAGS,
	  PCI_ACS_SCOPE_ALL, true },
	/* Egress Control can override Request Redirect as well. */
	{ "ec", ACS_REQ_FLAGS | PCI_ACS_EC, ACS_REQ_FLAGS,
	  PCI_ACS_SCOPE_ALL, false },
	{ "ec_dt_tb", ACS_REQ_FLAGS | PCI_ACS_EC | PCI_ACS_DT | PCI_ACS_TB,
	  ACS_REQ_FLAGS, PCI_ACS_SCOPE_ALL, false },
	/* Without Request Redirect requested, neither bit is consulted. */
	{ "no_rr_dt", PCI_ACS_SV | PCI_ACS_CR | PCI_ACS_UF | PCI_ACS_DT,
	  PCI_ACS_SV | PCI_ACS_CR | PCI_ACS_UF, PCI_ACS_SCOPE_ALL, true },
	/* A control bit the caller asked for is simply missing. */
	{ "rr_not_enabled", PCI_ACS_SV | PCI_ACS_CR | PCI_ACS_UF, ACS_REQ_FLAGS,
	  PCI_ACS_SCOPE_ALL, false },

	/*
	 * An Untranslated-only caller such as pci_enable_pasid() is not
	 * affected by Direct Translated P2P, but is still affected by Egress
	 * Control, which acts on Untranslated peer Requests too.
	 */
	{ "untrans/plain_rr", ACS_PASID_FLAGS, ACS_PASID_FLAGS,
	  PCI_ACS_SCOPE_UNTRANSLATED, true },
	{ "untrans/dt", ACS_PASID_FLAGS | PCI_ACS_DT, ACS_PASID_FLAGS,
	  PCI_ACS_SCOPE_UNTRANSLATED, true },
	{ "untrans/dt_tb", ACS_PASID_FLAGS | PCI_ACS_DT | PCI_ACS_TB,
	  ACS_PASID_FLAGS, PCI_ACS_SCOPE_UNTRANSLATED, true },
	{ "untrans/ec", ACS_PASID_FLAGS | PCI_ACS_EC, ACS_PASID_FLAGS,
	  PCI_ACS_SCOPE_UNTRANSLATED, false },
	{ "untrans/rr_not_enabled", PCI_ACS_UF, ACS_PASID_FLAGS,
	  PCI_ACS_SCOPE_UNTRANSLATED, false },
};

static void acs_isolation_desc(const struct acs_isolation_case *c, char *desc)
{
	strscpy(desc, c->desc, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(acs_isolation, acs_isolation_cases, acs_isolation_desc);

static void pci_acs_flags_enabled_test(struct kunit *test)
{
	const struct acs_isolation_case *c = test->param_value;
	struct acs_ctrl_cfg cfg = { .devfn = PCI_DEVFN(0, 0),
				    .cap = ACS_TEST_CAP, .ctrl = c->ctrl };
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

	KUNIT_EXPECT_EQ(test, pci_acs_flags_enabled(pdev, c->req, c->scope),
			c->expect);
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

	return pci_acs_flags_enabled(pdev, acs_flags, PCI_ACS_SCOPE_ALL);
}

/*
 * Without an ACS capability there is no control register to consult.  The
 * fake answers at offset 0 here, so dropping the acs_cap guard would read an
 * isolating control word rather than nothing.
 */
static void pci_acs_flags_no_cap_test(struct kunit *test)
{
	struct acs_ctrl_cfg cfg = { .devfn = PCI_DEVFN(0, 0), .cap = 0,
				    .ctrl = ACS_REQ_FLAGS };

	KUNIT_EXPECT_FALSE(test, acs_isolated(test, &cfg, 0, ACS_REQ_FLAGS));
}

/*
 * An unreadable ACS Control register reads back as all ones, which satisfies
 * any requested control.  Ask without Request Redirect, so that neither
 * pci_acs_rr_ineffective() nor the control word itself can deny isolation and
 * the read failure is the only thing left that can.
 */
static void pci_acs_flags_read_fails_test(struct kunit *test)
{
	u16 no_rr = ACS_REQ_FLAGS & ~PCI_ACS_RR;
	struct acs_ctrl_cfg cfg = { .devfn = PCI_DEVFN(0, 0),
				    .cap = ACS_TEST_CAP,
				    .ctrl = ACS_REQ_FLAGS };

	KUNIT_EXPECT_TRUE(test, acs_isolated(test, &cfg, ACS_TEST_CAP, no_rr));

	cfg.fail_read = true;
	KUNIT_EXPECT_FALSE(test, acs_isolated(test, &cfg, ACS_TEST_CAP, no_rr));
}

/*
 * pci_acs_egress_ctrl_is_set(): drive the config-space reads with a fake pci_ops
 * so the Egress Control Vector lookup is exercised without real hardware --
 * the target Port Number from LNKCAP, the vector DWORD at target_port/32, and
 * the bit at target_port%32.
 */

/* PCIe Capabilities register value: device/port @type, capability version 2. */
#define ACS_TEST_PCIE_FLAGS(type)	(((type) << 4) | 0x2)
#define ACS_DOWNSTREAM			ACS_TEST_PCIE_FLAGS(PCI_EXP_TYPE_DOWNSTREAM)
#define ACS_ENDPOINT			ACS_TEST_PCIE_FLAGS(PCI_EXP_TYPE_ENDPOINT)
#define ACS_ROOT_PORT			ACS_TEST_PCIE_FLAGS(PCI_EXP_TYPE_ROOT_PORT)
#define ACS_PCIE_BRIDGE			ACS_TEST_PCIE_FLAGS(PCI_EXP_TYPE_PCIE_BRIDGE)

struct acs_fake_cfg {
	unsigned int	pdev_devfn;
	unsigned int	target_devfn;
	u16		pdev_acs_cap;
	u8		target_pcie_cap;
	u8		target_port;
	u32		egress_vector[8];	/* full 256-bit vector */
};

static int acs_fake_cfg_read(struct pci_bus *bus, unsigned int devfn,
			     int where, int size, u32 *val)
{
	struct acs_fake_cfg *cfg = bus->sysdata;

	*val = 0;
	if (size != 4)
		return PCIBIOS_SUCCESSFUL;

	if (devfn == cfg->target_devfn &&
	    where == cfg->target_pcie_cap + PCI_EXP_LNKCAP) {
		*val = FIELD_PREP(PCI_EXP_LNKCAP_PN, cfg->target_port);
	} else if (devfn == cfg->pdev_devfn) {
		int base = cfg->pdev_acs_cap + PCI_ACS_EGRESS_CTL_V;

		if (where >= base &&
		    where < base + (int)sizeof(cfg->egress_vector))
			*val = cfg->egress_vector[(where - base) / 4];
	}
	return PCIBIOS_SUCCESSFUL;
}

static int acs_fake_cfg_write(struct pci_bus *bus, unsigned int devfn,
			      int where, int size, u32 val)
{
	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops acs_fake_ops = {
	.read	= acs_fake_cfg_read,
	.write	= acs_fake_cfg_write,
};

static struct acs_fake_cfg acs_base_cfg(void)
{
	return (struct acs_fake_cfg){
		.pdev_devfn	 = PCI_DEVFN(0, 0),
		.target_devfn	 = PCI_DEVFN(1, 0),
		.pdev_acs_cap	 = 0x100,
		.target_pcie_cap = 0x40,
	};
}

static int acs_egress_ctrl_set(struct kunit *test, struct acs_fake_cfg *cfg,
			       u16 pdev_acs_caps, u16 pdev_flags, u16 target_flags)
{
	struct pci_bus *bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	struct pci_dev *pdev = kunit_kzalloc(test, sizeof(*pdev), GFP_KERNEL);
	struct pci_dev *target = kunit_kzalloc(test, sizeof(*target), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, bus);
	KUNIT_ASSERT_NOT_NULL(test, pdev);
	KUNIT_ASSERT_NOT_NULL(test, target);

	bus->ops = &acs_fake_ops;
	bus->sysdata = cfg;

	pdev->bus = bus;
	pdev->devfn = cfg->pdev_devfn;
	pdev->acs_cap = cfg->pdev_acs_cap;
	pdev->acs_capabilities = pdev_acs_caps;
	pdev->pcie_cap = 0x40;
	pdev->pcie_flags_reg = pdev_flags;

	target->bus = bus;
	target->devfn = cfg->target_devfn;
	target->pcie_cap = cfg->target_pcie_cap;
	target->pcie_flags_reg = target_flags;

	return pci_acs_egress_ctrl_is_set(pdev, target);
}

static void acs_egress_vector_bit_set_test(struct kunit *test)
{
	struct acs_fake_cfg cfg = acs_base_cfg();

	cfg.target_port = 5;
	cfg.egress_vector[0] = BIT(5);

	KUNIT_EXPECT_EQ(test,
			acs_egress_ctrl_set(test, &cfg, PCI_ACS_EC | (32 << 8),
					    ACS_DOWNSTREAM, ACS_DOWNSTREAM),
			1);
}

static void acs_egress_vector_bit_clear_test(struct kunit *test)
{
	struct acs_fake_cfg cfg = acs_base_cfg();

	cfg.target_port = 5;	/* vector left all-zero */

	KUNIT_EXPECT_EQ(test,
			acs_egress_ctrl_set(test, &cfg, PCI_ACS_EC | (32 << 8),
					    ACS_DOWNSTREAM, ACS_DOWNSTREAM),
			0);
}

static void acs_egress_high_port_index_test(struct kunit *test)
{
	struct acs_fake_cfg cfg = acs_base_cfg();

	/* Port 40 lives in vector DWORD 1, bit 8: exercises target_port/32. */
	cfg.target_port = 40;
	cfg.egress_vector[1] = BIT(40 % 32);

	KUNIT_EXPECT_EQ(test,
			acs_egress_ctrl_set(test, &cfg, PCI_ACS_EC | (64 << 8),
					    ACS_DOWNSTREAM, ACS_DOWNSTREAM),
			1);
}

static void acs_egress_port_out_of_range_test(struct kunit *test)
{
	struct acs_fake_cfg cfg = acs_base_cfg();

	/* Vector Size 8, port 40 is beyond it. */
	cfg.target_port = 40;

	KUNIT_EXPECT_EQ(test,
			acs_egress_ctrl_set(test, &cfg, PCI_ACS_EC | (8 << 8),
					    ACS_DOWNSTREAM, ACS_DOWNSTREAM),
			-ERANGE);
}

static void acs_egress_no_ec_cap_test(struct kunit *test)
{
	struct acs_fake_cfg cfg = acs_base_cfg();

	cfg.target_port = 5;

	/* acs_capabilities without PCI_ACS_EC: unsupported. */
	KUNIT_EXPECT_EQ(test,
			acs_egress_ctrl_set(test, &cfg, 32 << 8,
					    ACS_DOWNSTREAM, ACS_DOWNSTREAM),
			-EOPNOTSUPP);
}

static void acs_egress_pdev_not_downstream_test(struct kunit *test)
{
	struct acs_fake_cfg cfg = acs_base_cfg();

	cfg.target_port = 5;

	KUNIT_EXPECT_EQ(test,
			acs_egress_ctrl_set(test, &cfg, PCI_ACS_EC | (32 << 8),
					    ACS_ENDPOINT, ACS_DOWNSTREAM),
			-EOPNOTSUPP);
}

static void acs_egress_target_not_downstream_test(struct kunit *test)
{
	struct acs_fake_cfg cfg = acs_base_cfg();

	cfg.target_port = 5;

	KUNIT_EXPECT_EQ(test,
			acs_egress_ctrl_set(test, &cfg, PCI_ACS_EC | (32 << 8),
					    ACS_DOWNSTREAM, ACS_ENDPOINT),
			-EOPNOTSUPP);
}

/*
 * The vector is indexed by Port Number only for Root Ports and Switch
 * Downstream Ports, so a PCI/PCI-X to PCIe Bridge must not be indexed by its
 * Link Capabilities Port Number.
 */
static void acs_egress_pdev_pcie_bridge_test(struct kunit *test)
{
	struct acs_fake_cfg cfg = acs_base_cfg();

	cfg.target_port = 5;

	KUNIT_EXPECT_EQ(test,
			acs_egress_ctrl_set(test, &cfg, PCI_ACS_EC | (32 << 8),
					    ACS_PCIE_BRIDGE, ACS_DOWNSTREAM),
			-EOPNOTSUPP);
}

static void acs_egress_target_pcie_bridge_test(struct kunit *test)
{
	struct acs_fake_cfg cfg = acs_base_cfg();

	cfg.target_port = 5;

	KUNIT_EXPECT_EQ(test,
			acs_egress_ctrl_set(test, &cfg, PCI_ACS_EC | (32 << 8),
					    ACS_DOWNSTREAM, ACS_PCIE_BRIDGE),
			-EOPNOTSUPP);
}

/*
 * Each vector bit is a Port Number within one Switch or Root Complex (PCIe
 * r7.0, sec 7.7.12.4), so a target that does not share the ingress port's bus
 * has no bit here even when the bit at its Port Number is set.
 */
static void acs_egress_target_other_bus_test(struct kunit *test)
{
	struct acs_fake_cfg cfg = acs_base_cfg();
	struct pci_bus *bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	struct pci_bus *other = kunit_kzalloc(test, sizeof(*other), GFP_KERNEL);
	struct pci_dev *pdev = kunit_kzalloc(test, sizeof(*pdev), GFP_KERNEL);
	struct pci_dev *target = kunit_kzalloc(test, sizeof(*target), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, bus);
	KUNIT_ASSERT_NOT_NULL(test, other);
	KUNIT_ASSERT_NOT_NULL(test, pdev);
	KUNIT_ASSERT_NOT_NULL(test, target);

	cfg.target_port = 5;
	cfg.egress_vector[0] = BIT(5);

	bus->ops = &acs_fake_ops;
	bus->sysdata = &cfg;
	other->ops = &acs_fake_ops;
	other->sysdata = &cfg;

	pdev->bus = bus;
	pdev->devfn = cfg.pdev_devfn;
	pdev->acs_cap = cfg.pdev_acs_cap;
	pdev->acs_capabilities = PCI_ACS_EC | (32 << 8);
	pdev->pcie_cap = 0x40;
	pdev->pcie_flags_reg = ACS_DOWNSTREAM;

	target->bus = other;
	target->devfn = cfg.target_devfn;
	target->pcie_cap = cfg.target_pcie_cap;
	target->pcie_flags_reg = ACS_DOWNSTREAM;

	KUNIT_EXPECT_EQ(test, pci_acs_egress_ctrl_is_set(pdev, target),
			-EOPNOTSUPP);
}

/* A Root Port is a valid ingress and egress port for the vector. */
static void acs_egress_root_port_test(struct kunit *test)
{
	struct acs_fake_cfg cfg = acs_base_cfg();

	cfg.target_port = 5;
	cfg.egress_vector[0] = BIT(5);

	KUNIT_EXPECT_EQ(test,
			acs_egress_ctrl_set(test, &cfg, PCI_ACS_EC | (32 << 8),
					    ACS_ROOT_PORT, ACS_ROOT_PORT),
			1);
}

/*
 * calc_map_type_and_dist(): drive the full provider->client hierarchy walk
 * over a fabricated PCIe fabric matching the canonical "two devices behind one
 * switch" tree:
 *
 *   host bridge / root bus
 *     Root Port
 *       Switch Upstream Port
 *         Switch Downstream Port 0 -- provider
 *         Switch Downstream Port 1 -- client
 *
 * A fake pci_ops answers the ACS Control, Egress Control Vector and LNKCAP
 * reads for the two downstream ports, so the ACS Egress Control evaluated at
 * the path divergence (Downstream Port 0 targeting Downstream Port 1) decides
 * the mapping without any real hardware.
 */

struct acs_dn_cfg {
	u16	acs_ctrl;		/* ACS Control register value */
	u8	port;			/* this port's LNKCAP Port Number */
	u32	egress[8];		/* Egress Control Vector (256 bits) */
};

struct acs_fabric {
	struct pci_dev		*provider;
	struct pci_dev		*client;
	struct pci_dev		*dn0;	/* Downstream Port 0 (provider side) */
	struct pci_dev		*dn1;	/* Downstream Port 1 (client side) */
	struct acs_dn_cfg	dn0_cfg;
	struct acs_dn_cfg	dn1_cfg;
};

static void acs_dn_read(struct pci_dev *dn, struct acs_dn_cfg *c,
			int where, int size, u32 *val)
{
	int vec = dn->acs_cap + PCI_ACS_EGRESS_CTL_V;

	if (size == 4 && where == dn->pcie_cap + PCI_EXP_LNKCAP)
		*val = FIELD_PREP(PCI_EXP_LNKCAP_PN, c->port);
	else if (dn->acs_cap && size == 2 && where == dn->acs_cap + PCI_ACS_CTRL)
		*val = c->acs_ctrl;
	else if (dn->acs_cap && size == 4 &&
		 where >= vec && where < vec + (int)sizeof(c->egress))
		*val = c->egress[(where - vec) / 4];
}

static int acs_fabric_read(struct pci_bus *bus, unsigned int devfn,
			   int where, int size, u32 *val)
{
	struct acs_fabric *f = bus->sysdata;

	*val = 0;
	if (bus == f->dn0->bus && devfn == f->dn0->devfn)
		acs_dn_read(f->dn0, &f->dn0_cfg, where, size, val);
	else if (bus == f->dn1->bus && devfn == f->dn1->devfn)
		acs_dn_read(f->dn1, &f->dn1_cfg, where, size, val);
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
	dev->pcie_flags_reg = ACS_TEST_PCIE_FLAGS(pcie_type);
	list_add_tail(&dev->bus_list, &bus->devices);
	return dev;
}

static void acs_build_fabric(struct kunit *test, struct acs_fabric *f)
{
	struct pci_bus *bus0, *bus1, *bus2, *bus3, *bus4;
	struct pci_dev *rootport, *swup;
	struct pci_host_bridge *host;

	host = kunit_kzalloc(test, sizeof(*host), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, host);

	bus0 = acs_add_bus(test, NULL, NULL, 0, f);		/* root bus */
	/* The Root Port doubles as the whitelisted host-bridge device. */
	rootport = acs_add_dev(test, bus0, PCI_DEVFN(0, 0),
			       PCI_EXP_TYPE_ROOT_PORT);
	rootport->vendor = PCI_VENDOR_ID_GOOGLE;
	rootport->device = 0x1234;
	host->bus = bus0;
	bus0->bridge = &host->dev;

	bus1 = acs_add_bus(test, bus0, rootport, 1, f);
	swup = acs_add_dev(test, bus1, PCI_DEVFN(0, 0), PCI_EXP_TYPE_UPSTREAM);

	bus2 = acs_add_bus(test, bus1, swup, 2, f);
	f->dn0 = acs_add_dev(test, bus2, PCI_DEVFN(0, 0), PCI_EXP_TYPE_DOWNSTREAM);
	f->dn1 = acs_add_dev(test, bus2, PCI_DEVFN(1, 0), PCI_EXP_TYPE_DOWNSTREAM);

	bus3 = acs_add_bus(test, bus2, f->dn0, 3, f);
	f->provider = acs_add_dev(test, bus3, PCI_DEVFN(0, 0),
				  PCI_EXP_TYPE_ENDPOINT);

	bus4 = acs_add_bus(test, bus2, f->dn1, 4, f);
	f->client = acs_add_dev(test, bus4, PCI_DEVFN(0, 0),
				PCI_EXP_TYPE_ENDPOINT);
}

static enum pci_p2pdma_map_type acs_walk_map(struct acs_fabric *f)
{
	int dist;

	return calc_map_type_and_dist(f->provider, f->client, &dist, false);
}

static void acs_walk_bus_addr_test(struct kunit *test)
{
	struct acs_fabric f = {};

	acs_build_fabric(test, &f);
	/* No ACS on the path: peer-to-peer is allowed directly. */
	KUNIT_EXPECT_EQ(test, acs_walk_map(&f), PCI_P2PDMA_MAP_BUS_ADDR);
}

static void acs_walk_ec_violation_test(struct kunit *test)
{
	struct acs_fabric f = {};

	acs_build_fabric(test, &f);
	/*
	 * Downstream Port 0 has Egress Control enabled with the vector bit for
	 * the client's Downstream Port 1 set and Request Redirect clear: an ACS
	 * Violation, so the direct path is unusable and the request has to take
	 * the host-bridge route.
	 */
	f.dn0->acs_cap = 0x100;
	f.dn0->acs_capabilities = PCI_ACS_EC | (64 << 8);
	f.dn0_cfg.acs_ctrl = PCI_ACS_EC;
	f.dn1_cfg.port = 5;
	f.dn0_cfg.egress[0] = BIT(5);

	KUNIT_EXPECT_EQ(test, acs_walk_map(&f),
			PCI_P2PDMA_MAP_THRU_HOST_BRIDGE);
}

static void acs_walk_ec_vector_clear_test(struct kunit *test)
{
	struct acs_fabric f = {};

	acs_build_fabric(test, &f);
	/* Egress Control enabled but the vector bit for the peer is clear. */
	f.dn0->acs_cap = 0x100;
	f.dn0->acs_capabilities = PCI_ACS_EC | (64 << 8);
	f.dn0_cfg.acs_ctrl = PCI_ACS_EC;
	f.dn1_cfg.port = 5;		/* egress vector left all-zero */

	KUNIT_EXPECT_EQ(test, acs_walk_map(&f), PCI_P2PDMA_MAP_BUS_ADDR);
}

static void acs_walk_thru_host_bridge_test(struct kunit *test)
{
	struct acs_fabric f = {};

	acs_build_fabric(test, &f);
	/* Request Redirect set: traffic is redirected up to the host bridge. */
	f.dn0->acs_cap = 0x100;
	f.dn0->acs_capabilities = PCI_ACS_RR;
	f.dn0_cfg.acs_ctrl = PCI_ACS_RR;

	/* The Google root port is whitelisted, so the host-bridge path is OK. */
	KUNIT_EXPECT_EQ(test, acs_walk_map(&f),
			PCI_P2PDMA_MAP_THRU_HOST_BRIDGE);
}

static struct kunit_case pci_acs_test_cases[] = {
	KUNIT_CASE_PARAM(pci_acs_p2pdma_decision_test, acs_decision_gen_params),
	KUNIT_CASE_PARAM(pci_acs_egress_port_valid_test, egress_valid_gen_params),
	KUNIT_CASE_PARAM(pci_acs_flags_enabled_test, acs_isolation_gen_params),
	KUNIT_CASE(pci_acs_flags_no_cap_test),
	KUNIT_CASE(pci_acs_flags_read_fails_test),
	KUNIT_CASE(acs_egress_vector_bit_set_test),
	KUNIT_CASE(acs_egress_vector_bit_clear_test),
	KUNIT_CASE(acs_egress_high_port_index_test),
	KUNIT_CASE(acs_egress_port_out_of_range_test),
	KUNIT_CASE(acs_egress_no_ec_cap_test),
	KUNIT_CASE(acs_egress_pdev_not_downstream_test),
	KUNIT_CASE(acs_egress_target_not_downstream_test),
	KUNIT_CASE(acs_egress_pdev_pcie_bridge_test),
	KUNIT_CASE(acs_egress_target_pcie_bridge_test),
	KUNIT_CASE(acs_egress_target_other_bus_test),
	KUNIT_CASE(acs_egress_root_port_test),
	KUNIT_CASE(acs_walk_bus_addr_test),
	KUNIT_CASE(acs_walk_ec_violation_test),
	KUNIT_CASE(acs_walk_ec_vector_clear_test),
	KUNIT_CASE(acs_walk_thru_host_bridge_test),
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
