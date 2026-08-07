// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright(c) 2015-2017 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#include <linux/ctype.h>
#include <rdma/ib_sysfs.h>

#include "hfi2.h"
#include "mad.h"
#include "trace.h"
#include "chip_gen.h"
#include "vf2pf.h"

static struct hfi2_pportdata *hfi2_get_pportdata_kobj(struct kobject *kobj)
{
	u32 port_num;
	struct ib_device *ibdev = ib_port_sysfs_get_ibdev_kobj(kobj, &port_num);
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);

	return &dd->pport[port_num - 1];
}

/*
 * Start of per-port congestion control structures and support code
 */

/*
 * Congestion control table size followed by table entries
 */
static ssize_t cc_table_bin_read(struct file *filp, struct kobject *kobj,
				 const struct bin_attribute *bin_attr,
				 char *buf, loff_t pos, size_t count)
{
	int ret;
	struct hfi2_pportdata *ppd = hfi2_get_pportdata_kobj(kobj);
	struct cc_state *cc_state;

	ret = ppd->total_cct_entry * sizeof(struct ib_cc_table_entry_shadow)
		 + sizeof(__be16);

	if (pos > ret)
		return -EINVAL;

	if (count > ret - pos)
		count = ret - pos;

	if (!count)
		return count;

	rcu_read_lock();
	cc_state = get_cc_state(ppd);
	if (!cc_state) {
		rcu_read_unlock();
		return -EINVAL;
	}
	memcpy(buf, (void *)&cc_state->cct + pos, count);
	rcu_read_unlock();

	return count;
}
static const BIN_ATTR_RO(cc_table_bin, PAGE_SIZE);

/*
 * Congestion settings: port control, control map and an array of 16
 * entries for the congestion entries - increase, timer, event log
 * trigger threshold and the minimum injection rate delay.
 */
static ssize_t cc_setting_bin_read(struct file *filp, struct kobject *kobj,
				   const struct bin_attribute *bin_attr,
				   char *buf, loff_t pos, size_t count)
{
	struct hfi2_pportdata *ppd = hfi2_get_pportdata_kobj(kobj);
	int ret;
	struct cc_state *cc_state;

	ret = sizeof(struct opa_congestion_setting_attr_shadow);

	if (pos > ret)
		return -EINVAL;
	if (count > ret - pos)
		count = ret - pos;

	if (!count)
		return count;

	rcu_read_lock();
	cc_state = get_cc_state(ppd);
	if (!cc_state) {
		rcu_read_unlock();
		return -EINVAL;
	}
	memcpy(buf, (void *)&cc_state->cong_setting + pos, count);
	rcu_read_unlock();

	return count;
}
static const BIN_ATTR_RO(cc_setting_bin, PAGE_SIZE);

static const struct bin_attribute *const port_cc_bin_attributes[] = {
	&bin_attr_cc_setting_bin,
	&bin_attr_cc_table_bin,
	NULL
};

static ssize_t cc_prescan_show(struct ib_device *ibdev, u32 port_num,
			       struct ib_port_attribute *attr, char *buf)
{
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_pportdata *ppd = &dd->pport[port_num - 1];

	return sysfs_emit(buf, "%s\n", ppd->cc_prescan ? "on" : "off");
}

static ssize_t cc_prescan_store(struct ib_device *ibdev, u32 port_num,
				struct ib_port_attribute *attr, const char *buf,
				size_t count)
{
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_pportdata *ppd = &dd->pport[port_num - 1];

	if (!memcmp(buf, "on", 2))
		ppd->cc_prescan = true;
	else if (!memcmp(buf, "off", 3))
		ppd->cc_prescan = false;

	return count;
}
static IB_PORT_ATTR_ADMIN_RW(cc_prescan);

static struct attribute *port_cc_attributes[] = {
	&ib_port_attr_cc_prescan.attr,
	NULL
};

static const struct attribute_group port_cc_group = {
	.name = "CCMgtA",
	.attrs = port_cc_attributes,
	.bin_attrs = port_cc_bin_attributes,
};

/* Start sc2vl */
struct hfi2_sc2vl_attr {
	struct ib_port_attribute attr;
	int sc;
};

static ssize_t sc2vl_attr_show(struct ib_device *ibdev, u32 port_num,
			       struct ib_port_attribute *attr, char *buf)
{
	struct hfi2_sc2vl_attr *sattr =
		container_of(attr, struct hfi2_sc2vl_attr, attr);
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_pportdata *ppd = &dd->pport[port_num - 1];

	return sysfs_emit(buf, "%u\n", *((u8 *)ppd->sc2vl + sattr->sc));
}

#define HFI2_SC2VL_ATTR(N)                                                     \
	static struct hfi2_sc2vl_attr hfi2_sc2vl_attr_##N = {                  \
		.attr = __ATTR(N, 0444, sc2vl_attr_show, NULL),                \
		.sc = N,                                                       \
	}

HFI2_SC2VL_ATTR(0);
HFI2_SC2VL_ATTR(1);
HFI2_SC2VL_ATTR(2);
HFI2_SC2VL_ATTR(3);
HFI2_SC2VL_ATTR(4);
HFI2_SC2VL_ATTR(5);
HFI2_SC2VL_ATTR(6);
HFI2_SC2VL_ATTR(7);
HFI2_SC2VL_ATTR(8);
HFI2_SC2VL_ATTR(9);
HFI2_SC2VL_ATTR(10);
HFI2_SC2VL_ATTR(11);
HFI2_SC2VL_ATTR(12);
HFI2_SC2VL_ATTR(13);
HFI2_SC2VL_ATTR(14);
HFI2_SC2VL_ATTR(15);
HFI2_SC2VL_ATTR(16);
HFI2_SC2VL_ATTR(17);
HFI2_SC2VL_ATTR(18);
HFI2_SC2VL_ATTR(19);
HFI2_SC2VL_ATTR(20);
HFI2_SC2VL_ATTR(21);
HFI2_SC2VL_ATTR(22);
HFI2_SC2VL_ATTR(23);
HFI2_SC2VL_ATTR(24);
HFI2_SC2VL_ATTR(25);
HFI2_SC2VL_ATTR(26);
HFI2_SC2VL_ATTR(27);
HFI2_SC2VL_ATTR(28);
HFI2_SC2VL_ATTR(29);
HFI2_SC2VL_ATTR(30);
HFI2_SC2VL_ATTR(31);

static struct attribute *port_sc2vl_attributes[] = {
	&hfi2_sc2vl_attr_0.attr.attr,
	&hfi2_sc2vl_attr_1.attr.attr,
	&hfi2_sc2vl_attr_2.attr.attr,
	&hfi2_sc2vl_attr_3.attr.attr,
	&hfi2_sc2vl_attr_4.attr.attr,
	&hfi2_sc2vl_attr_5.attr.attr,
	&hfi2_sc2vl_attr_6.attr.attr,
	&hfi2_sc2vl_attr_7.attr.attr,
	&hfi2_sc2vl_attr_8.attr.attr,
	&hfi2_sc2vl_attr_9.attr.attr,
	&hfi2_sc2vl_attr_10.attr.attr,
	&hfi2_sc2vl_attr_11.attr.attr,
	&hfi2_sc2vl_attr_12.attr.attr,
	&hfi2_sc2vl_attr_13.attr.attr,
	&hfi2_sc2vl_attr_14.attr.attr,
	&hfi2_sc2vl_attr_15.attr.attr,
	&hfi2_sc2vl_attr_16.attr.attr,
	&hfi2_sc2vl_attr_17.attr.attr,
	&hfi2_sc2vl_attr_18.attr.attr,
	&hfi2_sc2vl_attr_19.attr.attr,
	&hfi2_sc2vl_attr_20.attr.attr,
	&hfi2_sc2vl_attr_21.attr.attr,
	&hfi2_sc2vl_attr_22.attr.attr,
	&hfi2_sc2vl_attr_23.attr.attr,
	&hfi2_sc2vl_attr_24.attr.attr,
	&hfi2_sc2vl_attr_25.attr.attr,
	&hfi2_sc2vl_attr_26.attr.attr,
	&hfi2_sc2vl_attr_27.attr.attr,
	&hfi2_sc2vl_attr_28.attr.attr,
	&hfi2_sc2vl_attr_29.attr.attr,
	&hfi2_sc2vl_attr_30.attr.attr,
	&hfi2_sc2vl_attr_31.attr.attr,
	NULL
};

static const struct attribute_group port_sc2vl_group = {
	.name = "sc2vl",
	.attrs = port_sc2vl_attributes,
};
/* End sc2vl */

/* Start sl2sc */
struct hfi2_sl2sc_attr {
	struct ib_port_attribute attr;
	int sl;
};

static ssize_t sl2sc_attr_show(struct ib_device *ibdev, u32 port_num,
			       struct ib_port_attribute *attr, char *buf)
{
	struct hfi2_sl2sc_attr *sattr =
		container_of(attr, struct hfi2_sl2sc_attr, attr);
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_ibport *ibp = &dd->pport[port_num - 1].ibport_data;

	return sysfs_emit(buf, "%u\n", ibp->sl_to_sc[sattr->sl]);
}

#define HFI2_SL2SC_ATTR(N)                                                     \
	static struct hfi2_sl2sc_attr hfi2_sl2sc_attr_##N = {                  \
		.attr = __ATTR(N, 0444, sl2sc_attr_show, NULL), .sl = N        \
	}

HFI2_SL2SC_ATTR(0);
HFI2_SL2SC_ATTR(1);
HFI2_SL2SC_ATTR(2);
HFI2_SL2SC_ATTR(3);
HFI2_SL2SC_ATTR(4);
HFI2_SL2SC_ATTR(5);
HFI2_SL2SC_ATTR(6);
HFI2_SL2SC_ATTR(7);
HFI2_SL2SC_ATTR(8);
HFI2_SL2SC_ATTR(9);
HFI2_SL2SC_ATTR(10);
HFI2_SL2SC_ATTR(11);
HFI2_SL2SC_ATTR(12);
HFI2_SL2SC_ATTR(13);
HFI2_SL2SC_ATTR(14);
HFI2_SL2SC_ATTR(15);
HFI2_SL2SC_ATTR(16);
HFI2_SL2SC_ATTR(17);
HFI2_SL2SC_ATTR(18);
HFI2_SL2SC_ATTR(19);
HFI2_SL2SC_ATTR(20);
HFI2_SL2SC_ATTR(21);
HFI2_SL2SC_ATTR(22);
HFI2_SL2SC_ATTR(23);
HFI2_SL2SC_ATTR(24);
HFI2_SL2SC_ATTR(25);
HFI2_SL2SC_ATTR(26);
HFI2_SL2SC_ATTR(27);
HFI2_SL2SC_ATTR(28);
HFI2_SL2SC_ATTR(29);
HFI2_SL2SC_ATTR(30);
HFI2_SL2SC_ATTR(31);

static struct attribute *port_sl2sc_attributes[] = {
	&hfi2_sl2sc_attr_0.attr.attr,
	&hfi2_sl2sc_attr_1.attr.attr,
	&hfi2_sl2sc_attr_2.attr.attr,
	&hfi2_sl2sc_attr_3.attr.attr,
	&hfi2_sl2sc_attr_4.attr.attr,
	&hfi2_sl2sc_attr_5.attr.attr,
	&hfi2_sl2sc_attr_6.attr.attr,
	&hfi2_sl2sc_attr_7.attr.attr,
	&hfi2_sl2sc_attr_8.attr.attr,
	&hfi2_sl2sc_attr_9.attr.attr,
	&hfi2_sl2sc_attr_10.attr.attr,
	&hfi2_sl2sc_attr_11.attr.attr,
	&hfi2_sl2sc_attr_12.attr.attr,
	&hfi2_sl2sc_attr_13.attr.attr,
	&hfi2_sl2sc_attr_14.attr.attr,
	&hfi2_sl2sc_attr_15.attr.attr,
	&hfi2_sl2sc_attr_16.attr.attr,
	&hfi2_sl2sc_attr_17.attr.attr,
	&hfi2_sl2sc_attr_18.attr.attr,
	&hfi2_sl2sc_attr_19.attr.attr,
	&hfi2_sl2sc_attr_20.attr.attr,
	&hfi2_sl2sc_attr_21.attr.attr,
	&hfi2_sl2sc_attr_22.attr.attr,
	&hfi2_sl2sc_attr_23.attr.attr,
	&hfi2_sl2sc_attr_24.attr.attr,
	&hfi2_sl2sc_attr_25.attr.attr,
	&hfi2_sl2sc_attr_26.attr.attr,
	&hfi2_sl2sc_attr_27.attr.attr,
	&hfi2_sl2sc_attr_28.attr.attr,
	&hfi2_sl2sc_attr_29.attr.attr,
	&hfi2_sl2sc_attr_30.attr.attr,
	&hfi2_sl2sc_attr_31.attr.attr,
	NULL
};

static const struct attribute_group port_sl2sc_group = {
	.name = "sl2sc",
	.attrs = port_sl2sc_attributes,
};

/* End sl2sc */

/* Start vl2mtu */

struct hfi2_vl2mtu_attr {
	struct ib_port_attribute attr;
	int vl;
};

static ssize_t vl2mtu_attr_show(struct ib_device *ibdev, u32 port_num,
				struct ib_port_attribute *attr, char *buf)
{
	struct hfi2_vl2mtu_attr *vlattr =
		container_of(attr, struct hfi2_vl2mtu_attr, attr);
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_pportdata *ppd = &dd->pport[port_num - 1];

	return sysfs_emit(buf, "%u\n", ppd->vld[vlattr->vl].mtu);
}

#define HFI2_VL2MTU_ATTR(N)                                                    \
	static struct hfi2_vl2mtu_attr hfi2_vl2mtu_attr_##N = {                \
		.attr = __ATTR(N, 0444, vl2mtu_attr_show, NULL),               \
		.vl = N,                                                       \
	}

HFI2_VL2MTU_ATTR(0);
HFI2_VL2MTU_ATTR(1);
HFI2_VL2MTU_ATTR(2);
HFI2_VL2MTU_ATTR(3);
HFI2_VL2MTU_ATTR(4);
HFI2_VL2MTU_ATTR(5);
HFI2_VL2MTU_ATTR(6);
HFI2_VL2MTU_ATTR(7);
HFI2_VL2MTU_ATTR(8);
HFI2_VL2MTU_ATTR(9);
HFI2_VL2MTU_ATTR(10);
HFI2_VL2MTU_ATTR(11);
HFI2_VL2MTU_ATTR(12);
HFI2_VL2MTU_ATTR(13);
HFI2_VL2MTU_ATTR(14);
HFI2_VL2MTU_ATTR(15);

static struct attribute *port_vl2mtu_attributes[] = {
	&hfi2_vl2mtu_attr_0.attr.attr,
	&hfi2_vl2mtu_attr_1.attr.attr,
	&hfi2_vl2mtu_attr_2.attr.attr,
	&hfi2_vl2mtu_attr_3.attr.attr,
	&hfi2_vl2mtu_attr_4.attr.attr,
	&hfi2_vl2mtu_attr_5.attr.attr,
	&hfi2_vl2mtu_attr_6.attr.attr,
	&hfi2_vl2mtu_attr_7.attr.attr,
	&hfi2_vl2mtu_attr_8.attr.attr,
	&hfi2_vl2mtu_attr_9.attr.attr,
	&hfi2_vl2mtu_attr_10.attr.attr,
	&hfi2_vl2mtu_attr_11.attr.attr,
	&hfi2_vl2mtu_attr_12.attr.attr,
	&hfi2_vl2mtu_attr_13.attr.attr,
	&hfi2_vl2mtu_attr_14.attr.attr,
	&hfi2_vl2mtu_attr_15.attr.attr,
	NULL
};

static const struct attribute_group port_vl2mtu_group = {
	.name = "vl2mtu",
	.attrs = port_vl2mtu_attributes,
};

/* start per-port nctxts and freectxts */
static ssize_t num_ctxts_show(struct ib_device *ibdev, u32 port_num,
			      struct ib_port_attribute *attr, char *buf)
{
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_portrsrcs *pr = &dd->rsrcs.ppr[port_num - 1];

	return sysfs_emit(buf, "%u\n", pr->num_user_contexts);
}

static IB_PORT_ATTR_RO(num_ctxts);

static ssize_t num_freectxts_show(struct ib_device *ibdev, u32 port_num,
				  struct ib_port_attribute *attr, char *buf)
{
	struct hfi2_devdata *dd = dd_from_ibdev(ibdev);
	struct hfi2_pportdata *ppd = &dd->pport[port_num - 1];

	return sysfs_emit(buf, "%u\n", ppd->freectxts);
}

static IB_PORT_ATTR_RO(num_freectxts);

static struct attribute *port_ctxt_attributes[] = {
	&ib_port_attr_num_ctxts.attr,
	&ib_port_attr_num_freectxts.attr,
	NULL,
};

static struct attribute_group port_ctxt_group = {
	.attrs = port_ctxt_attributes,
};

/* end of per-port file structures and support code */

/*
 * Start of per-unit (or driver, in some cases, but replicated
 * per unit) functions (these get a device *)
 */
static ssize_t hw_rev_show(struct device *device, struct device_attribute *attr,
			   char *buf)
{
	struct hfi2_ibdev *dev =
		rdma_device_to_drv_device(device, struct hfi2_ibdev, rdi.ibdev);

	return sysfs_emit(buf, "%x\n", dd_from_dev(dev)->minrev);
}
static DEVICE_ATTR_RO(hw_rev);

static ssize_t board_id_show(struct device *device,
			     struct device_attribute *attr, char *buf)
{
	struct hfi2_ibdev *dev =
		rdma_device_to_drv_device(device, struct hfi2_ibdev, rdi.ibdev);
	struct hfi2_devdata *dd = dd_from_dev(dev);

	if (!dd->boardname)
		return -EINVAL;

	return sysfs_emit(buf, "%s\n", dd->boardname);
}
static DEVICE_ATTR_RO(board_id);

static ssize_t boardversion_show(struct device *device,
				 struct device_attribute *attr, char *buf)
{
	struct hfi2_ibdev *dev =
		rdma_device_to_drv_device(device, struct hfi2_ibdev, rdi.ibdev);
	struct hfi2_devdata *dd = dd_from_dev(dev);

	/* The string printed here is already newline-terminated. */
	return sysfs_emit(buf, "%s\n", dd->boardversion);
}
static DEVICE_ATTR_RO(boardversion);

static ssize_t nctxts_show(struct device *device,
			   struct device_attribute *attr, char *buf)
{
	struct hfi2_ibdev *dev =
		rdma_device_to_drv_device(device, struct hfi2_ibdev, rdi.ibdev);
	struct hfi2_devdata *dd = dd_from_dev(dev);
	u32 total;
	int pidx;

	total = 0;
	for (pidx = 0; pidx < dd->num_pports; pidx++)
		total += dd->rsrcs.ppr[pidx].num_user_contexts;

	return sysfs_emit(buf, "%u\n", total);
}
static DEVICE_ATTR_RO(nctxts);

static ssize_t nfreectxts_show(struct device *device,
			       struct device_attribute *attr, char *buf)
{
	struct hfi2_ibdev *dev =
		rdma_device_to_drv_device(device, struct hfi2_ibdev, rdi.ibdev);
	struct hfi2_devdata *dd = dd_from_dev(dev);
	u32 total;
	int i;

	for (total = 0, i = 0; i < dd->num_pports; i++)
		total += dd->pport[i].freectxts;

	/* Return the number of free user ports (contexts) available. */
	return sysfs_emit(buf, "%u\n", total);
}
static DEVICE_ATTR_RO(nfreectxts);

static ssize_t serial_show(struct device *device,
			   struct device_attribute *attr, char *buf)
{
	struct hfi2_ibdev *dev =
		rdma_device_to_drv_device(device, struct hfi2_ibdev, rdi.ibdev);
	struct hfi2_devdata *dd = dd_from_dev(dev);

	/* dd->serial is already newline terminated in chip.c */
	return sysfs_emit(buf, "%s\n", dd->serial);
}
static DEVICE_ATTR_RO(serial);

static ssize_t chip_reset_store(struct device *device,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct hfi2_ibdev *dev =
		rdma_device_to_drv_device(device, struct hfi2_ibdev, rdi.ibdev);
	struct hfi2_devdata *dd = dd_from_dev(dev);
	int ret;

	if (count < 5 || memcmp(buf, "reset", 5)) {
		ret = -EINVAL;
		goto bail;
	}

	ret = hfi2_reset_device(dd->unit);
bail:
	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_WO(chip_reset);

/*
 * Convert the reported temperature from an integer (reported in
 * units of 0.25C) to a floating point number.
 */
#define temp_d(t) ((t) >> 2)
#define temp_f(t) (((t)&0x3) * 25u)

static ssize_t emit_temperature(char *buf, ssize_t off, const char *lbl,
				s16 temp, bool valid)
{
	int n = temp * 125;
	bool neg = false;

	if (!valid)
		return sysfs_emit_at(buf, off, "%s none\n", lbl);
	if (n < 0) {
		neg = true;
		n = -n;
	}
	return sysfs_emit_at(buf, off, "%s %s%u.%03u\n", lbl, neg ? "-" : "",
			     n / 1000, n % 1000);
}

static ssize_t emit_cport_temp(struct hfi2_devdata *dd, char *buf,
			       struct cport_temp *temp)
{
	ssize_t off = 0;

	off += emit_temperature(buf, off, "ASIC", temp->asic,
				temp->asic_valid);
	off += emit_temperature(buf, off, "QSFP1", temp->qsfp1,
				temp->qsfp1_valid);
	off += emit_temperature(buf, off, "QSFP2", temp->qsfp2,
				temp->qsfp2_valid);
	return off;
}

/*
 * Dump tempsense values, in decimal, to ease shell-scripts.
 */
static ssize_t tempsense_show(struct device *device,
			      struct device_attribute *attr, char *buf)
{
	struct hfi2_ibdev *dev =
		rdma_device_to_drv_device(device, struct hfi2_ibdev, rdi.ibdev);
	struct hfi2_devdata *dd = dd_from_dev(dev);
	struct hfi2_cport *cport;
	struct cport_temp temp = {0};
	int ret;

	if (dd->params->chip_type == CHIP_WFR) {
		struct hfi2_temp temp;

		ret = hfi2_tempsense_rd(dd, &temp);
		if (ret)
			return ret;

		return sysfs_emit(buf, "%u.%02u %u.%02u %u.%02u %u.%02u %u %u %u\n",
				  temp_d(temp.curr), temp_f(temp.curr),
				  temp_d(temp.lo_lim), temp_f(temp.lo_lim),
				  temp_d(temp.hi_lim), temp_f(temp.hi_lim),
				  temp_d(temp.crit_lim), temp_f(temp.crit_lim),
				  temp.triggers & 0x1,
				  temp.triggers & 0x2,
				  temp.triggers & 0x4);
	}

	/* beyond WFR */
	cport = dd->cport;
	if (!cport)
		return -EINVAL;

	/* the firmware does not update often, use cached value until timeout */
	if (time_after(jiffies, cport->temp_timeout)) {
		ret = hfi2_cport_read_temp(dd, &temp);
		if (ret)
			return ret;
		cport->temp = temp;
		/* firmware updates every ~15 seconds */
		cport->temp_timeout = jiffies + msecs_to_jiffies(5000);
	} else {
		temp = cport->temp;
	}
	return emit_cport_temp(dd, buf, &temp);
}
static DEVICE_ATTR_RO(tempsense);

/*
 * Dump device resource assignments.
 */
static ssize_t hw_resources_show(struct device *device,
				 struct device_attribute *attr, char *buf)
{
	struct hfi2_ibdev *dev =
		rdma_device_to_drv_device(device, struct hfi2_ibdev, rdi.ibdev);
	struct hfi2_devdata *dd = dd_from_dev(dev);
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	ssize_t off = 0;
	int pidx;

	off += sysfs_emit_at(buf, off,
			     "pfunit %u\n"
			     "si_idx %u\n"
			     "numvfs %u\n",
			     dr->pfunit, dr->si_idx, dr->num_vfs);

	off += sysfs_emit_at(buf, off,
			     "sctxt  %u-%u\n"
			     "rctxt  %u-%u\n"
			     "sde    %u-%u\n"
			     "rcvary %u-%u\n"
			     "pio    %u-%u\n",
			     dr->c.first_send_context, dr->c.last_send_context - 1,
			     dr->c.first_rcv_context, dr->c.last_rcv_context - 1,
			     dr->first_sdma_engine, dr->last_sdma_engine - 1,
			     dr->c.first_rcvarray_entry, dr->c.last_rcvarray_entry - 1,
			     dr->c.first_pio_block, dr->c.last_pio_block - 1);

	for (pidx = 0; pidx < dd->num_pports; ++pidx) {
		struct hfi2_portrsrcs *pr = &dr->ppr[pidx];

		if (!pr->num_rcv_contexts) {
			off += sysfs_emit_at(buf, off, "p%d not used\n", pidx);
			continue;
		}

		off += sysfs_emit_at(buf, off,
				     "p%d.rctxt    %u-%u\n"
				     "p%d.firstdyn %u\n",
				     pidx, pr->rcv_context_base,
				     pr->rcv_context_base + pr->num_rcv_contexts - 1,
				     pidx, pr->first_dyn_alloc_ctxt);
		off += sysfs_emit_at(buf, off,
				     "p%d.nkrcvq   %u\n"
				     "p%d.nnetdev  %u\n"
				     "p%d.nuctxts  %u\n",
				     pidx, pr->n_krcv_queues,
				     pidx, pr->num_netdev_contexts,
				     pidx, pr->num_user_contexts);
		off += sysfs_emit_at(buf, off,
				     "p%d.rcvary   %u\n",
				     pidx, pr->rcv_array_base);
	}
	off += hfi2_vf2pf_sysfs_emit_at(dd, buf, off);
	if (off >= PAGE_SIZE) {
		dd_dev_warn(dd, "hw_resources exceeds PAGE_SIZE.\n");
		return -EFBIG;
	}
	return off;
}
static DEVICE_ATTR_RO(hw_resources);

/*
 * end of per-unit (or driver, in some cases, but replicated
 * per unit) functions
 */

/* start of per-unit file structures and support code */
static struct attribute *hfi2_attributes[] = {
	&dev_attr_hw_rev.attr,
	&dev_attr_board_id.attr,
	&dev_attr_nctxts.attr,
	&dev_attr_nfreectxts.attr,
	&dev_attr_serial.attr,
	&dev_attr_boardversion.attr,
	&dev_attr_tempsense.attr,
	&dev_attr_chip_reset.attr,
	&dev_attr_hw_resources.attr,
	NULL,
};

const struct attribute_group hfi2_ib_hfi2_attr_group = {
	.attrs = hfi2_attributes,
};

const struct attribute_group *hfi2_wfr_attr_port_groups[] = {
	&port_cc_group,
	&port_sc2vl_group,
	&port_sl2sc_group,
	&port_vl2mtu_group,
	&port_ctxt_group,
	NULL,
};

const struct attribute_group *hfi2_cport_attr_port_groups[] = {
	&port_sc2vl_group,
	&port_sl2sc_group,
	&port_vl2mtu_group,
	&port_ctxt_group,
	NULL,
};

struct sde_attribute {
	struct attribute attr;
	ssize_t (*show)(struct sdma_engine *sde, char *buf);
	ssize_t (*store)(struct sdma_engine *sde, const char *buf, size_t cnt);
};

static ssize_t sde_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct sde_attribute *sde_attr =
		container_of(attr, struct sde_attribute, attr);
	struct sdma_engine *sde =
		container_of(kobj, struct sdma_engine, kobj);

	if (!sde_attr->show)
		return -EINVAL;

	return sde_attr->show(sde, buf);
}

static ssize_t sde_store(struct kobject *kobj, struct attribute *attr,
			 const char *buf, size_t count)
{
	struct sde_attribute *sde_attr =
		container_of(attr, struct sde_attribute, attr);
	struct sdma_engine *sde =
		container_of(kobj, struct sdma_engine, kobj);

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (!sde_attr->store)
		return -EINVAL;

	return sde_attr->store(sde, buf, count);
}

static const struct sysfs_ops sde_sysfs_ops = {
	.show = sde_show,
	.store = sde_store,
};

static const struct kobj_type sde_ktype = {
	.sysfs_ops = &sde_sysfs_ops,
};

#define SDE_ATTR(_name, _mode, _show, _store) \
	struct sde_attribute sde_attr_##_name = \
		__ATTR(_name, _mode, _show, _store)

static ssize_t sde_show_cpu_to_sde_map(struct sdma_engine *sde, char *buf)
{
	return hfi2_sdma_get_cpu_to_sde_map(sde, buf);
}

static ssize_t sde_store_cpu_to_sde_map(struct sdma_engine *sde,
					const char *buf, size_t count)
{
	return hfi2_sdma_set_cpu_to_sde_map(sde, buf, count);
}

static ssize_t sde_show_vl(struct sdma_engine *sde, char *buf)
{
	struct hfi2_devdata *dd = sde->dd;
	ssize_t off = 0;
	int vl;
	int i;

	for (i = 0; i < dd->num_pports; i++) {
		vl = hfi2_sdma_engine_get_vl(&dd->pport[i], sde);
		if (vl < 0)
			return vl;
		if (i != 0)
			off += sysfs_emit_at(buf, off, ",");
		off += sysfs_emit_at(buf, off, "%d", vl);
	}

	return off;
}

static SDE_ATTR(cpu_list, 0644,
		sde_show_cpu_to_sde_map,
		sde_store_cpu_to_sde_map);
static SDE_ATTR(vl, 0444, sde_show_vl, NULL);

static struct sde_attribute *sde_attribs[] = {
	&sde_attr_cpu_list,
	&sde_attr_vl
};

/*
 * Register and create our files in /sys/class/infiniband.
 */
int hfi2_verbs_register_sysfs(struct hfi2_devdata *dd)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	struct ib_device *dev = &dd->verbs_dev.rdi.ibdev;
	struct device *class_dev = &dev->dev;
	int i, j, ret;

	for (i = dr->first_sdma_engine; i < dr->last_sdma_engine; i++) {
		ret = kobject_init_and_add(&dd->per_sdma[i].kobj,
					   &sde_ktype, &class_dev->kobj,
					   "sdma%d", i);
		if (ret)
			goto bail;

		for (j = 0; j < ARRAY_SIZE(sde_attribs); j++) {
			ret = sysfs_create_file(&dd->per_sdma[i].kobj,
						&sde_attribs[j]->attr);
			if (ret)
				goto bail;
		}
	}

	hfi2_vf2pf_init_sysfs(dd, class_dev);

	return 0;
bail:
	/*
	 * The function kobject_put() will call kobject_del() if the kobject
	 * has been added successfully. The sysfs files created under the
	 * kobject directory will also be removed during the process.
	 */
	for (; i >= dr->first_sdma_engine; i--)
		kobject_put(&dd->per_sdma[i].kobj);

	return ret;
}

/*
 * Unregister and remove our files in /sys/class/infiniband.
 */
void hfi2_verbs_unregister_sysfs(struct hfi2_devdata *dd)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	int i;

	/* Unwind operations in hfi2_verbs_register_sysfs() */
	for (i = dr->first_sdma_engine; i < dr->last_sdma_engine; i++)
		kobject_put(&dd->per_sdma[i].kobj);
}
