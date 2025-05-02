// SPDX-License-Identifier: BSD-2-Clause

/* This file contains miscellaneous utility functions for Homa, such
 * as initializing and destroying homa structs.
 */

#include "homa_impl.h"
#include "homa_pacer.h"
#include "homa_peer.h"
#include "homa_rpc.h"
#include "homa_stub.h"

/**
 * homa_init() - Constructor for homa objects.
 * @homa:   Object to initialize.
 * @net:    Network namespace that @homa is associated with.
 *
 * Return:  0 on success, or a negative errno if there was an error. Even
 *          if an error occurs, it is safe (and necessary) to call
 *          homa_destroy at some point.
 */
int homa_init(struct homa *homa, struct net *net)
{
	int err;

	memset(homa, 0, sizeof(*homa));
	atomic64_set(&homa->next_outgoing_id, 2);
	homa->pacer = homa_pacer_new(homa, net);
	if (IS_ERR(homa->pacer)) {
		err = PTR_ERR(homa->pacer);
		homa->pacer = NULL;
		return err;
	}
	homa->prev_default_port = HOMA_MIN_DEFAULT_PORT - 1;
	homa->port_map = kmalloc(sizeof(*homa->port_map), GFP_KERNEL);
	if (!homa->port_map) {
		pr_err("%s couldn't create port_map: kmalloc failure",
		       __func__);
		return -ENOMEM;
	}
	homa_socktab_init(homa->port_map);
	homa->peers = kmalloc(sizeof(*homa->peers), GFP_KERNEL);
	if (!homa->peers) {
		pr_err("%s couldn't create peers: kmalloc failure", __func__);
		return -ENOMEM;
	}
	err = homa_peertab_init(homa->peers);
	if (err) {
		pr_err("%s couldn't initialize peer table (errno %d)\n",
		       __func__, -err);
		return err;
	}

	/* Wild guesses to initialize configuration values... */
	homa->resend_ticks = 5;
	homa->resend_interval = 5;
	homa->timeout_ticks = 100;
	homa->timeout_resends = 5;
	homa->request_ack_ticks = 2;
	homa->reap_limit = 10;
	homa->dead_buffs_limit = 5000;
	homa->max_gso_size = 10000;
	homa->wmem_max = 100000000;
	homa->bpage_lease_usecs = 10000;
	return 0;
}

/**
 * homa_destroy() -  Destructor for homa objects.
 * @homa:      Object to destroy.
 */
void homa_destroy(struct homa *homa)
{
	/* The order of the following statements matters! */
	if (homa->port_map) {
		homa_socktab_destroy(homa->port_map);
		kfree(homa->port_map);
		homa->port_map = NULL;
	}
	if (homa->pacer) {
		homa_pacer_destroy(homa->pacer);
		homa->pacer = NULL;
	}
	if (homa->peers) {
		homa_peertab_destroy(homa->peers);
		kfree(homa->peers);
		homa->peers = NULL;
	}
}

/**
 * homa_spin() - Delay (without sleeping) for a given time interval.
 * @ns:   How long to delay (in nanoseconds)
 */
void homa_spin(int ns)
{
	u64 end;

	end = sched_clock() + ns;
	while (sched_clock() < end)
		/* Empty loop body.*/
		;
}
