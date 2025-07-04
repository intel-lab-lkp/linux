// SPDX-License-Identifier: GPL-2.0
/*
 * WARNING: this API MUST NOT be used for cryptographic purposes!
 *
 * xoshiro256++ is a high-quality non-cryptographic
 * pseudorandom number generator (PRNG).
 *
 * For a more detailed description, see:
 * https://vigna.di.unimi.it/ftp/papers/ScrambledLinear.pdf
 *
 * Usage Advice: As the cryptographic random subsystem is really fast these days,
 * you should come up with a good reason, to introduce PRNG usage into new code.
 * Consider its usage, when a predictable/repeatable sequence is needed for
 * testing purposes. Prefer to use get_random_32(), when possible.
 *
 * Based on: https://prng.di.unimi.it/xoshiro256plusplus.c
 */

#include <linux/types.h>
#include <linux/percpu.h>
#include <linux/export.h>
#include <linux/jiffies.h>
#include <linux/prandom.h>
#include <linux/sched.h>
#include <linux/bitops.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

/**
 *	prandom_u64_state - seeded pseudo-random number generator.
 *	@state: pointer to state structure holding seeded state.
 *
 *	This is used for pseudo-randomness with no outside seeding.
 *	For more random results, use get_random_u64().
 *
 *	WARNING: this API MUST NOT be used for cryptographic purposes!
 */
u64 prandom_u64_state(struct rnd_state *state)
{
	const u64 result = rol64(state->s[0] + state->s[3], 23) + state->s[0];
	const u64 t = state->s[1] << 17;

	state->s[2] ^= state->s[0];
	state->s[3] ^= state->s[1];
	state->s[1] ^= state->s[2];
	state->s[0] ^= state->s[3];

	state->s[2] ^= t;

	state->s[3] = rol64(state->s[3], 45);

	return result;
}
EXPORT_SYMBOL(prandom_u64_state);

/**
 *	prandom_u32_state - seeded pseudo-random number generator.
 *	@state: pointer to state structure holding seeded state.
 *
 *	This is used for pseudo-randomness with no outside seeding.
 *	For more random results, use get_random_u32().
 *
 *	WARNING: this API MUST NOT be used for cryptographic purposes!
 */
u32 prandom_u32_state(struct rnd_state *state)
{
	return (u32) prandom_u64_state(state);
}
EXPORT_SYMBOL(prandom_u32_state);

/**
 *	prandom_bytes_state - get the requested number of pseudo-random bytes
 *
 *	@state: pointer to state structure holding seeded state.
 *	@buf: where to copy the pseudo-random bytes to
 *	@bytes: the requested number of bytes
 *
 *	This is used for pseudo-randomness with no outside seeding.
 *	For more random results, use get_random_bytes().
 *
 *	WARNING: this API MUST NOT be used for cryptographic purposes!
 */
void prandom_bytes_state(struct rnd_state *state, void *buf, size_t bytes)
{
	u8 *ptr = buf;

	while (bytes >= sizeof(u64)) {
		put_unaligned(prandom_u64_state(state), (u64 *) ptr);
		ptr += sizeof(u64);
		bytes -= sizeof(u64);
	}

	if (bytes > 0) {
		u64 rem = prandom_u64_state(state);
		do {
			*ptr++ = (u8) rem;
			bytes--;
			rem >>= BITS_PER_BYTE;
		} while (bytes > 0);
	}
}
EXPORT_SYMBOL(prandom_bytes_state);

/**
 * prandom_seed_state - set seed for prandom_u32_state().
 * @state: pointer to state structure to receive the seed.
 * @seed: arbitrary 64-bit value to use as a seed.
 *
 * splitmix64 init as suggested for xoshiro256++
 * See: https://prng.di.unimi.it/splitmix64.c
 */
void prandom_seed_state(struct rnd_state *state, u64 seed)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(state->s); ++i) {
		seed += 0x9e3779b97f4a7c15;
		u64 z = seed;
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
		z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
		state->s[i] = z ^ (z >> 31);
	}
}
EXPORT_SYMBOL(prandom_seed_state);

/**
 * prandom_seed_full_state - seed all related per-cpu states in pcpu_state.
 * @pcpu_state: pointer to states on all CPUs.
 */
void prandom_seed_full_state(struct rnd_state __percpu *pcpu_state)
{
	int i;

	for_each_possible_cpu(i) {
		struct rnd_state *state = per_cpu_ptr(pcpu_state, i);
		get_random_bytes(&state->s, sizeof(state->s));
	}
}
EXPORT_SYMBOL(prandom_seed_full_state);

#ifdef CONFIG_RANDOM32_SELFTEST
static struct prandom_test1 {
	u32 seed;
	u32 result;
} test1[] = {
	{ 1U, 1862517403U },
	{ 2U, 3049585706U },
	{ 3U, 3105450281U },
	{ 4U, 2527704881U },
};

static struct prandom_test2 {
	u32 seed;
	u32 iteration;
	u32 result;
} test2[] = {
	/* Test cases against Xoshiro256++, generated with reference impl. */
	{  931557656U, 959U, 2221272722U },
	{ 1339693295U, 876U,   12322103U },
	{ 1545556285U, 961U, 2793306339U },
	{  601730776U, 723U,  186699327U },
	{ 1027516047U, 687U, 3385354088U },
	{  416526298U, 700U, 3047662436U },
	{ 1395522032U, 652U,  370169503U },
	{  366221443U, 617U, 2468792816U },
	{ 1539836965U, 714U, 4178175423U },
	{  556206671U, 994U, 1935910425U },
	{  684907218U, 799U, 2892366361U },
	{ 2121230701U, 931U, 2395880533U },
	{ 1668516451U, 644U,  659315062U },
	{  768046066U, 883U,  729262650U },
	{ 1989159136U, 833U,   44268867U },
	{  536585145U, 996U, 3734292685U },
	{ 1008129373U, 642U, 3779097844U },
	{ 1740775604U, 939U,  958440770U },
	{ 1967883163U, 508U, 2766790334U },
	{ 1923019697U, 730U, 4188718967U },
	{  442079932U, 560U, 2658351430U },
	{ 1961302714U, 845U,  418725413U },
	{ 2030205964U, 962U, 3541720353U },
	{ 1160407529U, 507U,  155686916U },
	{  635482502U, 779U, 2954399934U },
	{ 1252788931U, 699U, 3195364074U },
	{ 1961817131U, 719U, 3860285454U },
	{ 1071468216U, 983U, 2558711391U },
	{ 1281848367U, 932U, 2429019683U },
	{  582537119U, 780U, 2251637575U },
	{ 1973672777U, 853U, 2686341687U },
	{ 1896756996U, 762U, 3853948474U },
	{  847917054U, 500U, 1352384361U },
	{ 1240520510U, 951U,  424294678U },
	{ 1685071682U, 567U, 1127358663U },
	{ 1516232129U, 557U, 2438163725U },
	{ 1208118903U, 612U,  494560814U },
	{ 1817269927U, 693U, 2296243096U },
	{ 1510091701U, 717U, 2522390997U },
	{  365916850U, 807U, 1704622356U },
	{  399324359U, 702U, 1692076319U },
	{ 1318480274U, 779U, 3132074148U },
	{  697758115U, 840U, 2298934293U },
	{ 1696507773U, 840U,  460426950U },
	{ 2081979121U, 981U, 2030259222U },
	{  955646687U, 742U,  849374769U },
	{ 1250683506U, 749U, 3793648395U },
	{  595003102U, 534U,  312261219U },
	{   47485338U, 558U, 1498074521U },
	{  619433479U, 610U, 3298978919U },
	{  704096520U, 518U, 1859635041U },
	{ 1712224984U, 606U, 2636341373U },
	{ 1318233152U, 922U, 4294083073U },
	{  855572992U, 761U, 1398432208U },
	{   64721421U, 703U, 2917659579U },
	{  678931758U, 840U, 2610263429U },
	{  692711973U, 778U, 2364471195U },
	{  677703619U, 530U, 4293322414U },
	{   92393223U, 586U, 2200038234U },
	{ 1222592920U, 743U, 3902404436U },
	{  358288986U, 695U, 4187968216U },
	{ 1935056945U, 958U, 2216763617U },
	{  735675993U, 990U, 3851267177U },
	{ 1560089402U, 897U, 3110095036U },
	{   70616361U, 829U, 2727884311U },
	{  368234700U, 731U, 1394533121U },
	{   20221190U, 879U, 2426527948U },
	{  539444654U, 682U, 3323050847U },
	{ 1314987297U, 840U, 1532766257U },
	{ 2019295544U, 645U, 2824742190U },
	{  469023838U, 716U, 2626662944U },
	{ 1843754496U, 653U, 3912965452U },
	{  400672036U, 809U,  794673667U },
	{  404722249U, 965U, 2590267245U },
	{  600702209U, 758U, 3742697327U },
	{  519953954U, 667U,  909153960U },
	{ 1658071126U, 694U, 1016441204U },
	{  420480037U, 749U, 4007274669U },
	{  690103647U, 969U, 2262636339U },
	{ 1029424799U, 937U, 1940659603U },
	{ 2012608669U, 506U,  239338087U },
	{ 1535432887U, 998U, 1889887018U },
	{ 1330635533U, 857U, 4029849650U },
	{ 1223800550U, 539U,  678829248U },
	{ 1322411537U, 680U, 2745139632U },
	{ 1877847898U, 945U,  153715577U },
	{ 1646356099U, 874U,  525516737U },
	{  805687536U, 744U, 1564399105U },
	{ 1948093210U, 633U,  507042967U },
	{  392609744U, 783U,   33573161U },
	{  690241304U, 770U, 1551691277U },
	{ 1360302965U, 696U,  391079011U },
	{ 1220090946U, 780U, 2587343386U },
	{  447092251U, 500U, 3335665101U },
	{ 1613868791U, 592U,  131974931U },
	{  523430951U, 548U, 3871675719U },
	{  726692899U, 810U, 2919536533U },
	{ 1364340021U, 836U, 1695810824U },
	{ 1986257729U, 931U,  550308813U },
	{  407983964U, 921U,  360202562U }
};

static int __init prandom_state_selftest(void)
{
	int i, j, errors = 0, runs = 0;
	bool error = false;

	for (i = 0; i < ARRAY_SIZE(test1); i++) {
		struct rnd_state state;

		prandom_seed_state(&state, test1[i].seed);

		if (test1[i].result != prandom_u32_state(&state))
			error = true;
	}

	if (error)
		pr_warn("prandom: seed boundary self test failed\n");
	else
		pr_info("prandom: seed boundary self test passed\n");

	for (i = 0; i < ARRAY_SIZE(test2); i++) {
		struct rnd_state state;

		prandom_seed_state(&state, test2[i].seed);

		for (j = 0; j < test2[i].iteration - 1; j++)
			prandom_u32_state(&state);

		if (test2[i].result != prandom_u32_state(&state))
			errors++;

		runs++;
		cond_resched();
	}

	if (errors)
		pr_warn("prandom: %d/%d self tests failed\n", errors, runs);
	else
		pr_info("prandom: %d self tests passed\n", runs);
	return 0;
}
core_initcall(prandom_state_selftest);
#endif
