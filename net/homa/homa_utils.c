// SPDX-License-Identifier: BSD-2-Clause or GPL-2.0+

/* This file contains miscellaneous utility functions for Homa, such
 * as initializing and destroying homa structs.
 */

#include "homa_impl.h"
#include "homa_peer.h"
#include "homa_rpc.h"

#include "homa_stub.h"

/**
 * homa_init() - Constructor for homa objects.
 * @homa:   Object to initialize.
 *
 * Return:  0 on success, or a negative errno if there was an error. Even
 *          if an error occurs, it is safe (and necessary) to call
 *          homa_destroy at some point.
 */
int homa_init(struct homa *homa)
{
	int err;

	memset(homa, 0, sizeof(*homa));

	atomic64_set(&homa->next_outgoing_id, 2);
	homa->link_mbps = 25000;
	homa->peertab = homa_peer_alloc_peertab();
	if (IS_ERR(homa->peertab)) {
		err = PTR_ERR(homa->peertab);
		homa->peertab = NULL;
		return err;
	}
	homa->socktab = kmalloc(sizeof(*homa->socktab), GFP_KERNEL);
	if (!homa->socktab)
		return -ENOMEM;
	homa_socktab_init(homa->socktab);

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
 * @homa:      Object to destroy. It is safe if this object has already
 *             been previously destroyed.
 */
void homa_destroy(struct homa *homa)
{
	/* The order of the following cleanups matters! */
	if (homa->socktab) {
		homa_socktab_destroy(homa->socktab, NULL);
		kfree(homa->socktab);
		homa->socktab = NULL;
	}
	if (homa->peertab) {
		homa_peer_free_peertab(homa->peertab);
		homa->peertab = NULL;
	}
}

/**
 * homa_net_init() - Initialize a new struct homa_net as a per-net subsystem.
 * @hnet:    Struct to initialzie.
 * @net:     The network namespace the struct will be associated with.
 * @homa:    The main Homa data structure to use for the net.
 * Return:  0 on success, otherwise a negative errno.
 */
int homa_net_init(struct homa_net *hnet, struct net *net, struct homa *homa)
{
	memset(hnet, 0, sizeof(*hnet));
	hnet->homa = homa;
	hnet->prev_default_port = HOMA_MIN_DEFAULT_PORT - 1;
	return 0;
}

/**
 * homa_net_destroy() - Release any resources associated with a homa_net.
 * @hnet:    Object to destroy; must not be used again after this function
 *           returns.
 */
void homa_net_destroy(struct homa_net *hnet)
{
	homa_socktab_destroy(hnet->homa->socktab, hnet);
	homa_peer_free_net(hnet);
}

/**
 * homa_spin() - Delay (without sleeping) for a given time interval.
 * @ns:   How long to delay (in nanoseconds)
 */
void homa_spin(int ns)
{
	u64 end;

	end = homa_clock() + homa_ns_to_cycles(ns);
	while (homa_clock() < end)
		cpu_relax();
}
