// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the random clientid, session ID, and COPY_NOTIFY
 * stateid generators in nfs4state.c. Included at the end of that file
 * so the tests reach its static helpers. KUNIT_STATIC_STUB_REDIRECT()
 * replaces a random source only in the KUnit task, so the tests call
 * the helpers directly rather than driving them over the wire.
 */
#include <kunit/test.h>
#include <linux/xarray.h>

/*
 * Script for nfsd4_get_random_u32(). Past the script's end the last
 * value repeats, so a one-value script models a source stuck on one
 * value.
 */
static u32 nfsd_clid_test_rng[NFSD4_RANDOM_RETRIES + 2];
static unsigned int nfsd_clid_test_rng_len;
static unsigned int nfsd_clid_test_rng_pos;
static unsigned int nfsd_clid_test_rng_calls;

static u32 nfsd_clid_test_random_u32(void)
{
	nfsd_clid_test_rng_calls++;
	if (nfsd_clid_test_rng_pos < nfsd_clid_test_rng_len)
		return nfsd_clid_test_rng[nfsd_clid_test_rng_pos++];
	return nfsd_clid_test_rng_len ?
		nfsd_clid_test_rng[nfsd_clid_test_rng_len - 1] : 0;
}

static void nfsd_clid_test_script(const u32 *vals, unsigned int n)
{
	unsigned int i;

	n = min_t(unsigned int, n, ARRAY_SIZE(nfsd_clid_test_rng));
	for (i = 0; i < n; i++)
		nfsd_clid_test_rng[i] = vals[i];
	nfsd_clid_test_rng_len = n;
	nfsd_clid_test_rng_pos = 0;
	nfsd_clid_test_rng_calls = 0;
}

/* Script for nfsd4_get_random_u64(), repeating its last value the same way. */
static u64 nfsd_sid_test_rng[NFSD4_RANDOM_RETRIES + 2];
static unsigned int nfsd_sid_test_rng_len;
static unsigned int nfsd_sid_test_rng_pos;
static unsigned int nfsd_sid_test_rng_calls;

static u64 nfsd_sid_test_random_u64(void)
{
	nfsd_sid_test_rng_calls++;
	if (nfsd_sid_test_rng_pos < nfsd_sid_test_rng_len)
		return nfsd_sid_test_rng[nfsd_sid_test_rng_pos++];
	return nfsd_sid_test_rng_len ?
		nfsd_sid_test_rng[nfsd_sid_test_rng_len - 1] : 0;
}

static void nfsd_sid_test_script(const u64 *vals, unsigned int n)
{
	unsigned int i;

	n = min_t(unsigned int, n, ARRAY_SIZE(nfsd_sid_test_rng));
	for (i = 0; i < n; i++)
		nfsd_sid_test_rng[i] = vals[i];
	nfsd_sid_test_rng_len = n;
	nfsd_sid_test_rng_pos = 0;
	nfsd_sid_test_rng_calls = 0;
}

static struct nfsd_net *nfsd_clid_test_net(struct kunit *test)
{
	struct nfsd_net *nn;
	unsigned int i;

	nn = kunit_kzalloc(test, sizeof(*nn), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, nn);
	nn->conf_id_hashtbl = kunit_kmalloc_array(test, CLIENT_HASH_SIZE,
						  sizeof(struct list_head),
						  GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, nn->conf_id_hashtbl);
	nn->unconf_id_hashtbl = kunit_kmalloc_array(test, CLIENT_HASH_SIZE,
						    sizeof(struct list_head),
						    GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, nn->unconf_id_hashtbl);
	for (i = 0; i < CLIENT_HASH_SIZE; i++) {
		INIT_LIST_HEAD(&nn->conf_id_hashtbl[i]);
		INIT_LIST_HEAD(&nn->unconf_id_hashtbl[i]);
	}
	spin_lock_init(&nn->client_lock);
	return nn;
}

static struct nfs4_client *nfsd_clid_test_client(struct kunit *test,
						 struct list_head *tbl, u32 id)
{
	struct nfs4_client *clp;

	clp = kunit_kzalloc(test, sizeof(*clp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, clp);
	clp->cl_clientid.cl_id = id;
	list_add(&clp->cl_idhash, &tbl[clientid_hashval(id)]);
	return clp;
}

/*
 * cl_id_in_use_locked() finds a hashed client of any minor version or
 * expiry state, so no random id aliases one find_client_in_id_table()
 * skips.
 */
static void nfsd_clid_in_use_test(struct kunit *test)
{
	struct nfsd_net *nn = nfsd_clid_test_net(test);
	struct nfs4_client *expired, *other;
	bool conf, unconf, exp, minor, free;

	nfsd_clid_test_client(test, nn->conf_id_hashtbl, 0x1111);
	nfsd_clid_test_client(test, nn->unconf_id_hashtbl, 0x2222);
	expired = nfsd_clid_test_client(test, nn->conf_id_hashtbl, 0x3333);
	expired->cl_time = 0;
	other = nfsd_clid_test_client(test, nn->conf_id_hashtbl, 0x4444);
	other->cl_minorversion = 1;

	/* A failed expectation allocates with GFP_KERNEL, so check unlocked. */
	spin_lock(&nn->client_lock);
	conf = cl_id_in_use_locked(nn, 0x1111);
	unconf = cl_id_in_use_locked(nn, 0x2222);
	exp = cl_id_in_use_locked(nn, 0x3333);
	minor = cl_id_in_use_locked(nn, 0x4444);
	free = cl_id_in_use_locked(nn, 0x5555);
	spin_unlock(&nn->client_lock);

	KUNIT_EXPECT_TRUE(test, conf);
	KUNIT_EXPECT_TRUE(test, unconf);
	KUNIT_EXPECT_TRUE(test, exp);
	KUNIT_EXPECT_TRUE(test, minor);
	KUNIT_EXPECT_FALSE(test, free);
}

/*
 * The COPY_NOTIFY bit is cleared from a random clientid without a retry,
 * so no clientid can equal a token's cl_id word.
 */
static void nfsd_clid_mask_test(struct kunit *test)
{
	struct nfsd_net *nn = nfsd_clid_test_net(test);
	struct nfs4_client *new;
	static const u32 seq[] = { 0x1234 | NFSD4_CPNTF_CLID_BIT };
	__be32 status;

	new = kunit_kzalloc(test, sizeof(*new), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, new);
	kunit_activate_static_stub(test, nfsd4_get_random_u32,
				   nfsd_clid_test_random_u32);
	nfsd_clid_test_script(seq, ARRAY_SIZE(seq));

	spin_lock(&nn->client_lock);
	status = nfsd4_assign_clid_locked(new, nn, NULL, NULL);
	spin_unlock(&nn->client_lock);

	KUNIT_EXPECT_TRUE(test, status == nfs_ok);
	KUNIT_EXPECT_EQ(test, new->cl_clientid.cl_id, 0x1234);
	KUNIT_EXPECT_EQ(test, nfsd_clid_test_rng_calls, 1);
}

static void nfsd_clid_collision_test(struct kunit *test)
{
	struct nfsd_net *nn = nfsd_clid_test_net(test);
	struct nfs4_client *new, *expired, *other;
	static const u32 seq[] = { 0xAAAA, 0xBBBB, 0xCCCC, 0xDDDD, 0xEEEE };
	__be32 status;

	nfsd_clid_test_client(test, nn->conf_id_hashtbl, 0xAAAA);
	nfsd_clid_test_client(test, nn->unconf_id_hashtbl, 0xBBBB);
	expired = nfsd_clid_test_client(test, nn->conf_id_hashtbl, 0xCCCC);
	expired->cl_time = 0;
	other = nfsd_clid_test_client(test, nn->conf_id_hashtbl, 0xDDDD);
	other->cl_minorversion = 1;

	new = kunit_kzalloc(test, sizeof(*new), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, new);
	kunit_activate_static_stub(test, nfsd4_get_random_u32,
				   nfsd_clid_test_random_u32);
	nfsd_clid_test_script(seq, ARRAY_SIZE(seq));

	spin_lock(&nn->client_lock);
	status = nfsd4_assign_clid_locked(new, nn, NULL, NULL);
	spin_unlock(&nn->client_lock);

	KUNIT_EXPECT_TRUE(test, status == nfs_ok);
	KUNIT_EXPECT_EQ(test, new->cl_clientid.cl_id, 0xEEEE);
	KUNIT_EXPECT_EQ(test, nfsd_clid_test_rng_calls, 5);
}

static void nfsd_clid_stuck_test(struct kunit *test)
{
	struct nfsd_net *nn = nfsd_clid_test_net(test);
	struct nfs4_client *new;
	static const u32 seq[] = { 0xCAFE };
	bool conf, unconf;
	__be32 status;

	nfsd_clid_test_client(test, nn->conf_id_hashtbl, 0xCAFE);
	nfsd_clid_test_client(test, nn->unconf_id_hashtbl, 0xBEEF);

	new = kunit_kzalloc(test, sizeof(*new), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, new);
	kunit_activate_static_stub(test, nfsd4_get_random_u32,
				   nfsd_clid_test_random_u32);
	nfsd_clid_test_script(seq, ARRAY_SIZE(seq));

	spin_lock(&nn->client_lock);
	status = nfsd4_assign_clid_locked(new, nn, NULL, NULL);
	conf = cl_id_in_use_locked(nn, 0xCAFE);
	unconf = cl_id_in_use_locked(nn, 0xBEEF);
	spin_unlock(&nn->client_lock);

	KUNIT_EXPECT_TRUE(test, status == nfserr_jukebox);
	KUNIT_EXPECT_EQ(test, new->cl_clientid.cl_id, 0);
	KUNIT_EXPECT_EQ(test, nfsd_clid_test_rng_calls, NFSD4_RANDOM_RETRIES);
	KUNIT_EXPECT_TRUE(test, conf);
	KUNIT_EXPECT_TRUE(test, unconf);
}

/*
 * A confirmed client with a matching verifier gets no random id. The
 * SETCLIENTID callback-update path copies the confirmed client's
 * clientid in copy_clid() instead.
 */
static void nfsd_clid_copy_clid_skip_test(struct kunit *test)
{
	struct nfsd_net *nn = nfsd_clid_test_net(test);
	struct nfs4_client *new, *conf;
	nfs4_verifier verf;
	__be32 status;

	conf = kunit_kzalloc(test, sizeof(*conf), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, conf);
	memset(conf->cl_verifier.data, 0x5a, sizeof(conf->cl_verifier.data));
	memcpy(verf.data, conf->cl_verifier.data, sizeof(verf.data));

	new = kunit_kzalloc(test, sizeof(*new), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, new);
	kunit_activate_static_stub(test, nfsd4_get_random_u32,
				   nfsd_clid_test_random_u32);
	nfsd_clid_test_script(NULL, 0);

	spin_lock(&nn->client_lock);
	status = nfsd4_assign_clid_locked(new, nn, conf, &verf);
	spin_unlock(&nn->client_lock);

	KUNIT_EXPECT_TRUE(test, status == nfs_ok);
	KUNIT_EXPECT_EQ(test, new->cl_clientid.cl_id, 0);
	KUNIT_EXPECT_EQ(test, nfsd_clid_test_rng_calls, 0);
}

/*
 * A boot_time an hour ahead of the clock forces the same-second branch
 * whatever the clock does during the test.
 */
static void nfsd_clid_boot_time_test(struct kunit *test)
{
	struct nfsd_net *nn = nfsd_clid_test_net(test);
	time64_t previous = ktime_get_real_seconds() + 3600;

	nn->boot_time = previous;

	KUNIT_EXPECT_EQ(test, nfsd4_next_boot_time(nn), previous + 1);
}

static void nfsd_cp_test_idr_destroy(void *ctx)
{
	idr_destroy(ctx);
}

static struct nfsd_net *nfsd_cp_test_net(struct kunit *test)
{
	struct nfsd_net *nn;

	nn = kunit_kzalloc(test, sizeof(*nn), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, nn);
	spin_lock_init(&nn->s2s_cp_lock);
	idr_init(&nn->s2s_cp_stateids);
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, nfsd_cp_test_idr_destroy,
						  &nn->s2s_cp_stateids));
	return nn;
}

static void nfsd_cp_word_test(struct kunit *test)
{
	struct nfsd_net *nn = nfsd_cp_test_net(test);
	static const u32 seq[] = { 0x1234, 0x5678 };
	struct nfs4_cpntf_state *first, *second;
	struct nfs4_stid *parent;

	parent = kunit_kzalloc(test, sizeof(*parent), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, parent);
	INIT_LIST_HEAD(&parent->sc_cp_list);
	first = kunit_kzalloc(test, sizeof(*first), GFP_KERNEL);
	second = kunit_kzalloc(test, sizeof(*second), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, first);
	KUNIT_ASSERT_NOT_NULL(test, second);
	INIT_LIST_HEAD(&first->cp_list);
	INIT_LIST_HEAD(&second->cp_list);

	kunit_activate_static_stub(test, nfsd4_get_random_u32,
				   nfsd_clid_test_random_u32);
	nfsd_clid_test_script(seq, ARRAY_SIZE(seq));
	KUNIT_EXPECT_EQ(test, nfs4_init_cp_state(nn, &first->cp_stateid,
						 parent), 1);
	KUNIT_EXPECT_EQ(test, nfs4_init_cp_state(nn, &second->cp_stateid,
						 parent), 1);

	KUNIT_EXPECT_EQ(test, nfsd_clid_test_rng_calls, 2);
	KUNIT_EXPECT_EQ(test, first->cp_stateid.cs_stid.si_opaque.so_clid.cl_id,
			0x1234 | NFSD4_CPNTF_CLID_BIT);
	KUNIT_EXPECT_EQ(test, second->cp_stateid.cs_stid.si_opaque.so_clid.cl_id,
			0x5678 | NFSD4_CPNTF_CLID_BIT);
	KUNIT_EXPECT_EQ(test, first->cp_stateid.cs_stid.si_opaque.so_id, 0);
	KUNIT_EXPECT_EQ(test, second->cp_stateid.cs_stid.si_opaque.so_id, 1);
	KUNIT_EXPECT_PTR_EQ(test, idr_find(&nn->s2s_cp_stateids, 1),
			    &second->cp_stateid);
}

static struct nfs4_cpntf_state *nfsd_cp_test_entry(struct kunit *test,
						   struct nfsd_net *nn,
						   struct list_head *parent,
						   const clientid_t *owner,
						   u32 key, u32 word)
{
	struct nfs4_cpntf_state *cps;
	u32 k = key;
	int ret;

	/* Not kunit_kzalloc(): a successful revoke frees the entry. */
	cps = kzalloc_obj(*cps);
	KUNIT_ASSERT_NOT_NULL(test, cps);
	cps->cp_p_clid = *owner;
	cps->cp_stateid.cs_stid.si_opaque.so_clid.cl_boot = (u32)nn->boot_time;
	cps->cp_stateid.cs_stid.si_opaque.so_clid.cl_id = word;
	cps->cp_stateid.cs_stid.si_opaque.so_id = key;
	cps->cp_stateid.cs_type = NFS4_COPYNOTIFY_STID;
	refcount_set(&cps->cp_stateid.cs_count, 1);
	list_add(&cps->cp_list, parent);
	ret = idr_alloc_u32(&nn->s2s_cp_stateids, &cps->cp_stateid, &k, k,
			    GFP_KERNEL);
	if (ret) {
		list_del(&cps->cp_list);
		kfree(cps);
	}
	KUNIT_ASSERT_EQ(test, 0, ret);
	return cps;
}

/*
 * A NULL @clp is the READ/SEEK form, which accepts any holder of a
 * matching token, not only the creator. The creator's @clp is the
 * OFFLOAD_CANCEL form. The checks run in an order that keeps the entry
 * alive until the final revoke.
 */
static void nfsd_cp_token_check_test(struct kunit *test)
{
	struct nfsd_net *nn = nfsd_cp_test_net(test);
	clientid_t a = { .cl_boot = 1000, .cl_id = 0xA },
		   b = { .cl_boot = 1000, .cl_id = 0xB };
	struct nfs4_client *clp_a, *clp_b;
	struct nfs4_cpntf_state *cps, *found = NULL;
	LIST_HEAD(parent);
	stateid_t st = { };
	__be32 bad_word, bad_boot, wrong_client, read_form, cancel_form;
	void *after;

	nn->boot_time = 1000;
	clp_a = kunit_kzalloc(test, sizeof(*clp_a), GFP_KERNEL);
	clp_b = kunit_kzalloc(test, sizeof(*clp_b), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, clp_a);
	KUNIT_ASSERT_NOT_NULL(test, clp_b);
	clp_a->cl_clientid = a;
	clp_b->cl_clientid = b;

	cps = nfsd_cp_test_entry(test, nn, &parent, &a, 0x4321, 0xC0FFEE);
	st.si_opaque.so_clid.cl_boot = (u32)nn->boot_time;
	st.si_opaque.so_clid.cl_id = 0xC0FFEE;
	st.si_opaque.so_id = 0x4321;

	st.si_opaque.so_clid.cl_id = 0xC0FFEE + 1;
	bad_word = manage_cpntf_state(nn, &st, NULL, &found);
	st.si_opaque.so_clid.cl_id = 0xC0FFEE;

	st.si_opaque.so_clid.cl_boot = (u32)nn->boot_time + 1;
	bad_boot = manage_cpntf_state(nn, &st, clp_a, &found);
	st.si_opaque.so_clid.cl_boot = (u32)nn->boot_time;

	wrong_client = manage_cpntf_state(nn, &st, clp_b, &found);
	KUNIT_EXPECT_NULL(test, found);

	read_form = manage_cpntf_state(nn, &st, NULL, &found);
	KUNIT_EXPECT_PTR_EQ(test, found, cps);
	KUNIT_EXPECT_EQ(test, refcount_read(&cps->cp_stateid.cs_count), 2);
	if (read_form == nfs_ok)
		refcount_dec(&cps->cp_stateid.cs_count);

	cancel_form = manage_cpntf_state(nn, &st, clp_a, &found);
	after = idr_find(&nn->s2s_cp_stateids, 0x4321);

	KUNIT_EXPECT_TRUE(test, bad_word == nfserr_bad_stateid);
	KUNIT_EXPECT_TRUE(test, bad_boot == nfserr_bad_stateid);
	KUNIT_EXPECT_TRUE(test, wrong_client == nfserr_bad_stateid);
	KUNIT_EXPECT_TRUE(test, read_form == nfs_ok);
	KUNIT_EXPECT_TRUE(test, cancel_form == nfs_ok);
	KUNIT_EXPECT_NULL(test, after);

	/* A failed cancel leaves the entry in place, so free it here. */
	if (after) {
		idr_remove(&nn->s2s_cp_stateids, 0x4321);
		kfree(cps);
	}
}

static void nfsd_so_id_free_stid(struct nfs4_client *cl, struct kmem_cache *slab,
				 struct nfs4_stid *stid)
{
	spin_lock(&cl->cl_lock);
	idr_remove(&cl->cl_stateids, stid->sc_stateid.si_opaque.so_id);
	spin_unlock(&cl->cl_lock);
	kmem_cache_free(slab, stid);
}

static void nfsd_so_id_destroy_slab(void *ctx)
{
	kmem_cache_destroy(ctx);
}

static void nfsd_so_id_destroy_xa(void *ctx)
{
	xa_destroy(ctx);
}

/*
 * nfs4_alloc_stid() hands out so_id values cyclically, so an id freed
 * a moment ago is not handed out again.
 */
#define NFSD_SO_ID_TEST_COUNT	4096

static void nfsd_so_id_nonreuse_test(struct kunit *test)
{
	struct kmem_cache *slab;
	struct nfs4_client *cl;
	struct xarray *seen;
	unsigned int i;
	int err;

	cl = kunit_kzalloc(test, sizeof(*cl), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, cl);
	spin_lock_init(&cl->cl_lock);
	idr_init(&cl->cl_stateids);
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, nfsd_cp_test_idr_destroy,
						  &cl->cl_stateids));
	slab = kmem_cache_create("nfsd_test_stid", sizeof(struct nfs4_stid), 0,
				 0, NULL);
	KUNIT_ASSERT_NOT_NULL(test, slab);
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, nfsd_so_id_destroy_slab,
						  slab));
	/* Cleanup runs on another kthread, so @seen cannot live on this stack. */
	seen = kunit_kzalloc(test, sizeof(*seen), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, seen);
	xa_init(seen);
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, nfsd_so_id_destroy_xa,
						  seen));

	for (i = 0; i < NFSD_SO_ID_TEST_COUNT; i++) {
		struct nfs4_stid *stid = nfs4_alloc_stid(cl, slab, NULL);

		KUNIT_ASSERT_NOT_NULL(test, stid);
		err = xa_insert(seen, stid->sc_stateid.si_opaque.so_id,
				xa_mk_value(1), GFP_KERNEL);
		nfsd_so_id_free_stid(cl, slab, stid);
		KUNIT_ASSERT_EQ(test, err, 0);
		if (!(i & 1023))
			cond_resched();
	}
}

/*
 * nfsd4_assign_sessionid_locked() takes a struct net, so build one whose
 * net_generic() slot for nfsd points at the bare nfsd_net.
 */
static struct net *nfsd_sid_test_net(struct kunit *test, struct nfsd_net **nnp)
{
	struct net_generic *ng;
	struct nfsd_net *nn;
	struct net *net;
	unsigned int i;

	net = kunit_kzalloc(test, sizeof(*net), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, net);
	ng = kunit_kzalloc(test, struct_size(ng, ptr, nfsd_net_id + 1),
			   GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ng);
	nn = kunit_kzalloc(test, sizeof(*nn), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, nn);
	nn->sessionid_hashtbl = kunit_kmalloc_array(test, SESSION_HASH_SIZE,
						    sizeof(struct list_head),
						    GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, nn->sessionid_hashtbl);
	for (i = 0; i < SESSION_HASH_SIZE; i++)
		INIT_LIST_HEAD(&nn->sessionid_hashtbl[i]);
	spin_lock_init(&nn->client_lock);
	ng->ptr[nfsd_net_id] = nn;
	rcu_assign_pointer(net->gen, ng);
	*nnp = nn;
	return net;
}

static unsigned int nfsd_sid_test_count(struct nfsd_net *nn)
{
	struct nfsd4_session *se;
	unsigned int i, n = 0;

	for (i = 0; i < SESSION_HASH_SIZE; i++)
		list_for_each_entry(se, &nn->sessionid_hashtbl[i], se_hash)
			n++;
	return n;
}

static void nfsd_sid_test(struct kunit *test)
{
	static const u64 taken = 0x1122334455667788ULL;
	static const u64 seq[] = { taken, 0x0102030405060708ULL };
	static const u64 stuck[] = { taken };
	const clientid_t clid = { .cl_boot = 7, .cl_id = 0x99 };
	struct nfsd4_sessionid *sid;
	struct nfsd4_session *se;
	struct nfsd_net *nn;
	struct net *net;
	__be32 status, stuck_status;
	u64 rand = 0, stuck_rand = 0;
	unsigned int before, after;

	net = nfsd_sid_test_net(test, &nn);
	se = kunit_kzalloc(test, sizeof(*se), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, se);
	sid = (struct nfsd4_sessionid *)se->se_sessionid.data;
	sid->clientid = clid;
	sid->sequence = lower_32_bits(taken);
	sid->reserved = upper_32_bits(taken);
	list_add(&se->se_hash, &nn->sessionid_hashtbl[sid->sequence %
						      SESSION_HASH_SIZE]);

	kunit_activate_static_stub(test, nfsd4_get_random_u64,
				   nfsd_sid_test_random_u64);
	nfsd_sid_test_script(seq, ARRAY_SIZE(seq));
	spin_lock(&nn->client_lock);
	status = nfsd4_assign_sessionid_locked(net, &clid, &rand);
	spin_unlock(&nn->client_lock);

	KUNIT_EXPECT_TRUE(test, status == nfs_ok);
	KUNIT_EXPECT_EQ(test, rand, seq[1]);
	KUNIT_EXPECT_EQ(test, nfsd_sid_test_rng_calls, 2);

	nfsd_sid_test_script(stuck, ARRAY_SIZE(stuck));
	before = nfsd_sid_test_count(nn);
	spin_lock(&nn->client_lock);
	stuck_status = nfsd4_assign_sessionid_locked(net, &clid, &stuck_rand);
	spin_unlock(&nn->client_lock);
	after = nfsd_sid_test_count(nn);

	KUNIT_EXPECT_TRUE(test, stuck_status == nfserr_jukebox);
	KUNIT_EXPECT_EQ(test, stuck_rand, 0);
	KUNIT_EXPECT_EQ(test, nfsd_sid_test_rng_calls, NFSD4_RANDOM_RETRIES);
	KUNIT_EXPECT_EQ(test, before, after);
}

static struct kunit_case nfsd_clid_test_cases[] = {
	KUNIT_CASE(nfsd_clid_in_use_test),
	KUNIT_CASE(nfsd_clid_mask_test),
	KUNIT_CASE(nfsd_clid_collision_test),
	KUNIT_CASE(nfsd_clid_stuck_test),
	KUNIT_CASE(nfsd_clid_copy_clid_skip_test),
	KUNIT_CASE(nfsd_clid_boot_time_test),
	KUNIT_CASE(nfsd_cp_word_test),
	KUNIT_CASE(nfsd_cp_token_check_test),
	KUNIT_CASE(nfsd_so_id_nonreuse_test),
	KUNIT_CASE(nfsd_sid_test),
	{}
};

static struct kunit_suite nfsd_clid_test_suite = {
	.name = "nfsd-clientid",
	.test_cases = nfsd_clid_test_cases,
};

kunit_test_suite(nfsd_clid_test_suite);
