/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
#ifndef _DT_BINDINGS_CLK_TI_CDCE6214_H
#define _DT_BINDINGS_CLK_TI_CDCE6214_H

/*
 * primary/secondary inputs. Not registered as clocks, but used
 * as reg properties for the subnodes specifying the input properties
 */
#define CDCE6214_CLK_PRIREF	0
#define CDCE6214_CLK_SECREF	1

/*
 * Clock indices for the clocks provided by the CDCE6214. Also used
 * as reg properties for the subnodes specifying the output properties
 */
#define CDCE6214_CLK_OUT0	2
#define CDCE6214_CLK_OUT1	3
#define CDCE6214_CLK_OUT2	4
#define CDCE6214_CLK_OUT3	5
#define CDCE6214_CLK_OUT4	6
#define CDCE6214_CLK_PLL	7
#define CDCE6214_CLK_PSA	8
#define CDCE6214_CLK_PSB	9

#endif /* _DT_BINDINGS_CLK_TI_CDCE6214_H */
