/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __AIROHA_SCU_SSR__
#define __AIROHA_SCU_SSR__

enum airoha_scu_serdes_port {
	AIROHA_SCU_SERDES_WIFI1 = 0,
	AIROHA_SCU_SERDES_WIFI2,
	AIROHA_SCU_SERDES_USB1,
	AIROHA_SCU_SERDES_USB2,

	AIROHA_SCU_MAX_SERDES_PORT,
};

int airoha_scu_ssr_get_serdes_mode(struct device *dev,
				   enum airoha_scu_serdes_port port);

#endif
