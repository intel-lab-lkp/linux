// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm MSM8x60 family (MSM8260/MSM8660/APQ8060) interconnect driver
 *
 * Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
 *
 * Based on msm8974.c by Brian Masney <masneyb@onstation.org>
 * and webOS kernel msm_bus_board_8660.c / msm_bus_fabric.c
 * Copyright (c) 2010-2011, Code Aurora Forum. All rights reserved.
 *
 * MSM8x60 has a fabric-based bus architecture:
 *
 *              +------------------+
 *              |   APPSS Fabric   |  (CPU, L2, Memory)
 *              +--------+---------+
 *                       |
 *         +-------------+-------------+
 *         |                           |
 *  +------+------+            +-------+-------+
 *  | MMSS Fabric |            | System Fabric |
 *  | (Display,   |            | (Peripherals, |
 *  |  Camera,    |            |  DMA, etc)    |
 *  |  Video)     |            +-------+-------+
 *  +-------------+                    |
 *                           +---------+---------+
 *                           |                   |
 *                    +------+------+     +------+------+
 *                    | System FPB  |     |  CPSS FPB   |
 *                    | (RPM, PMIC) |     | (GSBI, USB) |
 *                    +-------------+     +-------------+
 *
 * Each fabric has an RPM arbitration interface that programs per-port
 * bandwidth and priority tier via MM_FABRIC_ARB / SYS_FABRIC_ARB /
 * APPS_FABRIC_ARB registers.  The webOS kernel sent these as packed
 * u16 arrays (bwsum + arb) through msm_rpm_set().  This driver uses
 * the mainline qcom_rpm_write() interface to do the same.
 */

#include <dt-bindings/interconnect/qcom,msm8660.h>
#include <dt-bindings/mfd/qcom-rpm.h>

#include <linux/args.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/interconnect-provider.h>
#include <linux/io.h>
#include <linux/mfd/qcom_rpm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

/* Internal node IDs - these map to the DT binding IDs plus fabric offset */
enum {
	/* APPSS Fabric nodes */
	MSM8660_AFAB_MAS_AMPSS_M0 = 1,
	MSM8660_AFAB_MAS_AMPSS_M1,
	MSM8660_AFAB_SLV_EBI_CH0,
	MSM8660_AFAB_SLV_AMPSS_L2,
	MSM8660_AFAB_TO_MMSS,
	MSM8660_AFAB_TO_SYSTEM,

	/* System Fabric nodes */
	MSM8660_SFAB_MAS_APPSS,
	MSM8660_SFAB_MAS_SPS,
	MSM8660_SFAB_MAS_ADM0_PORT0,
	MSM8660_SFAB_MAS_ADM0_PORT1,
	MSM8660_SFAB_MAS_ADM1_PORT0,
	MSM8660_SFAB_MAS_ADM1_PORT1,
	MSM8660_SFAB_MAS_LPASS_PROC,
	MSM8660_SFAB_MAS_MSS_PROCI,
	MSM8660_SFAB_MAS_MSS_PROCD,
	MSM8660_SFAB_MAS_MSS_MDM_PORT0,
	MSM8660_SFAB_MAS_LPASS,
	MSM8660_SFAB_MAS_MMSS_FPB,
	MSM8660_SFAB_MAS_ADM1_CI,
	MSM8660_SFAB_MAS_ADM0_CI,
	MSM8660_SFAB_MAS_MSS_MDM_PORT1,
	MSM8660_SFAB_MAS_USB_HS,
	MSM8660_SFAB_TO_APPSS,
	MSM8660_SFAB_TO_SYSTEM_FPB,
	MSM8660_SFAB_TO_CPSS_FPB,
	MSM8660_SFAB_SLV_SPS,
	MSM8660_SFAB_SLV_SYSTEM_IMEM,
	MSM8660_SFAB_SLV_AMPSS,
	MSM8660_SFAB_SLV_MSS,
	MSM8660_SFAB_SLV_LPASS,
	MSM8660_SFAB_SLV_MMSS_FPB,
	MSM8660_SFAB_TO_DFAB,

	/* Daytona Fabric nodes (DFAB) - peripheral bus */
	MSM8660_DFAB_MAS_SDC1,
	MSM8660_DFAB_MAS_SDC2,
	MSM8660_DFAB_MAS_SDC3,
	MSM8660_DFAB_MAS_SDC4,
	MSM8660_DFAB_MAS_SDC5,
	MSM8660_DFAB_MAS_ADM0_MASTER,
	MSM8660_DFAB_MAS_ADM1_MASTER,
	MSM8660_DFAB_TO_SFAB,
	MSM8660_DFAB_SLV_SDC1,
	MSM8660_DFAB_SLV_SDC2,
	MSM8660_DFAB_SLV_SDC3,
	MSM8660_DFAB_SLV_SDC4,
	MSM8660_DFAB_SLV_SDC5,
	MSM8660_DFAB_MAS_USB_HS,	/* USB HS DFAB voter */
	MSM8660_DFAB_MAS_DSPS,		/* DSPS DFAB voter */

	/* MMSS Fabric nodes */
	MSM8660_MMFAB_MAS_MDP_PORT0,
	MSM8660_MMFAB_MAS_MDP_PORT1,
	MSM8660_MMFAB_MAS_ADM1_PORT0,
	MSM8660_MMFAB_MAS_ROTATOR,
	MSM8660_MMFAB_MAS_GRAPHICS_3D,
	MSM8660_MMFAB_MAS_JPEG_DEC,
	MSM8660_MMFAB_MAS_GRAPHICS_2D_CORE0,
	MSM8660_MMFAB_MAS_VFE,
	MSM8660_MMFAB_MAS_VPE,
	MSM8660_MMFAB_MAS_JPEG_ENC,
	MSM8660_MMFAB_MAS_GRAPHICS_2D_CORE1,
	MSM8660_MMFAB_MAS_HD_CODEC_PORT0,
	MSM8660_MMFAB_MAS_HD_CODEC_PORT1,
	MSM8660_MMFAB_TO_APPSS,
	MSM8660_MMFAB_SLV_SMI,
	MSM8660_MMFAB_SLV_MM_IMEM,
};

#define to_msm8660_icc_provider(_provider) \
	container_of(_provider, struct msm8660_icc_provider, provider)

/*
 * Minimum fabric clock rate to prevent bus starvation.
 *
 * When no consumers request bandwidth, the rate calculation yields 0,
 * causing fabric clocks to drop to minimum. This creates bimodal
 * performance: fast when other subsystems (like display) happen to
 * request bandwidth, slow otherwise.
 *
 * 384 MHz keeps fabric fast during concurrent MDP display scanout
 * and USB gadget traffic. webOS docs: "AXI bus frequency needs to be
 * kept at maximum value while USB data transfers are happening."
 * 266 MHz was insufficient - USB crashed during display activity.
 */
#define MSM8660_FABRIC_MIN_RATE		384000000UL	/* 384 MHz */

/*
 * Maximum RPM ARB buffer size across all fabrics.
 * MM fabric is largest at 23 u32 words.
 */
#define MSM8660_MAX_RPM_BUF		23

/*
 * RPM fabric arbitration data format (from webOS msm_bus_fabric.c):
 *
 * Each u16 arb entry: bit 15 = tier (1=TIER1 high priority), bits 14-0 = BW
 * Bandwidth is in 128KB units (bytes >> 17).
 * Two u16 values are packed into each u32 RPM register word.
 *
 * Buffer layout: [bwsum pairs] [arb pairs]
 * bwsum[slave_port] = total bandwidth to that slave
 * arb[(tier-1)*nmasters + master_port] = per-master arbitration entry
 */
#define ARB_BWMASK		0x7FFF
#define ARB_TIERMASK		0x8000
#define ARB_TIER1		1
#define ARB_TIER2		2

static const struct clk_bulk_data msm8660_afab_clocks[] = {
	{ .id = "bus" },
	{ .id = "bus_a" },
	{ .id = "ebi1" },
	{ .id = "ebi1_a" },
};

static const struct clk_bulk_data msm8660_sfab_clocks[] = {
	{ .id = "bus" },
	{ .id = "bus_a" },
};

static const struct clk_bulk_data msm8660_mmfab_clocks[] = {
	{ .id = "bus" },
	{ .id = "bus_a" },
	{ .id = "smi" },
	{ .id = "smi_a" },
};

static const struct clk_bulk_data msm8660_dfab_clocks[] = {
	{ .id = "bus" },
	{ .id = "bus_a" },
};

/**
 * struct msm8660_icc_node - MSM8660 specific interconnect nodes
 * @name: the node name used in debugfs
 * @id: a unique node identifier
 * @links: an array of nodes where we can go next while traversing
 * @num_links: the total number of @links
 * @buswidth: width of the interconnect between a node and the bus (bytes)
 * @rate: current bus clock rate in Hz
 * @mas_port: master port index for RPM ARB (-1 if not a master)
 * @slv_port: slave port index for RPM bwsum (-1 if not a slave)
 * @mas_tier: master priority tier (ARB_TIER1 or ARB_TIER2, 0 if N/A)
 */
struct msm8660_icc_node {
	unsigned char *name;
	u16 id;
#define MSM8660_ICC_MAX_LINKS	3
	u16 links[MSM8660_ICC_MAX_LINKS];
	u16 num_links;
	u16 buswidth;
	s8 mas_port;
	s8 slv_port;
	u8 mas_tier;
};

/**
 * struct msm8660_icc_desc - Fabric descriptor
 * @nodes: array of node pointers
 * @num_nodes: number of nodes
 * @bus_clks: clock definitions
 * @num_clks: number of clocks
 * @rpm_resource: QCOM_RPM_*_FABRIC_ARB constant, or -1 for no ARB
 * @nmasters: number of master ports in this fabric (for ARB array sizing)
 * @nslaves: number of slave ports in this fabric (for bwsum array sizing)
 * @ntieredslaves: number of tiered slaves (ARB rows)
 * @default_tiered_slave: 1-based index of default tiered slave for masters
 * @rpm_buf_size: number of u32 words for RPM write
 * @bus_width: representative fabric bus width in bytes, used as the
 *             divisor for translating aggregate bytes/sec into a single
 *             clock rate that drives the whole fabric
 */
struct msm8660_icc_desc {
	struct msm8660_icc_node * const *nodes;
	size_t num_nodes;
	const struct clk_bulk_data *bus_clks;
	size_t num_clks;
	int rpm_resource;
	u8 nmasters;
	u8 nslaves;
	u8 ntieredslaves;
	u8 default_tiered_slave;
	u8 rpm_buf_size;
	u8 bus_width;
};

/**
 * struct msm8660_icc_provider - MSM8660 specific interconnect provider
 * @provider: generic interconnect provider
 * @bus_clks: the clk_bulk_data table of bus clocks
 * @num_clks: the total number of clk_bulk_data entries
 * @rpm: RPM handle for fabric arbitration writes
 * @desc: fabric descriptor with RPM metadata
 * @arb: pre-allocated arbitration array (nmasters * ntieredslaves u16 entries)
 * @bwsum: pre-allocated bandwidth sum array (nslaves u16 entries)
 * @rpm_buf: pre-allocated RPM write buffer (rpm_buf_size u32 entries)
 * @rate: last clock rate applied to the fabric bus_clks, used as the
 *        single source of truth for whether the rate actually needs to
 *        be reprogrammed (per-node caching would desync when different
 *        masters update at different times)
 */
struct msm8660_icc_provider {
	struct icc_provider provider;
	struct clk_bulk_data *bus_clks;
	int num_clks;
	struct qcom_rpm *rpm;
	const struct msm8660_icc_desc *desc;
	u16 *arb;
	u16 *bwsum;
	u32 *rpm_buf;
	u32 rate;
};

/*
 * Node definitions with RPM port mapping.
 *
 * DEFINE_QNODE(_name, _id, _buswidth, _mas_port, _slv_port, _tier, links...)
 *
 * _mas_port: master port index for RPM ARB array (-1 if not a master)
 * _slv_port: slave port index for RPM bwsum array (-1 if not a slave)
 * _tier: master priority tier (ARB_TIER1=1, ARB_TIER2=2, 0 if N/A)
 */
#define DEFINE_QNODE(_name, _id, _buswidth, _mas, _slv, _tier, ...)	\
	static struct msm8660_icc_node _name = {			\
		.name = #_name,						\
		.id = _id,						\
		.buswidth = _buswidth,					\
		.num_links = COUNT_ARGS(__VA_ARGS__),			\
		.links = { __VA_ARGS__ },				\
		.mas_port = _mas,					\
		.slv_port = _slv,					\
		.mas_tier = _tier,					\
	}

/*
 * =========================================================================
 * APPSS Fabric nodes
 *
 * 4 masters, 4 slaves, 2 tiered slaves
 * Master ports: SMPSS_M0=0, SMPSS_M1=1, FAB_MMSS=2, FAB_SYSTEM=3
 * Slave ports:  EBI_CH0=0, SMPSS_L2=1, MMSS_FAB=2, SYSTEM_FAB=3
 * Default target: tiered slave 1 (EBI_CH0)
 * =========================================================================
 */
DEFINE_QNODE(mas_ampss_m0, MSM8660_AFAB_MAS_AMPSS_M0, 8, 0, -1, ARB_TIER2,
	     MSM8660_AFAB_SLV_EBI_CH0, MSM8660_AFAB_TO_MMSS, MSM8660_AFAB_TO_SYSTEM);
DEFINE_QNODE(mas_ampss_m1, MSM8660_AFAB_MAS_AMPSS_M1, 8, 1, -1, ARB_TIER2,
	     MSM8660_AFAB_SLV_EBI_CH0, MSM8660_AFAB_TO_MMSS, MSM8660_AFAB_TO_SYSTEM);
DEFINE_QNODE(slv_ebi_ch0, MSM8660_AFAB_SLV_EBI_CH0, 8, -1, 0, 0);
DEFINE_QNODE(slv_ampss_l2, MSM8660_AFAB_SLV_AMPSS_L2, 8, -1, 1, 0);
/*
 * Gateway nodes need links to both the cross-fabric gateway AND the memory
 * slave to enable cross-fabric paths. Without link to EBI_CH0, path_find()
 * can't route from MMSS/System fabric masters to main memory.
 *
 * AFAB_TO_MMSS doubles as AFAB master port 2 (the FAB_MMSS master). MDP
 * scanout and GPU traffic enter AFAB through this gateway. Mark it
 * ARB_TIER1 so display/multimedia traffic keeps priority over CPU L2
 * misses inside the APPSS fabric — without this, MDP TIER1 priority
 * earned in MMFAB is dropped at the AFAB boundary and MDP fetches lose
 * arbitration to CPU traffic, producing PRIMARY_INTF_UDERRUN.
 */
DEFINE_QNODE(afab_to_mmss, MSM8660_AFAB_TO_MMSS, 8, 2, 2, ARB_TIER1,
	     MSM8660_MMFAB_TO_APPSS, MSM8660_AFAB_SLV_EBI_CH0);
DEFINE_QNODE(afab_to_system, MSM8660_AFAB_TO_SYSTEM, 8, 3, 3, ARB_TIER2,
	     MSM8660_SFAB_TO_APPSS, MSM8660_AFAB_SLV_EBI_CH0);

static struct msm8660_icc_node * const msm8660_afab_nodes[] = {
	[AFAB_MAS_AMPSS_M0] = &mas_ampss_m0,
	[AFAB_MAS_AMPSS_M1] = &mas_ampss_m1,
	[AFAB_SLV_EBI_CH0] = &slv_ebi_ch0,
	[AFAB_SLV_AMPSS_L2] = &slv_ampss_l2,
	[AFAB_TO_MMSS] = &afab_to_mmss,
	[AFAB_TO_SYSTEM] = &afab_to_system,
};

static const struct msm8660_icc_desc msm8660_afab = {
	.nodes = msm8660_afab_nodes,
	.num_nodes = ARRAY_SIZE(msm8660_afab_nodes),
	.bus_clks = msm8660_afab_clocks,
	.num_clks = ARRAY_SIZE(msm8660_afab_clocks),
	.rpm_resource = QCOM_RPM_APPS_FABRIC_ARB,
	.nmasters = 4,
	.nslaves = 4,
	.ntieredslaves = 2,
	.default_tiered_slave = 1,	/* EBI_CH0 */
	.rpm_buf_size = 6,
	.bus_width = 8,			/* 64-bit APPSS fabric datapath */
};

/*
 * =========================================================================
 * System Fabric nodes
 *
 * 17 masters, 9 slaves, 2 tiered slaves
 * Master ports: see enum msm_bus_8660_master_ports_type in webOS
 * Slave ports:  APPSS_FAB=0, SPS=1, SYSTEM_IMEM=2, SMPSS=3, MSS=4,
 *               LPASS=5, CPSS_FPB=6, SYSTEM_FPB=7, MMSS_FPB=8
 * Default target: tiered slave 1 (APPSS gateway)
 * =========================================================================
 */
DEFINE_QNODE(sfab_mas_appss, MSM8660_SFAB_MAS_APPSS, 8, 0, -1, ARB_TIER2,
	     MSM8660_AFAB_TO_SYSTEM);
DEFINE_QNODE(sfab_mas_sps, MSM8660_SFAB_MAS_SPS, 8, 1, -1, ARB_TIER2,
	     MSM8660_SFAB_SLV_SPS);
/*
 * ADM DMA masters - route through SFAB_TO_APPSS to reach EBI memory.
 * Path: ADM -> SFAB_TO_APPSS -> AFAB_TO_SYSTEM -> AFAB_SLV_EBI_CH0
 * This enables proper EBI bandwidth voting for DMA operations.
 */
DEFINE_QNODE(sfab_mas_adm0_port0, MSM8660_SFAB_MAS_ADM0_PORT0, 8, 2, -1, ARB_TIER2,
	     MSM8660_SFAB_TO_APPSS);
DEFINE_QNODE(sfab_mas_adm0_port1, MSM8660_SFAB_MAS_ADM0_PORT1, 8, 3, -1, ARB_TIER2,
	     MSM8660_SFAB_TO_APPSS);
DEFINE_QNODE(sfab_mas_adm1_port0, MSM8660_SFAB_MAS_ADM1_PORT0, 8, 4, -1, ARB_TIER2,
	     MSM8660_SFAB_TO_APPSS);
DEFINE_QNODE(sfab_mas_adm1_port1, MSM8660_SFAB_MAS_ADM1_PORT1, 8, 5, -1, ARB_TIER2,
	     MSM8660_SFAB_TO_APPSS);
DEFINE_QNODE(sfab_mas_lpass_proc, MSM8660_SFAB_MAS_LPASS_PROC, 8, 6, -1, ARB_TIER2);
DEFINE_QNODE(sfab_mas_mss_proci, MSM8660_SFAB_MAS_MSS_PROCI, 8, 7, -1, ARB_TIER2);
DEFINE_QNODE(sfab_mas_mss_procd, MSM8660_SFAB_MAS_MSS_PROCD, 8, 8, -1, ARB_TIER2);
DEFINE_QNODE(sfab_mas_mss_mdm_port0, MSM8660_SFAB_MAS_MSS_MDM_PORT0, 8, 9, -1, ARB_TIER2);
DEFINE_QNODE(sfab_mas_lpass, MSM8660_SFAB_MAS_LPASS, 8, 10, -1, ARB_TIER2);
DEFINE_QNODE(sfab_mas_mmss_fpb, MSM8660_SFAB_MAS_MMSS_FPB, 8, 13, -1, ARB_TIER2);
DEFINE_QNODE(sfab_mas_adm1_ci, MSM8660_SFAB_MAS_ADM1_CI, 8, 14, -1, ARB_TIER2);
DEFINE_QNODE(sfab_mas_adm0_ci, MSM8660_SFAB_MAS_ADM0_CI, 8, 15, -1, ARB_TIER2);
DEFINE_QNODE(sfab_mas_mss_mdm_port1, MSM8660_SFAB_MAS_MSS_MDM_PORT1, 8, 16, -1, ARB_TIER2);
/* USB HS has no dedicated master port in webOS SFAB - bandwidth voting only */
DEFINE_QNODE(sfab_mas_usb_hs, MSM8660_SFAB_MAS_USB_HS, 8, -1, -1, 0,
	     MSM8660_SFAB_TO_APPSS);
DEFINE_QNODE(sfab_to_appss, MSM8660_SFAB_TO_APPSS, 8, -1, 0, 0,
	     MSM8660_AFAB_TO_SYSTEM);
DEFINE_QNODE(sfab_to_system_fpb, MSM8660_SFAB_TO_SYSTEM_FPB, 4, -1, 7, 0);
DEFINE_QNODE(sfab_to_cpss_fpb, MSM8660_SFAB_TO_CPSS_FPB, 4, -1, 6, 0);
DEFINE_QNODE(sfab_slv_sps, MSM8660_SFAB_SLV_SPS, 8, -1, 1, 0);
DEFINE_QNODE(sfab_slv_system_imem, MSM8660_SFAB_SLV_SYSTEM_IMEM, 8, -1, 2, 0);
DEFINE_QNODE(sfab_slv_ampss, MSM8660_SFAB_SLV_AMPSS, 8, -1, 3, 0);
DEFINE_QNODE(sfab_slv_mss, MSM8660_SFAB_SLV_MSS, 8, -1, 4, 0);
DEFINE_QNODE(sfab_slv_lpass, MSM8660_SFAB_SLV_LPASS, 8, -1, 5, 0);
DEFINE_QNODE(sfab_slv_mmss_fpb, MSM8660_SFAB_SLV_MMSS_FPB, 8, -1, 8, 0);
/*
 * Gateway to DFAB: links to DFAB_TO_SFAB for path traversal.
 * Also links to SFAB_TO_APPSS to enable DFAB->SFAB->AFAB->memory paths.
 * No slave port in webOS SFAB config (DFAB is separate fabric).
 */
DEFINE_QNODE(sfab_to_dfab, MSM8660_SFAB_TO_DFAB, 8, -1, -1, 0,
	     MSM8660_DFAB_TO_SFAB, MSM8660_SFAB_TO_APPSS);

static struct msm8660_icc_node * const msm8660_sfab_nodes[] = {
	[SFAB_MAS_APPSS] = &sfab_mas_appss,
	[SFAB_MAS_SPS] = &sfab_mas_sps,
	[SFAB_MAS_ADM0_PORT0] = &sfab_mas_adm0_port0,
	[SFAB_MAS_ADM0_PORT1] = &sfab_mas_adm0_port1,
	[SFAB_MAS_ADM1_PORT0] = &sfab_mas_adm1_port0,
	[SFAB_MAS_ADM1_PORT1] = &sfab_mas_adm1_port1,
	[SFAB_MAS_LPASS_PROC] = &sfab_mas_lpass_proc,
	[SFAB_MAS_MSS_PROCI] = &sfab_mas_mss_proci,
	[SFAB_MAS_MSS_PROCD] = &sfab_mas_mss_procd,
	[SFAB_MAS_MSS_MDM_PORT0] = &sfab_mas_mss_mdm_port0,
	[SFAB_MAS_LPASS] = &sfab_mas_lpass,
	[SFAB_MAS_MMSS_FPB] = &sfab_mas_mmss_fpb,
	[SFAB_MAS_ADM1_CI] = &sfab_mas_adm1_ci,
	[SFAB_MAS_ADM0_CI] = &sfab_mas_adm0_ci,
	[SFAB_MAS_MSS_MDM_PORT1] = &sfab_mas_mss_mdm_port1,
	[SFAB_MAS_USB_HS] = &sfab_mas_usb_hs,
	[SFAB_TO_APPSS] = &sfab_to_appss,
	[SFAB_TO_SYSTEM_FPB] = &sfab_to_system_fpb,
	[SFAB_TO_CPSS_FPB] = &sfab_to_cpss_fpb,
	[SFAB_SLV_SPS] = &sfab_slv_sps,
	[SFAB_SLV_SYSTEM_IMEM] = &sfab_slv_system_imem,
	[SFAB_SLV_AMPSS] = &sfab_slv_ampss,
	[SFAB_SLV_MSS] = &sfab_slv_mss,
	[SFAB_SLV_LPASS] = &sfab_slv_lpass,
	[SFAB_SLV_MMSS_FPB] = &sfab_slv_mmss_fpb,
	[SFAB_TO_DFAB] = &sfab_to_dfab,
};

static const struct msm8660_icc_desc msm8660_sfab = {
	.nodes = msm8660_sfab_nodes,
	.num_nodes = ARRAY_SIZE(msm8660_sfab_nodes),
	.bus_clks = msm8660_sfab_clocks,
	.num_clks = ARRAY_SIZE(msm8660_sfab_clocks),
	.rpm_resource = QCOM_RPM_SYS_FABRIC_ARB,
	.nmasters = 17,
	.nslaves = 9,
	.ntieredslaves = 2,
	.default_tiered_slave = 1,	/* APPSS gateway */
	.rpm_buf_size = 22,
	.bus_width = 8,			/* 64-bit System fabric datapath */
};

/*
 * =========================================================================
 * MMSS Fabric nodes - Multimedia subsystem (MDP, camera, video, GPU)
 *
 * 14 masters, 4 slaves, 3 tiered slaves
 * Master ports: MDP0=0, MDP1=1, ADM1=2, ROT=3, 3D=4, JPEG_DEC=5,
 *               2D_CORE0=6, VFE=7, VPE=8, JPEG_ENC=9, 2D_CORE1=10,
 *               (APPS_FAB=11), HD_CODEC0=12, HD_CODEC1=13
 * Slave ports:  SMI=0, APPSS_FAB=1, (APPSS_FAB_1=2), MM_IMEM=3
 * Tiered slaves: SMI=1, APPSS_FAB=2, MM_IMEM=3
 * Default target: tiered slave 2 (APPSS gateway -> main memory)
 *
 * MDP ports get TIER1 (high priority) for guaranteed display refresh.
 * All other masters get TIER2 (default priority).
 * =========================================================================
 */
DEFINE_QNODE(mmfab_mas_mdp_port0, MSM8660_MMFAB_MAS_MDP_PORT0, 16, 0, -1, ARB_TIER1,
	     MSM8660_MMFAB_SLV_SMI, MSM8660_MMFAB_TO_APPSS);
DEFINE_QNODE(mmfab_mas_mdp_port1, MSM8660_MMFAB_MAS_MDP_PORT1, 16, 1, -1, ARB_TIER1,
	     MSM8660_MMFAB_SLV_SMI, MSM8660_MMFAB_TO_APPSS);
DEFINE_QNODE(mmfab_mas_adm1_port0, MSM8660_MMFAB_MAS_ADM1_PORT0, 8, 2, -1, ARB_TIER2);
DEFINE_QNODE(mmfab_mas_rotator, MSM8660_MMFAB_MAS_ROTATOR, 16, 3, -1, ARB_TIER2,
	     MSM8660_MMFAB_SLV_SMI, MSM8660_MMFAB_TO_APPSS);
DEFINE_QNODE(mmfab_mas_graphics_3d, MSM8660_MMFAB_MAS_GRAPHICS_3D, 16, 4, -1, ARB_TIER2,
	     MSM8660_MMFAB_SLV_SMI, MSM8660_MMFAB_TO_APPSS);
DEFINE_QNODE(mmfab_mas_jpeg_dec, MSM8660_MMFAB_MAS_JPEG_DEC, 16, 5, -1, ARB_TIER2,
	     MSM8660_MMFAB_SLV_SMI, MSM8660_MMFAB_TO_APPSS);
DEFINE_QNODE(mmfab_mas_graphics_2d_core0, MSM8660_MMFAB_MAS_GRAPHICS_2D_CORE0, 16,
	     6, -1, ARB_TIER2,
	     MSM8660_MMFAB_SLV_SMI, MSM8660_MMFAB_TO_APPSS);
DEFINE_QNODE(mmfab_mas_vfe, MSM8660_MMFAB_MAS_VFE, 16, 7, -1, ARB_TIER2,
	     MSM8660_MMFAB_SLV_SMI, MSM8660_MMFAB_TO_APPSS);
DEFINE_QNODE(mmfab_mas_vpe, MSM8660_MMFAB_MAS_VPE, 16, 8, -1, ARB_TIER2,
	     MSM8660_MMFAB_SLV_SMI, MSM8660_MMFAB_TO_APPSS);
DEFINE_QNODE(mmfab_mas_jpeg_enc, MSM8660_MMFAB_MAS_JPEG_ENC, 16, 9, -1, ARB_TIER2,
	     MSM8660_MMFAB_SLV_SMI, MSM8660_MMFAB_TO_APPSS);
DEFINE_QNODE(mmfab_mas_graphics_2d_core1, MSM8660_MMFAB_MAS_GRAPHICS_2D_CORE1, 16,
	     10, -1, ARB_TIER2,
	     MSM8660_MMFAB_SLV_SMI, MSM8660_MMFAB_TO_APPSS);
DEFINE_QNODE(mmfab_mas_hd_codec_port0, MSM8660_MMFAB_MAS_HD_CODEC_PORT0, 16,
	     12, -1, ARB_TIER2,
	     MSM8660_MMFAB_SLV_SMI, MSM8660_MMFAB_TO_APPSS);
DEFINE_QNODE(mmfab_mas_hd_codec_port1, MSM8660_MMFAB_MAS_HD_CODEC_PORT1, 16,
	     13, -1, ARB_TIER2,
	     MSM8660_MMFAB_SLV_SMI, MSM8660_MMFAB_TO_APPSS);
/*
 * Gateway from APPSS into MMSS: slave (port 1) for outbound traffic
 * leaving MMSS, AND master (port 2) for inbound traffic arriving from
 * APPSS (e.g. CPU memremap_wc accesses to SMI BOs). Without the master
 * port and the forward links into MMFAB slaves (SMI / MM_IMEM), the
 * ICC path-finder has no route from AMPSS_M0 to MMFAB_SLV_SMI; the
 * cross-fabric gateway only worked for outbound traffic (MMSS masters
 * reaching APPSS slaves like EBI).
 *
 * ARB_TIER1 keeps AMPSS->SMI traffic high-priority within MMFAB so
 * CPU mmap reads/writes to SMI BOs don't get starved by MDP scanout.
 */
DEFINE_QNODE(mmfab_to_appss, MSM8660_MMFAB_TO_APPSS, 8, 11, 1, ARB_TIER1,
	     MSM8660_AFAB_TO_MMSS,
	     MSM8660_MMFAB_SLV_SMI,
	     MSM8660_MMFAB_SLV_MM_IMEM);
DEFINE_QNODE(mmfab_slv_smi, MSM8660_MMFAB_SLV_SMI, 16, -1, 0, 0);
DEFINE_QNODE(mmfab_slv_mm_imem, MSM8660_MMFAB_SLV_MM_IMEM, 8, -1, 3, 0);

static struct msm8660_icc_node * const msm8660_mmfab_nodes[] = {
	[MMFAB_MAS_MDP_PORT0] = &mmfab_mas_mdp_port0,
	[MMFAB_MAS_MDP_PORT1] = &mmfab_mas_mdp_port1,
	[MMFAB_MAS_ADM1_PORT0] = &mmfab_mas_adm1_port0,
	[MMFAB_MAS_ROTATOR] = &mmfab_mas_rotator,
	[MMFAB_MAS_GRAPHICS_3D] = &mmfab_mas_graphics_3d,
	[MMFAB_MAS_JPEG_DEC] = &mmfab_mas_jpeg_dec,
	[MMFAB_MAS_GRAPHICS_2D_CORE0] = &mmfab_mas_graphics_2d_core0,
	[MMFAB_MAS_VFE] = &mmfab_mas_vfe,
	[MMFAB_MAS_VPE] = &mmfab_mas_vpe,
	[MMFAB_MAS_JPEG_ENC] = &mmfab_mas_jpeg_enc,
	[MMFAB_MAS_GRAPHICS_2D_CORE1] = &mmfab_mas_graphics_2d_core1,
	[MMFAB_MAS_HD_CODEC_PORT0] = &mmfab_mas_hd_codec_port0,
	[MMFAB_MAS_HD_CODEC_PORT1] = &mmfab_mas_hd_codec_port1,
	[MMFAB_TO_APPSS] = &mmfab_to_appss,
	[MMFAB_SLV_SMI] = &mmfab_slv_smi,
	[MMFAB_SLV_MM_IMEM] = &mmfab_slv_mm_imem,
};

static const struct msm8660_icc_desc msm8660_mmfab = {
	.nodes = msm8660_mmfab_nodes,
	.num_nodes = ARRAY_SIZE(msm8660_mmfab_nodes),
	.bus_clks = msm8660_mmfab_clocks,
	.num_clks = ARRAY_SIZE(msm8660_mmfab_clocks),
	.rpm_resource = QCOM_RPM_MM_FABRIC_ARB,
	.nmasters = 14,
	.nslaves = 4,
	.ntieredslaves = 3,
	.default_tiered_slave = 2,	/* APPSS gateway */
	.rpm_buf_size = 23,
	.bus_width = 16,		/* 128-bit Multimedia fabric datapath */
};

/*
 * =========================================================================
 * Daytona Fabric (DFAB) nodes - peripheral bus for SDCC and ADM DMA
 *
 * DFAB connects slower peripherals to SFAB via the DFAB_TO_SFAB gateway.
 * SDCC controllers (eMMC, SD card) connect here.
 *
 * No RPM ARB for DFAB - it's a simple peripheral bus with clock-only control.
 *
 * USB HS is included as a DFAB voter for compatibility with the legacy
 * webOS kernel clock voting mechanism.
 * =========================================================================
 */
DEFINE_QNODE(dfab_mas_sdc1, MSM8660_DFAB_MAS_SDC1, 8, -1, -1, 0,
	     MSM8660_DFAB_TO_SFAB);
DEFINE_QNODE(dfab_mas_sdc2, MSM8660_DFAB_MAS_SDC2, 8, -1, -1, 0,
	     MSM8660_DFAB_TO_SFAB);
DEFINE_QNODE(dfab_mas_sdc3, MSM8660_DFAB_MAS_SDC3, 8, -1, -1, 0,
	     MSM8660_DFAB_TO_SFAB);
DEFINE_QNODE(dfab_mas_sdc4, MSM8660_DFAB_MAS_SDC4, 8, -1, -1, 0,
	     MSM8660_DFAB_TO_SFAB);
DEFINE_QNODE(dfab_mas_sdc5, MSM8660_DFAB_MAS_SDC5, 8, -1, -1, 0,
	     MSM8660_DFAB_TO_SFAB);
DEFINE_QNODE(dfab_mas_adm0_master, MSM8660_DFAB_MAS_ADM0_MASTER, 8, -1, -1, 0,
	     MSM8660_DFAB_TO_SFAB);
DEFINE_QNODE(dfab_mas_adm1_master, MSM8660_DFAB_MAS_ADM1_MASTER, 8, -1, -1, 0,
	     MSM8660_DFAB_TO_SFAB);
DEFINE_QNODE(dfab_to_sfab, MSM8660_DFAB_TO_SFAB, 8, -1, -1, 0,
	     MSM8660_SFAB_TO_DFAB);
DEFINE_QNODE(dfab_slv_sdc1, MSM8660_DFAB_SLV_SDC1, 8, -1, -1, 0);
DEFINE_QNODE(dfab_slv_sdc2, MSM8660_DFAB_SLV_SDC2, 8, -1, -1, 0);
DEFINE_QNODE(dfab_slv_sdc3, MSM8660_DFAB_SLV_SDC3, 8, -1, -1, 0);
DEFINE_QNODE(dfab_slv_sdc4, MSM8660_DFAB_SLV_SDC4, 8, -1, -1, 0);
DEFINE_QNODE(dfab_slv_sdc5, MSM8660_DFAB_SLV_SDC5, 8, -1, -1, 0);
/* USB HS DFAB voter - keeps DFAB clock stable during USB activity */
DEFINE_QNODE(dfab_mas_usb_hs, MSM8660_DFAB_MAS_USB_HS, 8, -1, -1, 0,
	     MSM8660_DFAB_TO_SFAB);
/* DSPS DFAB voter - keeps DFAB clock stable during sensor activity */
DEFINE_QNODE(dfab_mas_dsps, MSM8660_DFAB_MAS_DSPS, 8, -1, -1, 0,
	     MSM8660_DFAB_TO_SFAB);

static struct msm8660_icc_node * const msm8660_dfab_nodes[] = {
	[DFAB_MAS_SDC1] = &dfab_mas_sdc1,
	[DFAB_MAS_SDC2] = &dfab_mas_sdc2,
	[DFAB_MAS_SDC3] = &dfab_mas_sdc3,
	[DFAB_MAS_SDC4] = &dfab_mas_sdc4,
	[DFAB_MAS_SDC5] = &dfab_mas_sdc5,
	[DFAB_MAS_ADM0_MASTER] = &dfab_mas_adm0_master,
	[DFAB_MAS_ADM1_MASTER] = &dfab_mas_adm1_master,
	[DFAB_TO_SFAB] = &dfab_to_sfab,
	[DFAB_SLV_SDC1] = &dfab_slv_sdc1,
	[DFAB_SLV_SDC2] = &dfab_slv_sdc2,
	[DFAB_SLV_SDC3] = &dfab_slv_sdc3,
	[DFAB_SLV_SDC4] = &dfab_slv_sdc4,
	[DFAB_SLV_SDC5] = &dfab_slv_sdc5,
	[DFAB_MAS_USB_HS] = &dfab_mas_usb_hs,
	[DFAB_MAS_DSPS] = &dfab_mas_dsps,
};

static const struct msm8660_icc_desc msm8660_dfab = {
	.nodes = msm8660_dfab_nodes,
	.num_nodes = ARRAY_SIZE(msm8660_dfab_nodes),
	.bus_clks = msm8660_dfab_clocks,
	.num_clks = ARRAY_SIZE(msm8660_dfab_clocks),
	.rpm_resource = -1,		/* No RPM ARB for DFAB */
	.bus_width = 8,			/* 64-bit Daytona fabric datapath */
};

/*
 * Pack bwsum[] and arb[] arrays into the u32 RPM buffer.
 *
 * Two u16 values are packed per u32 word: lower 16 bits first, upper 16 next.
 * Layout: [bwsum pairs] then [arb pairs], handling odd boundaries.
 *
 * This matches the webOS msm_bus_fabric_rpm_commit() packing algorithm.
 */
static void msm8660_pack_rpm_data(const u16 *bwsum, int nslaves,
				  const u16 *arb, int arb_size,
				  u32 *buf)
{
	int i, index = 0;

	/* Pack bwsum pairs */
	for (i = 0; i + 1 < nslaves; i += 2) {
		buf[index] = ((u32)bwsum[i + 1] << 16) | bwsum[i];
		index++;
	}

	/* Handle boundary between bwsum and arb for odd nslaves */
	if (nslaves & 1) {
		buf[index] = ((u32)arb[0] << 16) | bwsum[i];
		index++;
		i = 1;	/* Start arb from index 1 (index 0 already packed) */
	} else {
		i = 0;
	}

	/* Pack arb pairs */
	for (; i + 1 < arb_size; i += 2) {
		buf[index] = ((u32)arb[i + 1] << 16) | arb[i];
		index++;
	}

	/* Handle odd arb entry at end */
	if (i < arb_size) {
		buf[index] = arb[i];
		index++;
	}
}

/*
 * Send fabric arbitration data to RPM.
 *
 * Iterates over all ICC nodes in the provider, builds bwsum/arb arrays
 * from their aggregated bandwidth, and sends the packed data to RPM.
 */
static void msm8660_rpm_commit(struct msm8660_icc_provider *qp)
{
	const struct msm8660_icc_desc *desc = qp->desc;
	struct icc_provider *provider = &qp->provider;
	int nm = desc->nmasters;
	int ns = desc->nslaves;
	int nts = desc->ntieredslaves;
	int arb_size = nm * nts;
	int def_ts = desc->default_tiered_slave;
	struct icc_node *n;
	int ret;

	memset(qp->bwsum, 0, ns * sizeof(u16));
	memset(qp->arb, 0, arb_size * sizeof(u16));
	memset(qp->rpm_buf, 0, desc->rpm_buf_size * sizeof(u32));

	list_for_each_entry(n, &provider->nodes, node_list) {
		struct msm8660_icc_node *qn = n->data;
		u64 bw_bytes;
		u16 bw_128k;

		/* Use max of avg and peak bandwidth, convert to 128KB units */
		bw_bytes = max(icc_units_to_bps(n->avg_bw),
			       icc_units_to_bps(n->peak_bw));
		bw_128k = (u16)min_t(u64, bw_bytes >> 17, ARB_BWMASK);

		/* Set arb entry for masters at their default tiered slave */
		if (qn->mas_port >= 0 && qn->mas_port < nm && def_ts > 0) {
			int idx = (def_ts - 1) * nm + qn->mas_port;
			u8 tier;

			if (idx < arb_size) {
				tier = bw_128k ? qn->mas_tier : ARB_TIER2;
				qp->arb[idx] = (tier == ARB_TIER1 ? ARB_TIERMASK : 0)
					       | (bw_128k & ARB_BWMASK);
			}
		}

		/* Set bwsum for slaves */
		if (qn->slv_port >= 0 && qn->slv_port < ns)
			qp->bwsum[qn->slv_port] = bw_128k;
	}

	msm8660_pack_rpm_data(qp->bwsum, ns, qp->arb, arb_size, qp->rpm_buf);

	ret = qcom_rpm_write(qp->rpm, QCOM_RPM_ACTIVE_STATE,
			     desc->rpm_resource, qp->rpm_buf,
			     desc->rpm_buf_size);
	if (ret)
		dev_err_ratelimited(provider->dev,
				    "RPM fabric ARB write failed: %d\n", ret);
}

static int msm8660_icc_set(struct icc_node *src, struct icc_node *dst)
{
	struct msm8660_icc_node *src_qn;
	struct msm8660_icc_provider *qp;
	u64 sum_bw, max_peak_bw, rate;
	u32 agg_avg = 0, agg_peak = 0;
	struct icc_provider *provider;
	struct icc_node *n;
	int ret, i;

	src_qn = src->data;
	provider = src->provider;
	qp = to_msm8660_icc_provider(provider);

	list_for_each_entry(n, &provider->nodes, node_list)
		provider->aggregate(n, 0, n->avg_bw, n->peak_bw,
				    &agg_avg, &agg_peak);

	sum_bw = icc_units_to_bps(agg_avg);
	max_peak_bw = icc_units_to_bps(agg_peak);

	/*
	 * Divide by the *fabric* bus width, not src_qn->buswidth: every
	 * master on a given fabric shares the same hardware clock, so the
	 * required clock rate is a single function of total bandwidth and
	 * the fabric's bus width. Picking the bus width of whichever node
	 * happened to trigger this update would make the rate oscillate
	 * depending on which master called icc_set_bw() last.
	 */
	rate = max(sum_bw, max_peak_bw);
	do_div(rate, qp->desc->bus_width);
	/* Apply minimum floor to prevent bus starvation */
	rate = max_t(u64, rate, MSM8660_FABRIC_MIN_RATE);
	rate = min_t(u32, rate, INT_MAX);

	if (qp->rate != rate) {
		for (i = 0; i < qp->num_clks; i++) {
			ret = clk_set_rate(qp->bus_clks[i].clk, rate);
			if (ret) {
				dev_err(provider->dev,
					"%s clk_set_rate error: %d\n",
					qp->bus_clks[i].id, ret);
				ret = 0;
			}
		}
		qp->rate = rate;
	}

	/* Send RPM fabric arbitration if available */
	if (qp->rpm && qp->desc->rpm_resource >= 0)
		msm8660_rpm_commit(qp);

	return 0;
}

static int msm8660_get_bw(struct icc_node *node, u32 *avg, u32 *peak)
{
	*avg = 0;
	*peak = 0;
	return 0;
}

/*
 * Look up the RPM that owns fabric arbitration writes.
 *
 * Returns NULL if the DT does not have a "qcom,rpm" phandle (in which
 * case the caller silently drops RPM ARB and runs the fabric purely
 * via clk_set_rate).
 *
 * Returns ERR_PTR(-EPROBE_DEFER) if the RPM device exists in DT but
 * its driver has not finished probing yet, or if device_link_add()
 * fails. The caller is expected to propagate this so the interconnect
 * driver gets retried once the RPM is ready.
 *
 * On success returns the qcom_rpm handle and pins the RPM device
 * lifetime to ours via a consumer-supplier device link, so the
 * devres-allocated qcom_rpm cannot be freed while we still hold a
 * pointer to it.
 */
static struct qcom_rpm *msm8660_get_rpm(struct device *dev)
{
	struct device_node *rpm_np;
	struct platform_device *rpm_pdev;
	struct device_link *link;
	struct qcom_rpm *rpm;

	rpm_np = of_parse_phandle(dev->of_node, "qcom,rpm", 0);
	if (!rpm_np) {
		dev_dbg(dev, "no qcom,rpm phandle, RPM ARB disabled\n");
		return NULL;
	}

	rpm_pdev = of_find_device_by_node(rpm_np);
	of_node_put(rpm_np);
	if (!rpm_pdev) {
		dev_dbg(dev, "RPM device not found yet, deferring probe\n");
		return ERR_PTR(-EPROBE_DEFER);
	}

	rpm = dev_get_drvdata(&rpm_pdev->dev);
	if (!rpm) {
		put_device(&rpm_pdev->dev);
		dev_dbg(dev, "RPM not ready, deferring probe\n");
		return ERR_PTR(-EPROBE_DEFER);
	}

	/*
	 * Keep the RPM device alive for as long as we are bound; without
	 * a device link, the devres-managed qcom_rpm could be released
	 * out from under us if the RPM driver were unbound at runtime,
	 * leaving a dangling pointer in qp->rpm.
	 */
	link = device_link_add(dev, &rpm_pdev->dev,
			       DL_FLAG_AUTOREMOVE_CONSUMER);
	put_device(&rpm_pdev->dev);
	if (!link) {
		dev_warn(dev, "failed to add device link to RPM, deferring\n");
		return ERR_PTR(-EPROBE_DEFER);
	}

	return rpm;
}

static int msm8660_icc_probe(struct platform_device *pdev)
{
	const struct msm8660_icc_desc *desc;
	struct msm8660_icc_node * const *qnodes;
	struct msm8660_icc_provider *qp;
	struct device *dev = &pdev->dev;
	struct icc_onecell_data *data;
	struct icc_provider *provider;
	struct icc_node *node;
	size_t num_nodes, i;
	int ret;

	desc = of_device_get_match_data(dev);
	if (!desc)
		return -EINVAL;

	qnodes = desc->nodes;
	num_nodes = desc->num_nodes;

	qp = devm_kzalloc(dev, sizeof(*qp), GFP_KERNEL);
	if (!qp)
		return -ENOMEM;

	data = devm_kzalloc(dev, struct_size(data, nodes, num_nodes),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->num_nodes = num_nodes;

	qp->bus_clks = devm_kmemdup(dev, desc->bus_clks,
				    desc->num_clks * sizeof(*desc->bus_clks),
				    GFP_KERNEL);
	if (!qp->bus_clks)
		return -ENOMEM;

	qp->num_clks = desc->num_clks;

	/*
	 * MSM8660 fabric clocks are managed by RPM firmware and may not be
	 * available in mainline Linux yet. Once the clock provider exists,
	 * we want to honour it; until then we run without per-fabric clock
	 * scaling. The crucial part is that -EPROBE_DEFER means "the
	 * provider exists but hasn't probed yet" and MUST be propagated so
	 * we get retried; only other errors (genuine -ENOENT, etc.) get
	 * downgraded to "no clocks, continue".
	 */
	ret = devm_clk_bulk_get_optional(dev, qp->num_clks, qp->bus_clks);
	if (ret == -EPROBE_DEFER)
		return ret;
	if (ret) {
		dev_warn(dev, "Failed to get bus clocks: %d (continuing without clock scaling)\n",
			 ret);
		qp->num_clks = 0;
	}

	if (qp->num_clks) {
		ret = clk_bulk_prepare_enable(qp->num_clks, qp->bus_clks);
		if (ret) {
			dev_warn(dev, "Failed to enable bus clocks: %d\n", ret);
			qp->num_clks = 0;
		}
	}

	/* Set up RPM fabric arbitration */
	qp->desc = desc;
	if (desc->rpm_resource >= 0) {
		qp->rpm = msm8660_get_rpm(dev);
		if (IS_ERR(qp->rpm))
			return PTR_ERR(qp->rpm);
		if (qp->rpm) {
			int arb_size = desc->nmasters * desc->ntieredslaves;

			qp->bwsum = devm_kcalloc(dev, desc->nslaves,
						 sizeof(u16), GFP_KERNEL);
			qp->arb = devm_kcalloc(dev, arb_size + 1,
					       sizeof(u16), GFP_KERNEL);
			qp->rpm_buf = devm_kcalloc(dev, desc->rpm_buf_size,
						   sizeof(u32), GFP_KERNEL);
			if (!qp->bwsum || !qp->arb || !qp->rpm_buf) {
				dev_warn(dev, "RPM buffer alloc failed, ARB disabled\n");
				qp->rpm = NULL;
			} else {
				int rc;

				dev_info(dev, "RPM fabric ARB enabled (%d masters, %d slaves, %d tiered)\n",
					 desc->nmasters, desc->nslaves,
					 desc->ntieredslaves);

				/*
				 * One-shot sleep-context vote of zero bandwidth.
				 * Without an explicit SLEEP_STATE write, RPM has no
				 * fabric bandwidth target for deep-sleep and may
				 * keep the active vote applied indefinitely,
				 * preventing DDR from dropping its rate when CPUs
				 * power-collapse. The buffer is devm_kcalloc'd so
				 * it is all-zero at this point — written before
				 * any consumer can drive an active vote that would
				 * dirty it.
				 *
				 * msm8660_rpm_commit() writes ACTIVE_STATE only;
				 * SLEEP_STATE remains zero for the provider's
				 * lifetime, so this vote does not need refreshing.
				 */
				rc = qcom_rpm_write(qp->rpm,
						    QCOM_RPM_SLEEP_STATE,
						    desc->rpm_resource,
						    qp->rpm_buf,
						    desc->rpm_buf_size);
				if (rc)
					dev_warn(dev, "RPM fabric sleep vote failed: %d\n",
						 rc);
			}
		}
	}

	provider = &qp->provider;
	provider->dev = dev;
	provider->set = msm8660_icc_set;
	provider->aggregate = icc_std_aggregate;
	provider->xlate = of_icc_xlate_onecell;
	provider->data = data;
	provider->get_bw = msm8660_get_bw;

	icc_provider_init(provider);

	for (i = 0; i < num_nodes; i++) {
		size_t j;

		if (!qnodes[i])
			continue;

		node = icc_node_create(qnodes[i]->id);
		if (IS_ERR(node)) {
			ret = PTR_ERR(node);
			goto err_remove_nodes;
		}

		node->name = qnodes[i]->name;
		node->data = qnodes[i];
		icc_node_add(node, provider);

		dev_dbg(dev, "registered node %s\n", node->name);

		/* populate links */
		for (j = 0; j < qnodes[i]->num_links; j++)
			icc_link_create(node, qnodes[i]->links[j]);

		data->nodes[i] = node;
	}

	ret = icc_provider_register(provider);
	if (ret)
		goto err_remove_nodes;

	platform_set_drvdata(pdev, qp);

	dev_info(dev, "MSM8660 interconnect provider registered\n");

	return 0;

err_remove_nodes:
	icc_nodes_remove(provider);
	clk_bulk_disable_unprepare(qp->num_clks, qp->bus_clks);

	return ret;
}

static void msm8660_icc_remove(struct platform_device *pdev)
{
	struct msm8660_icc_provider *qp = platform_get_drvdata(pdev);

	icc_provider_deregister(&qp->provider);
	icc_nodes_remove(&qp->provider);
	if (qp->num_clks)
		clk_bulk_disable_unprepare(qp->num_clks, qp->bus_clks);
}

static const struct of_device_id msm8660_noc_of_match[] = {
	{ .compatible = "qcom,msm8660-apps-fabric", .data = &msm8660_afab },
	{ .compatible = "qcom,msm8660-system-fabric", .data = &msm8660_sfab },
	{ .compatible = "qcom,msm8660-mmss-fabric", .data = &msm8660_mmfab },
	{ .compatible = "qcom,msm8660-daytona-fabric", .data = &msm8660_dfab },
	{ },
};
MODULE_DEVICE_TABLE(of, msm8660_noc_of_match);

static struct platform_driver msm8660_noc_driver = {
	.probe = msm8660_icc_probe,
	.remove = msm8660_icc_remove,
	.driver = {
		.name = "qnoc-msm8660",
		.of_match_table = msm8660_noc_of_match,
		.sync_state = icc_sync_state,
	},
};
/*
 * Register the NOC provider at core_initcall, matching the mainline pattern
 * used by newer Qualcomm SoCs (sm8450, glymur, qdu1000, sc8280xp, sm8750).
 *
 * Why not module_platform_driver (device_initcall)?  drivers/Makefile lists
 * drivers/interconnect/ at position 189, *after* every ICC-consumer subdir
 * (clk/ @40, soc/ @46, gpu/ @68, base/mfd/ @76, spmi/ @89, usb/ @106,
 * i2c/ @116, mmc/ @133, remoteproc/ @158).  Within a single initcall level
 * execution order = link order, so a device_initcall registration here runs
 * *after* every consumer has already tried to probe.  Mainline relies on
 * deferred-probe retry to recover from that, but in this tree some consumer
 * (apcs-msm8660 + cpufreq cascade suspected) fails to recover within
 * deferred_probe_timeout=5 and boot dies at the Tux splash with no rootfs.
 * Empirically confirmed 2026-05-29 with module_platform_driver (commits
 * 99275d8a8ae9 + ca35c591854c, reverted).
 *
 * icc_provider_register does not require icc_init to have run first --
 * the framework's locks are statically DEFINE_MUTEX'd -- so registering
 * the provider at core_initcall (before icc_init at subsys_initcall) is
 * safe, same as mainline sm8450 etc.
 */
static int __init msm8660_noc_driver_init(void)
{
	return platform_driver_register(&msm8660_noc_driver);
}
core_initcall(msm8660_noc_driver_init);

static void __exit msm8660_noc_driver_exit(void)
{
	platform_driver_unregister(&msm8660_noc_driver);
}
module_exit(msm8660_noc_driver_exit);

MODULE_DESCRIPTION("Qualcomm MSM8x60 interconnect driver");
MODULE_LICENSE("GPL v2");
