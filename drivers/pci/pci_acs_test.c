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

static struct kunit_case pci_acs_test_cases[] = {
	KUNIT_CASE_PARAM(pci_acs_p2pdma_decision_test, acs_decision_gen_params),
	KUNIT_CASE_PARAM(pci_acs_egress_port_valid_test, egress_valid_gen_params),
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
