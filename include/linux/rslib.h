// SPDX-License-Identifier: GPL-2.0
/*
 * Generic Reed Solomon encoder / decoder library
 *
 * Copyright (C) 2004 Thomas Gleixner (tglx@linutronix.de)
 *
 * RS code lifted from reed solomon library written by Phil Karn
 * Copyright 2002 Phil Karn, KA9Q
 */
#ifndef _RSLIB_H_
#define _RSLIB_H_

#include <linux/types.h>	/* for gfp_t */
#include <linux/gfp.h>		/* for GFP_KERNEL */

/**
 * struct rs_codec - rs codec data
 *
 * @mm:		Bits per symbol
 * @nn:		Symbols per block (= (1<<mm)-1)
 * @alpha_to:	Antilog lookup table
 * @index_of:	log lookup table
 * @genpoly:	Generator polynomial
 * @genroot:	Roots of generator polynomial, index form
 * @nroots:	Number of generator roots = number of parity symbols
 * @fcr:	First consecutive root, index form
 * @prim:	Primitive element, index form
 * @iprim:	prim-th root of 1, index form
 * @gfpoly:	The primitive generator polynominal
 * @gffunc:	Function to generate the field, if non-canonical representation
 * @users:	Users of this structure
 * @list:	List entry for the rs codec list
*/
struct rs_codec {
	int		mm;
	int		nn;
	uint16_t	*alpha_to;
	uint16_t	*index_of;
	uint16_t	*genpoly;
	uint16_t	*genroot;
	int		nroots;
	int		fcr;
	int		prim;
	int		iprim;
	int		gfpoly;
	int		(*gffunc)(int);
	int		users;
	struct list_head list;
};

/**
 * struct rs_control - rs control structure per instance
 * @codec:	The codec used for this instance
 * @buffers:	Internal scratch buffers used in calls to decode_rs()
 */
struct rs_control {
	struct rs_codec	*codec;
	uint16_t	buffers[];
};

/* General purpose RS codec, 8-bit data width, symbol width 1-15 bit  */
#ifdef CONFIG_REED_SOLOMON_ENC8
int encode_rs8(struct rs_control *rs, uint8_t *data, int len, uint16_t *par,
	       uint16_t invmsk);
#endif
#ifdef CONFIG_REED_SOLOMON_DEC8
int decode_rs8(struct rs_control *rs, uint8_t *data, uint16_t *par, int len,
		uint16_t *s, int no_eras, int *eras_pos, uint16_t invmsk,
	       uint16_t *corr);
#endif

/* General purpose RS codec, 16-bit data width, symbol width 1-15 bit  */
#ifdef CONFIG_REED_SOLOMON_ENC16
int encode_rs16(struct rs_control *rs, uint16_t *data, int len, uint16_t *par,
		uint16_t invmsk);
#endif
#ifdef CONFIG_REED_SOLOMON_DEC16
int decode_rs16(struct rs_control *rs, uint16_t *data, uint16_t *par, int len,
		uint16_t *s, int no_eras, int *eras_pos, uint16_t invmsk,
		uint16_t *corr);
#endif

struct rs_control *init_rs_gfp(int symsize, int gfpoly, int fcr, int prim,
			       int nroots, gfp_t gfp);

/**
 * init_rs - Create a RS control struct and initialize it
 *  @symsize:	the symbol size (number of bits)
 *  @gfpoly:	the extended Galois field generator polynomial coefficients,
 *		with the 0th coefficient in the low order bit. The polynomial
 *		must be primitive;
 *  @fcr:	the first consecutive root of the rs code generator polynomial
 *		in index form
 *  @prim:	primitive element to generate polynomial roots
 *  @nroots:	RS code generator polynomial degree (number of roots)
 *
 * Allocations use GFP_KERNEL.
 */
static inline struct rs_control *init_rs(int symsize, int gfpoly, int fcr,
					 int prim, int nroots)
{
	return init_rs_gfp(symsize, gfpoly, fcr, prim, nroots, GFP_KERNEL);
}

struct rs_control *init_rs_non_canonical(int symsize, int (*func)(int),
					 int fcr, int prim, int nroots);

/* Release a rs control structure */
void free_rs(struct rs_control *rs);

/**
 * rs_modnn() - Modulo replacement for galois field arithmetics
 *
 *  @rs:	Pointer to the RS codec
 *  @x:		the value to reduce (requires x >= 0)
 *
 *  where
 *  rs->mm = number of bits per symbol
 *  rs->nn = (2^rs->mm) - 1
 *
 *  Calculate (x % rs->nn), without using a div instruction
*/
static inline int rs_modnn(struct rs_codec *rs, int x)
{
	while (x >= rs->nn) {
		x -= rs->nn;
		x = (x >> rs->mm) + (x & rs->nn);
	}
	return x;
}

/**
 * rs_modnn_mul() - Modulo replacement for galois field arithmetics
 *
 *  @rs:	Pointer to the RS codec
 *  @a:		a*b is the value to reduce (requires 0 <= a <= nn <= 0xFFFF)
 *  @b:		a*b is the value to reduce (requires 0 <= b <= nn <= 0xFFFF)
 *
 *  Same as rs_modnn(a*b), but avoid integer overflow when calculating a*b
*/
static inline int rs_modnn_mul(struct rs_codec *rs, int a, int b)
{
	/* nn <= 0xFFFF, so (a * b) will not overflow uint32_t */
	uint32_t x = (uint32_t)a * (uint32_t)b;
	uint32_t nn = (uint32_t)rs->nn;
	while (x >= nn) {
		x -= nn;
		x = (x >> rs->mm) + (x & nn);
	}
	return (int)x;
}

/**
 * rs_modnn_fast() - Modulo replacement for galois field arithmetics
 *
 *  @rs:	Pointer to the RS codec
 *  @x:		the value to reduce (requires 0 <= x < 2*nn)
 *
 *  Same as rs_modnn(x), but faster, at the cost of limited value range of @x
*/
static inline int rs_modnn_fast(struct rs_codec *rs, int x)
{
	return x - rs->nn < 0 ? x : x - rs->nn;
}

#endif
