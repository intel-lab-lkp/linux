// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for vli_mod_inv() and vli_mod_inv_vartime() in crypto/ecc.c.
 *
 * Both inverses are checked against known answers computed independently
 * (Python pow(x, -1, m)) and, on pseudo-random inputs, against the
 * defining property: result < m and input * result == 1 (mod m), using
 * vli_mod_mult_slow(). On the same random inputs the two functions must
 * also return identical results. Every curve the kernel inverts
 * over is covered (NIST P-192/256/384/521 and the EC-RDSA GOST curves),
 * each with both its field prime p and its group order n, since callers
 * invert modulo both.
 */
#include <kunit/test.h>
#include <linux/module.h>
#include <crypto/ecc_curve.h>
#include <crypto/ecdh.h>
#include <crypto/internal/ecc.h>

#include "ecrdsa_defs.h"

struct ecc_inv_modulus {
	const char *name;
	unsigned int curve_id;			/* NIST: via ecc_get_curve() */
	const struct ecc_curve *curve;		/* GOST: from ecrdsa_defs.h */
	bool use_n;
};

static const struct ecc_inv_modulus ecc_inv_moduli[] = {
	{ .name = "P-192 p", .curve_id = ECC_CURVE_NIST_P192, .use_n = false },
	{ .name = "P-192 n", .curve_id = ECC_CURVE_NIST_P192, .use_n = true },
	{ .name = "P-256 p", .curve_id = ECC_CURVE_NIST_P256, .use_n = false },
	{ .name = "P-256 n", .curve_id = ECC_CURVE_NIST_P256, .use_n = true },
	{ .name = "P-384 p", .curve_id = ECC_CURVE_NIST_P384, .use_n = false },
	{ .name = "P-384 n", .curve_id = ECC_CURVE_NIST_P384, .use_n = true },
	{ .name = "P-521 p", .curve_id = ECC_CURVE_NIST_P521, .use_n = false },
	{ .name = "P-521 n", .curve_id = ECC_CURVE_NIST_P521, .use_n = true },
	{ .name = "cp256a p", .curve = &gost_cp256a, .use_n = false },
	{ .name = "cp256a n", .curve = &gost_cp256a, .use_n = true },
	{ .name = "cp256b p", .curve = &gost_cp256b, .use_n = false },
	{ .name = "cp256b n", .curve = &gost_cp256b, .use_n = true },
	{ .name = "cp256c p", .curve = &gost_cp256c, .use_n = false },
	{ .name = "cp256c n", .curve = &gost_cp256c, .use_n = true },
	{ .name = "tc512a p", .curve = &gost_tc512a, .use_n = false },
	{ .name = "tc512a n", .curve = &gost_tc512a, .use_n = true },
	{ .name = "tc512b p", .curve = &gost_tc512b, .use_n = false },
	{ .name = "tc512b n", .curve = &gost_tc512b, .use_n = true },
};

static void ecc_inv_modulus_desc(const struct ecc_inv_modulus *m, char *desc)
{
	strscpy(desc, m->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(ecc_inv_modulus, ecc_inv_moduli, ecc_inv_modulus_desc);

struct ecc_inv_kat {
	struct ecc_inv_modulus mod;
	unsigned int ndigits;
	u64 input[ECC_MAX_DIGITS];
	u64 expected[ECC_MAX_DIGITS];
};

/*
 * Generated with Python: x = SHA-512 chain of "<curve>-<p|n>-<i>" mod m,
 * expected = pow(x, -1, m). Digits are little-endian, as in struct ecc_curve.
 */
static const struct ecc_inv_kat ecc_inv_kats[] = {
	{
		.mod = { .name = "P-192 p", .curve_id = ECC_CURVE_NIST_P192, .use_n = false },
		.ndigits = 3,
		.input = {
			0x41313653ac057a56ull, 0x142ad830dbd13605ull, 0x07dd616c2ad122beull,
		},
		.expected = {
			0x84bb5ade94eeb5b5ull, 0x46ef882aacf5d705ull, 0x0b000ae2c4bb67d6ull,
		},
	},
	{
		.mod = { .name = "P-192 p", .curve_id = ECC_CURVE_NIST_P192, .use_n = false },
		.ndigits = 3,
		.input = {
			0x801f93867c016edcull, 0xdc98f8a5b189dab0ull, 0xffd6dbecbdc4bbc9ull,
		},
		.expected = {
			0xef3b383f106b7746ull, 0x85a8c58c171b5df6ull, 0x2d43b03097118689ull,
		},
	},
	{
		.mod = { .name = "P-192 n", .curve_id = ECC_CURVE_NIST_P192, .use_n = true },
		.ndigits = 3,
		.input = {
			0xf75b1dbef5d78baeull, 0xf870edc7e8b09ff8ull, 0xd8e08a0e05d1a0e7ull,
		},
		.expected = {
			0xefac57e6f41e6b20ull, 0x90b13c8947aa298cull, 0xb99981827d0a2869ull,
		},
	},
	{
		.mod = { .name = "P-192 n", .curve_id = ECC_CURVE_NIST_P192, .use_n = true },
		.ndigits = 3,
		.input = {
			0xe04864a7f2b13e4aull, 0xb19e8eefd67f383dull, 0x02392ce4c1b41af8ull,
		},
		.expected = {
			0x4d5ab0715ac0b581ull, 0x19fbdf76ccb7672dull, 0xde00822edb6c4aeeull,
		},
	},
	{
		.mod = { .name = "P-256 p", .curve_id = ECC_CURVE_NIST_P256, .use_n = false },
		.ndigits = 4,
		.input = {
			0x6523829b0916044eull, 0x2e193a0987e6bf6bull, 0x51e4b9a3938c4315ull,
			0x053a04db5842fd37ull,
		},
		.expected = {
			0x9ea9af8228b3f038ull, 0x66d8326c69e2efe1ull, 0x510d33574ebe713eull,
			0xd8144a3619342c58ull,
		},
	},
	{
		.mod = { .name = "P-256 p", .curve_id = ECC_CURVE_NIST_P256, .use_n = false },
		.ndigits = 4,
		.input = {
			0xb37a68cd765ed66bull, 0xe8478b255a12858full, 0xd663ea1ff33742ddull,
			0x1eb3bb07b3ac4965ull,
		},
		.expected = {
			0xe3d1e8b0199a81f4ull, 0xd9cca403117232f0ull, 0xaf08dad13e21db23ull,
			0xb2bc323643233ad8ull,
		},
	},
	{
		.mod = { .name = "P-256 n", .curve_id = ECC_CURVE_NIST_P256, .use_n = true },
		.ndigits = 4,
		.input = {
			0xb67b047433438c5dull, 0xf4d3083f61d00519ull, 0x1eae1feca40dbffdull,
			0xc08a2a127a22dcceull,
		},
		.expected = {
			0x1a0946355ba9e712ull, 0x0b9ec57ebaab9841ull, 0x7243b59b0b7a1415ull,
			0xbcff341c41c4f8a2ull,
		},
	},
	{
		.mod = { .name = "P-256 n", .curve_id = ECC_CURVE_NIST_P256, .use_n = true },
		.ndigits = 4,
		.input = {
			0x4ff817671e33c11dull, 0x70f52cf5b3c60a75ull, 0x2e183e2bd91aa794ull,
			0x5a2df728a230d128ull,
		},
		.expected = {
			0x6842c388fe2f459eull, 0xf55bd97184c14cb2ull, 0x47887f23f851a1afull,
			0x6ddc4e41cde65424ull,
		},
	},
	{
		.mod = { .name = "P-384 p", .curve_id = ECC_CURVE_NIST_P384, .use_n = false },
		.ndigits = 6,
		.input = {
			0x554861bdb82c178aull, 0xa3638d45b5944a2dull, 0xb3bd1d4d3f2e7a70ull,
			0xdc949a2d25b48b08ull, 0x19baf1456da730f6ull, 0x79cf2a740d2f0adbull,
		},
		.expected = {
			0x5a98128c62eb9d11ull, 0x18e88df918b0ba21ull, 0x290c2fa3cfa1bed5ull,
			0x2382832c078eb462ull, 0x234af0afe619e114ull, 0xa06a10817512fa6cull,
		},
	},
	{
		.mod = { .name = "P-384 p", .curve_id = ECC_CURVE_NIST_P384, .use_n = false },
		.ndigits = 6,
		.input = {
			0x9ead6afd53dfd523ull, 0x10ade5f72542cdf4ull, 0xabd8ea28db92dadaull,
			0xc2fe7fb39c0a7dcbull, 0xa42858e6f3307f17ull, 0x3ce3fcf71b8662f5ull,
		},
		.expected = {
			0xfeb06845d2eb47d7ull, 0x28da3e2e76e99411ull, 0x7d3928538ea380beull,
			0x5faab5ea6d9003b8ull, 0x44bbcdae98ef6a9cull, 0x571db71a9d6b76eeull,
		},
	},
	{
		.mod = { .name = "P-384 n", .curve_id = ECC_CURVE_NIST_P384, .use_n = true },
		.ndigits = 6,
		.input = {
			0x2d87faf0ccdf628eull, 0x714f7dcce7709b5dull, 0xa2d9d8b6a9b0a488ull,
			0xed17e4e3600ce413ull, 0xc5b3933f9bb3e175ull, 0xbad9656902ab3e9cull,
		},
		.expected = {
			0x282ff4779f1c28d9ull, 0x4dc1a142880fe0a1ull, 0x23c24cfe518ffe6cull,
			0x98c775f094739137ull, 0xb4d0d7b0d2d114c3ull, 0xbc370ff4dbf33e60ull,
		},
	},
	{
		.mod = { .name = "P-384 n", .curve_id = ECC_CURVE_NIST_P384, .use_n = true },
		.ndigits = 6,
		.input = {
			0x094cf37530c6dd08ull, 0xaa016298335e9fcdull, 0x922d00b8ea119a94ull,
			0xe9d2e7151a1cecebull, 0xbde74e9f5df14560ull, 0x19616e9dcb1a757full,
		},
		.expected = {
			0x96e40db0d2da9a93ull, 0xfa98777c788840f2ull, 0x626994b74b623dc6ull,
			0x989991502aaba72full, 0xb9ba3491e12fc978ull, 0xc429548fd096bff6ull,
		},
	},
	{
		.mod = { .name = "P-521 p", .curve_id = ECC_CURVE_NIST_P521, .use_n = false },
		.ndigits = 9,
		.input = {
			0xfb735cb0eef9ffe4ull, 0xe15953c16210b4efull, 0x815eac8ebf15a34aull,
			0xb031a75a2aac033cull, 0x5ec6c676f0ec03bbull, 0x98e24304c243bb63ull,
			0xefaf94401896a141ull, 0xa858d39f6733000cull, 0x000000000000009eull,
		},
		.expected = {
			0x532b10dc88cc2e4full, 0x7a7eba9110c6047aull, 0xae4d8fdb3bec00d5ull,
			0x51a899b79cff4936ull, 0x25b72282fb76e7abull, 0x152ffb5fb46d1982ull,
			0xdae635c5790a849full, 0x0d78804330949b11ull, 0x00000000000000a3ull,
		},
	},
	{
		.mod = { .name = "P-521 p", .curve_id = ECC_CURVE_NIST_P521, .use_n = false },
		.ndigits = 9,
		.input = {
			0x5167bd279efb7d36ull, 0x7849d064452443ccull, 0x8927be45e25f0c90ull,
			0x02360631cecda1b2ull, 0x83badc286f1c0d4dull, 0x42ad82c752a0e78aull,
			0xdd91c23a7d9fcc71ull, 0x589a695382778fbbull, 0x0000000000000080ull,
		},
		.expected = {
			0xeb18d4f1444562e4ull, 0xb0e29ff965b7c2deull, 0x6bccc776f101177aull,
			0x200b46216fdf9fc6ull, 0x46cf5ef90af72440ull, 0x087887ad1d884c24ull,
			0x83887489b1475607ull, 0x2d26bdcf93b65023ull, 0x00000000000001edull,
		},
	},
	{
		.mod = { .name = "P-521 n", .curve_id = ECC_CURVE_NIST_P521, .use_n = true },
		.ndigits = 9,
		.input = {
			0x5da4484030a6b090ull, 0x8ff0250e68bace1bull, 0xd39fccf7abb7df64ull,
			0xded13ec201348ff4ull, 0x9db2769a87da6986ull, 0x1cc09104e46dc745ull,
			0xd561958ad922de9dull, 0xcdb60d0a7352049cull, 0x0000000000000132ull,
		},
		.expected = {
			0x36e436a0fa258cdbull, 0x1c0c1df0182e8bacull, 0xf656e689c42841a3ull,
			0x7462a99debc322f8ull, 0x94b643e1497da6adull, 0xde87ec7fa2ba4531ull,
			0xec17c7b58f8c13e4ull, 0xca738a6827129987ull, 0x00000000000000d4ull,
		},
	},
	{
		.mod = { .name = "P-521 n", .curve_id = ECC_CURVE_NIST_P521, .use_n = true },
		.ndigits = 9,
		.input = {
			0xfbc0c0ff8d43c1c1ull, 0x4d2470321f931883ull, 0x4518344488fafd9cull,
			0x29a0ab92cba1837bull, 0xc234b5931853da14ull, 0xa471f879bc985db8ull,
			0x81f7b041d98575e6ull, 0xfcf39ec0f6bab6c0ull, 0x000000000000019full,
		},
		.expected = {
			0xea311def0d876707ull, 0x59e00ddc3f36c625ull, 0xcae072b97e8cd382ull,
			0xe1e4c5c146a800b0ull, 0x0e91326379cb3161ull, 0x74a3f2a30648cedaull,
			0x68c5005af6a36f37ull, 0x4b9499c3670ed436ull, 0x0000000000000040ull,
		},
	},
	{
		.mod = { .name = "cp256a p", .curve = &gost_cp256a, .use_n = false },
		.ndigits = 4,
		.input = {
			0xce16f84d0d63e22full, 0x6f8cbf8eae192a59ull, 0xa505cfaec6c1672dull,
			0x322c8fd1a94154a6ull,
		},
		.expected = {
			0x38178b63653d43bfull, 0xbc1b939b9e739f3full, 0x55c236497fa605bfull,
			0xe9f32f9e76727d99ull,
		},
	},
	{
		.mod = { .name = "cp256a n", .curve = &gost_cp256a, .use_n = true },
		.ndigits = 4,
		.input = {
			0xb759cfc60aa26e77ull, 0xbb801ed5d60e7487ull, 0x8820b139878976a2ull,
			0x567b3983a6cf6d14ull,
		},
		.expected = {
			0xbb5c0e2ae3d0e8c5ull, 0x1c3f15fe4e8df779ull, 0x9f9258178d30d15eull,
			0x8ab0c3f798544f2aull,
		},
	},
	{
		.mod = { .name = "cp256b p", .curve = &gost_cp256b, .use_n = false },
		.ndigits = 4,
		.input = {
			0x4e2599d52eba613bull, 0xc3d0bdc47cb41448ull, 0xc033051690d10415ull,
			0x12436fddca16f1baull,
		},
		.expected = {
			0xffb11f38070173f1ull, 0x861b0af46d26bc0bull, 0x3eb235d38f85fc06ull,
			0x60151a884a64b20cull,
		},
	},
	{
		.mod = { .name = "cp256b n", .curve = &gost_cp256b, .use_n = true },
		.ndigits = 4,
		.input = {
			0xe1adabe1e455c2d4ull, 0x019b5270ca17ba4aull, 0xb79252223323ef13ull,
			0x31eabaa693675ee2ull,
		},
		.expected = {
			0xddb432fcd0b7c0d5ull, 0xf32b97d65bbcb5acull, 0x0dec14de06a3d3d5ull,
			0x136b3e1d8f17b062ull,
		},
	},
	{
		.mod = { .name = "cp256c p", .curve = &gost_cp256c, .use_n = false },
		.ndigits = 4,
		.input = {
			0x52731f91a04c0d1aull, 0x6bc2c9df3afa4640ull, 0x8578d8823a95f1e2ull,
			0x2907b4002c9143ebull,
		},
		.expected = {
			0xfd7e756e6bb4c542ull, 0x8688f3d1c9162e49ull, 0x3af990c585103c1aull,
			0x946b992cb30b5077ull,
		},
	},
	{
		.mod = { .name = "cp256c n", .curve = &gost_cp256c, .use_n = true },
		.ndigits = 4,
		.input = {
			0x04eaefc38d0a374eull, 0x481863d4541e39c3ull, 0xd1ae2e53b606631eull,
			0x26c4b32ff91608efull,
		},
		.expected = {
			0x4809bcd5dc3e28a1ull, 0x6c8d12ba33e87eeaull, 0xc00f19f241f0bbbcull,
			0x1419872f5f942363ull,
		},
	},
	{
		.mod = { .name = "tc512a p", .curve = &gost_tc512a, .use_n = false },
		.ndigits = 8,
		.input = {
			0x94a7a34e4ea393afull, 0xfd789a2892beb25bull, 0x05411d0dcd641399ull,
			0x79c05e32ce419729ull, 0x163eb6d6f1a05e9eull, 0x6816d9a6803c5c8cull,
			0xcb8fb6c546f5e058ull, 0xb7a346fb1ef6f26dull,
		},
		.expected = {
			0x8409998d9c8778c4ull, 0xf72a5eb92ffc2fb8ull, 0x5b7fc92002f0717eull,
			0x90fde815d13929d3ull, 0xba9df271b0bc45daull, 0x8f79350c3d211faaull,
			0x6821ea321fe52946ull, 0x6fdc937e1a5b664bull,
		},
	},
	{
		.mod = { .name = "tc512a n", .curve = &gost_tc512a, .use_n = true },
		.ndigits = 8,
		.input = {
			0x590d69e0666574ccull, 0x5903bb1718fa22bcull, 0xa6b2a1f0f811f300ull,
			0x4a2ae1e7a679b88dull, 0x95a0479a6072eaa7ull, 0xf8fa1df9a07bd49cull,
			0x2682c185b082f2aaull, 0x92087713b8be218bull,
		},
		.expected = {
			0xac1ca6f1a184d991ull, 0x3d45d71789e2594bull, 0x56fcbb412e2d120aull,
			0x8697b057dd9a1cacull, 0xefa889495de3f0fbull, 0x23d460c98f5fa379ull,
			0xf40f8636da09615bull, 0x267149ee428e9453ull,
		},
	},
	{
		.mod = { .name = "tc512b p", .curve = &gost_tc512b, .use_n = false },
		.ndigits = 8,
		.input = {
			0x37c146fc3154c930ull, 0x24dd35128dfb66feull, 0x7b848888b6033ef1ull,
			0xa8946ca1631265f9ull, 0xef8cae5498c9aaebull, 0x212b91ab415d30bcull,
			0x9772e4c91dcc372full, 0x29ce6a4ec759b075ull,
		},
		.expected = {
			0x05c000bf9c327561ull, 0x1693cc8097058928ull, 0xa77752791df33249ull,
			0x66d38526e131c363ull, 0xcfd69f2df80c552aull, 0xfebe86908032a1ccull,
			0x7afe502f5428790dull, 0x089c6c76c856a7f5ull,
		},
	},
	{
		.mod = { .name = "tc512b n", .curve = &gost_tc512b, .use_n = true },
		.ndigits = 8,
		.input = {
			0x93159e666b5c2440ull, 0x7d960868dfad959dull, 0xbc50d2cb26e2f801ull,
			0xfb30679a21a09ddfull, 0xe8af2a69a126f2d0ull, 0x92f5ad20e71ac074ull,
			0xfa0c47572bf1b86aull, 0x03cafc9d01211238ull,
		},
		.expected = {
			0x94714516b8ae1b15ull, 0xd518efd90c9ffe18ull, 0xc7e6875e9a36458dull,
			0x6597aaca768858f3ull, 0x5d7495310a466356ull, 0x3a482305545e6629ull,
			0x83939c133bd6666aull, 0x1c8686b6d67f8071ull,
		},
	},
};

/*
 * Look up the curve and pick p or n. Returns NULL if a NIST curve is
 * unavailable (P-192 in FIPS mode).
 */
static const u64 *ecc_inv_mod(const struct ecc_inv_modulus *m,
			      unsigned int *ndigits)
{
	const struct ecc_curve *curve = m->curve ?: ecc_get_curve(m->curve_id);

	if (!curve)
		return NULL;

	*ndigits = curve->g.ndigits;
	return m->use_n ? curve->n : curve->p;
}

static const u64 *ecc_inv_get_mod(struct kunit *test,
				  const struct ecc_inv_modulus *m,
				  unsigned int *ndigits)
{
	const u64 *mod = ecc_inv_mod(m, ndigits);

	if (!mod)
		kunit_skip(test, "%s not available (FIPS mode?)", m->name);

	return mod;
}

typedef void (*ecc_inv_fn)(u64 *result, const u64 *input, const u64 *mod,
			   unsigned int ndigits);

static void ecc_inv_run_kat(struct kunit *test, ecc_inv_fn inv)
{
	u64 result[ECC_MAX_DIGITS];
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ecc_inv_kats); i++) {
		const struct ecc_inv_kat *kat = &ecc_inv_kats[i];
		unsigned int ndigits;
		const u64 *mod = ecc_inv_mod(&kat->mod, &ndigits);

		if (!mod)
			continue;

		KUNIT_ASSERT_EQ(test, ndigits, kat->ndigits);
		inv(result, kat->input, mod, ndigits);
		KUNIT_EXPECT_MEMEQ_MSG(test, result, kat->expected,
				       ndigits * sizeof(u64),
				       "known-answer vector %u (%s)",
				       i, kat->mod.name);

		memcpy(result, kat->input, ndigits * sizeof(u64));
		inv(result, result, mod, ndigits);
		KUNIT_EXPECT_MEMEQ_MSG(test, result, kat->expected,
				       ndigits * sizeof(u64),
				       "in-place known-answer vector %u (%s)",
				       i, kat->mod.name);
	}
}

/* Edge cases: 0, 1, 2, m - 1 and m - 2 for every modulus. */
static void ecc_inv_run_edge(struct kunit *test, ecc_inv_fn inv)
{
	const struct ecc_inv_modulus *param = test->param_value;
	u64 x[ECC_MAX_DIGITS], result[ECC_MAX_DIGITS];
	u64 expected[ECC_MAX_DIGITS], prod[ECC_MAX_DIGITS];
	u64 one[ECC_MAX_DIGITS] = { 1 };
	unsigned int ndigits;
	const u64 *mod;

	mod = ecc_inv_get_mod(test, param, &ndigits);

	/* 0 has no inverse; the result is defined to be 0. */
	memset(x, 0, sizeof(x));
	memset(result, 0xff, sizeof(result));
	inv(result, x, mod, ndigits);
	KUNIT_EXPECT_TRUE_MSG(test, vli_is_zero(result, ndigits),
			      "inverse of 0 must be 0");

	/* 1^-1 = 1 */
	x[0] = 1;
	inv(result, x, mod, ndigits);
	KUNIT_EXPECT_MEMEQ(test, result, one, ndigits * sizeof(u64));

	/* 2^-1 = (m + 1) / 2: check via the defining property. */
	x[0] = 2;
	inv(result, x, mod, ndigits);
	vli_mod_mult_slow(prod, x, result, mod, ndigits);
	KUNIT_EXPECT_MEMEQ(test, prod, one, ndigits * sizeof(u64));

	/* (m - 1)^-1 = m - 1, since (-1)^-1 = -1 */
	memcpy(x, mod, ndigits * sizeof(u64));
	x[0] -= 1;	/* m is odd, so no borrow */
	memcpy(expected, x, ndigits * sizeof(u64));
	inv(result, x, mod, ndigits);
	KUNIT_EXPECT_MEMEQ(test, result, expected, ndigits * sizeof(u64));

	/* m - 2 = -2 */
	x[0] -= 1;
	inv(result, x, mod, ndigits);
	vli_mod_mult_slow(prod, x, result, mod, ndigits);
	KUNIT_EXPECT_MEMEQ(test, prod, one, ndigits * sizeof(u64));
}

#define ECC_INV_RANDOM_ITERS 200

static u64 ecc_inv_xorshift(u64 *state)
{
	u64 x = *state;

	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	*state = x;
	return x;
}

static u64 ecc_inv_seed(const struct ecc_inv_modulus *param)
{
	return 0x243f6a8885a308d3ull ^ (u64)(param - ecc_inv_moduli);
}

/* Pseudo-random input i in [1, m), from a fixed seed so failures reproduce. */
static void ecc_inv_random_input(u64 *x, const u64 *mod, unsigned int ndigits,
				 unsigned int i, u64 *state)
{
	unsigned int j;

	for (j = 0; j < ndigits; j++)
		x[j] = ecc_inv_xorshift(state);
	/* Every third input is small, to exercise short values. */
	if (i % 3 == 0)
		memset(&x[1], 0, (ndigits - 1) * sizeof(u64));
	while (vli_cmp(x, mod, ndigits) >= 0)
		x[ndigits - 1] >>= 1;
	if (vli_is_zero(x, ndigits))
		x[0] = 1;
}

/*
 * The result must be reduced (< m) and satisfy x * result == 1 (mod m).
 * The inverse modulo a prime is unique, so this pins down the answer.
 */
static void ecc_inv_run_random(struct kunit *test, ecc_inv_fn inv)
{
	const struct ecc_inv_modulus *param = test->param_value;
	u64 x[ECC_MAX_DIGITS], result[ECC_MAX_DIGITS];
	u64 prod[ECC_MAX_DIGITS];
	u64 one[ECC_MAX_DIGITS] = { 1 };
	u64 state = ecc_inv_seed(param);
	unsigned int ndigits, i;
	const u64 *mod;

	mod = ecc_inv_get_mod(test, param, &ndigits);

	for (i = 0; i < ECC_INV_RANDOM_ITERS; i++) {
		ecc_inv_random_input(x, mod, ndigits, i, &state);

		inv(result, x, mod, ndigits);
		KUNIT_ASSERT_LT_MSG(test, vli_cmp(result, mod, ndigits), 0,
				    "result not reduced, iteration %u", i);

		vli_mod_mult_slow(prod, x, result, mod, ndigits);
		KUNIT_ASSERT_MEMEQ_MSG(test, prod, one, ndigits * sizeof(u64),
				       "x * x^-1 != 1, iteration %u", i);
	}
}

static void ecc_inv_test_kat(struct kunit *test)
{
	ecc_inv_run_kat(test, vli_mod_inv);
}

static void ecc_inv_test_edge(struct kunit *test)
{
	ecc_inv_run_edge(test, vli_mod_inv);
}

static void ecc_inv_test_random(struct kunit *test)
{
	ecc_inv_run_random(test, vli_mod_inv);
}

static void ecc_inv_vartime_test_kat(struct kunit *test)
{
	ecc_inv_run_kat(test, vli_mod_inv_vartime);
}

static void ecc_inv_vartime_test_edge(struct kunit *test)
{
	ecc_inv_run_edge(test, vli_mod_inv_vartime);
}

static void ecc_inv_vartime_test_random(struct kunit *test)
{
	ecc_inv_run_random(test, vli_mod_inv_vartime);
}

/* On the random inputs, both functions must return identical results. */
static void ecc_inv_test_cross(struct kunit *test)
{
	const struct ecc_inv_modulus *param = test->param_value;
	u64 x[ECC_MAX_DIGITS], ct[ECC_MAX_DIGITS], vt[ECC_MAX_DIGITS];
	u64 state = ecc_inv_seed(param);
	unsigned int ndigits, i;
	const u64 *mod;

	mod = ecc_inv_get_mod(test, param, &ndigits);

	for (i = 0; i < ECC_INV_RANDOM_ITERS; i++) {
		ecc_inv_random_input(x, mod, ndigits, i, &state);

		vli_mod_inv(ct, x, mod, ndigits);
		vli_mod_inv_vartime(vt, x, mod, ndigits);
		KUNIT_ASSERT_MEMEQ_MSG(test, ct, vt, ndigits * sizeof(u64),
				       "vli_mod_inv() != vli_mod_inv_vartime(), iteration %u",
				       i);
	}
}

/*
 * An even modulus is rejected and the result is 0; the WARN is suppressed
 * so the log stays clean. Constant-time version only:
 * vli_mod_inv_vartime() requires an odd modulus and does not check it.
 */
static void ecc_inv_test_even_modulus(struct kunit *test)
{
	u64 mod[4] = { 0x10, 0, 0, 1 };
	u64 x[4] = { 3 };
	u64 result[4];

	memset(result, 0xff, sizeof(result));
	kunit_warning_suppress(test) {
		vli_mod_inv(result, x, mod, 4);
	}
	KUNIT_EXPECT_TRUE(test, vli_is_zero(result, 4));
}

static struct kunit_case ecc_inv_test_cases[] = {
	KUNIT_CASE(ecc_inv_test_kat),
	KUNIT_CASE_PARAM(ecc_inv_test_edge, ecc_inv_modulus_gen_params),
	KUNIT_CASE_PARAM(ecc_inv_test_random, ecc_inv_modulus_gen_params),
	KUNIT_CASE(ecc_inv_test_even_modulus),
	KUNIT_CASE(ecc_inv_vartime_test_kat),
	KUNIT_CASE_PARAM(ecc_inv_vartime_test_edge, ecc_inv_modulus_gen_params),
	KUNIT_CASE_PARAM(ecc_inv_vartime_test_random, ecc_inv_modulus_gen_params),
	KUNIT_CASE_PARAM(ecc_inv_test_cross, ecc_inv_modulus_gen_params),
	{}
};

static struct kunit_suite ecc_inv_test_suite = {
	.name = "ecc_mod_inv",
	.test_cases = ecc_inv_test_cases,
};

kunit_test_suite(ecc_inv_test_suite);

MODULE_DESCRIPTION("KUnit tests for ECC modular inversion");
MODULE_LICENSE("GPL");
