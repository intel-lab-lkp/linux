/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */

#ifndef __DT_BINDINGS_AIROHA_SCU_SSR_H
#define __DT_BINDINGS_AIROHA_SCU_SSR_H

/* WiFi1 port can be PCIe0 2 line, PCIe0 1 line or Ethernet (USXGMII/HSGMII) */
#define AIROHA_SCU_SSR_WIFI1_PCIE0_2LINE	0
#define AIROHA_SCU_SSR_WIFI1_PCIE0		1
#define AIROHA_SCU_SSR_WIFI1_ETHERNET		2

/* WiFi2 port can be PCIe0 2 line, PCIe1 1 line or Ethernet (USXGMII/HSGMII) */
#define AIROHA_SCU_SSR_WIFI2_PCIE0_2LINE	0
#define AIROHA_SCU_SSR_WIFI2_PCIE1		1
#define AIROHA_SCU_SSR_WIFI2_ETHERNET		2

/* USB1 port can be USB 3.0 port or Ethernet (HSGMII) */
#define AIROHA_SCU_SSR_USB1_USB			0
#define AIROHA_SCU_SSR_USB1_ETHERNET		1

/* USB2 port can be USB 3.0 port or PCIe2 1 line */
#define AIROHA_SCU_SSR_USB2_USB			0
#define AIROHA_SCU_SSR_USB2_PCIE2		1

#endif /* __DT_BINDINGS_AIROHA_SCU_SSR_H */
