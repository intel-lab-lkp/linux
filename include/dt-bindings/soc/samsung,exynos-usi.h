/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Copyright (c) 2021 Linaro Ltd.
 * Author: Sam Protsenko <semen.protsenko@linaro.org>
 *
 * Device Tree bindings for Samsung Exynos USI (Universal Serial Interface).
 */

#ifndef __DT_BINDINGS_SAMSUNG_EXYNOS_USI_H
#define __DT_BINDINGS_SAMSUNG_EXYNOS_USI_H

#define USI_V1_NONE		0
#define USI_V1_I2C0		1
#define USI_V1_I2C1		2
#define USI_V1_I2C0_1		3
#define USI_V1_SPI		4
#define USI_V1_UART		5
#define USI_V1_UART_I2C1	6

#define USI_V2_NONE		0
#define USI_V2_UART		1
#define USI_V2_SPI		2
#define USI_V2_I2C		3

#endif /* __DT_BINDINGS_SAMSUNG_EXYNOS_USI_H */
