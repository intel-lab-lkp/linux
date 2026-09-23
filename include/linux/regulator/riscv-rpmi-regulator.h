/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RISC-V RPMI voltage service group consumer interface
 *
 * Copyright (C) 2026 Shanghai StarFive Technology Co., Ltd.
 *
 * The domains of an RPMI voltage provider are regulators, and a consumer
 * normally names one the usual way, with a "<name>-supply" phandle to the
 * domain's node. A consumer may instead name a domain by its DOMAIN_ID,
 *
 *	voltage-domains = <&rpmi_voltage 7>;
 *	voltage-domain-names = "vdd";
 *
 * which needs no node for the domain but which the regulator core cannot
 * follow on its own. devm_rpmi_voltage_supply_alias() resolves such a name,
 * after which the consumer uses the regulator API as it would for any other
 * supply.
 */

#ifndef _LINUX_REGULATOR_RISCV_RPMI_REGULATOR_H_
#define _LINUX_REGULATOR_RISCV_RPMI_REGULATOR_H_

#include <linux/errno.h>

struct device;

#if IS_ENABLED(CONFIG_REGULATOR_RISCV_RPMI)

int devm_rpmi_voltage_supply_alias(struct device *dev, const char *id);

#else

static inline int devm_rpmi_voltage_supply_alias(struct device *dev,
						 const char *id)
{
	return -ENODEV;
}

#endif

#endif /* _LINUX_REGULATOR_RISCV_RPMI_REGULATOR_H_ */
