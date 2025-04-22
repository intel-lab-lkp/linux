/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025 Conclusive Engineering Sp. z o. o.
 */

#ifndef _NRF70_RF_PARAMS_H
#define _NRF70_RF_PARAMS_H

#define	NRF70_RF_DSSS_BKF(n)	((n) << 4)
#define	NRF70_RF_OFDM_BKF(n)	((n) << 0)

#define NRF70_PHY_CALIB_FLAG_RXDC		BIT(0)
#define NRF70_PHY_CALIB_FLAG_TXDC		BIT(1)
#define NRF70_PHY_CALIB_FLAG_TXPOW		0
#define NRF70_PHY_CALIB_FLAG_TXIQ		BIT(3)
#define NRF70_PHY_CALIB_FLAG_RXIQ		BIT(4)
#define NRF70_PHY_CALIB_FLAG_DPD		BIT(5)

#define NRF70_PHY_SCAN_CALIB_FLAG_RXDC		(1 << 16)
#define NRF70_PHY_SCAN_CALIB_FLAG_TXDC		(2 << 16)
#define NRF70_PHY_SCAN_CALIB_FLAG_TXPOW		(0 << 16)
#define NRF70_PHY_SCAN_CALIB_FLAG_TXIQ		(0 << 16)
#define NRF70_PHY_SCAN_CALIB_FLAG_RXIQ		(0 << 16)
#define NRF70_PHY_SCAN_CALIB_FLAG_DPD		(0 << 16)

#define NRF70_DEF_PHY_CALIB (NRF70_PHY_CALIB_FLAG_RXDC |		\
			     NRF70_PHY_CALIB_FLAG_TXDC |		\
			     NRF70_PHY_CALIB_FLAG_RXIQ |		\
			     NRF70_PHY_CALIB_FLAG_TXIQ |		\
			     NRF70_PHY_CALIB_FLAG_TXPOW |		\
			     NRF70_PHY_CALIB_FLAG_DPD |			\
			     NRF70_PHY_SCAN_CALIB_FLAG_RXDC |		\
			     NRF70_PHY_SCAN_CALIB_FLAG_TXDC |		\
			     NRF70_PHY_SCAN_CALIB_FLAG_RXIQ |		\
			     NRF70_PHY_SCAN_CALIB_FLAG_TXIQ |		\
			     NRF70_PHY_SCAN_CALIB_FLAG_TXPOW |		\
			     NRF70_PHY_SCAN_CALIB_FLAG_DPD)

/* Temperature based calibration params. */
#define NRF70_DEF_PHY_TEMP_CALIB (NRF70_PHY_CALIB_FLAG_RXDC |		\
				  NRF70_PHY_CALIB_FLAG_TXDC |		\
				  NRF70_PHY_CALIB_FLAG_RXIQ |		\
				  NRF70_PHY_CALIB_FLAG_TXIQ |		\
				  NRF70_PHY_CALIB_FLAG_TXPOW |		\
				  NRF70_PHY_CALIB_FLAG_DPD)

/* Convert from millivolts to vbat threshold. Value must be above 2.5 V. */
#define	NRF70_VBAT_MV_TO_VTH(n)	(((n) - 2500) / 70)

/* Undocumented PHY configuration parameters. */
#define NRF70_PHY_BLOB { \
	0x00, 0x70, 0x77, 0x00, 0x3f, 0x03, 0x24, 0x24, 0x00, 0x10, 0x00, \
	0x00, 0x28, 0x00, 0x32, 0x35, 0x00, 0x00, 0x0c, 0xf0, 0x08, 0x08, \
	0x7d, 0x81, 0x05, 0x01, 0x00, 0x71, 0x63, 0x03, 0x00, 0xee, 0xd5, \
	0x01, 0x00, 0x1f, 0x6f, 0x00, 0x00, 0x3b, 0x35, 0x01, 0x00, 0xf5, \
	0x2e, 0x00, 0x00, 0xe3, 0x5e, 0x00, 0x00, 0xb7, 0xb6, 0x00, 0x00, \
	0x66, 0xef, 0xfe, 0xff, 0xb5, 0xf6, 0x00, 0x00, 0x89, 0x62, 0x00, \
	0x00, 0x7a, 0x84, 0x02, 0x00, 0xe2, 0x8f, 0xfc, 0xff, 0x08, 0x08, \
	0x08, 0x08, 0x04, 0x08, 0x12, 0x01, 0x00, 0x00, 0x00, 0x00, 0xa1, \
	0xa1, 0x01, 0x78, 0x00, 0x00, 0x00, 0x08, 0x00, 0x50, 0x00, 0x3b, \
	0x02, 0x07, 0x26, 0x18, 0x18, 0x18, 0x18, 0x1a, 0x12, 0x0a, 0x14, \
	0x0e, 0x06, 0x00 \
}

#endif /* _NRF70_RF_PARAMS_H */
