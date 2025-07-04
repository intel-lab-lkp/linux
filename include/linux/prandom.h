/* SPDX-License-Identifier: GPL-2.0 */
/*
 * include/linux/prandom.h
 *
 * Include file for the fast pseudo-random 32-bit
 * generation.
 */
#ifndef _LINUX_PRANDOM_H
#define _LINUX_PRANDOM_H

#include <linux/types.h>
#include <linux/once.h>
#include <linux/percpu.h>
#include <linux/random.h>

struct rnd_state {
	__u64 s[4];
};

/* WARNING: this API MUST NOT be used for cryptographic purposes! */
u64 prandom_u64_state(struct rnd_state *state);
/* WARNING: this API MUST NOT be used for cryptographic purposes! */
u32 prandom_u32_state(struct rnd_state *state);
/* WARNING: this API MUST NOT be used for cryptographic purposes! */
void prandom_bytes_state(struct rnd_state *state, void *buf, size_t nbytes);
void prandom_seed_full_state(struct rnd_state __percpu *pcpu_state);
void prandom_seed_state(struct rnd_state *state, u64 seed);

#define prandom_init_once(pcpu_state)			\
	DO_ONCE(prandom_seed_full_state, (pcpu_state))

#endif
