// SPDX-License-Identifier: BSD-2-Clause

/* This file contains miscellaneous utility functions for Homa, such
 * as initializing and destroying homa structs.
 */

#include "homa_impl.h"
#include "homa_peer.h"
#include "homa_rpc.h"
#include "homa_stub.h"

struct completion homa_pacer_kthread_done;

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

	_Static_assert(HOMA_MAX_PRIORITIES >= 8,
		       "homa_init assumes at least 8 priority levels");

	homa->pacer_kthread = NULL;
	init_completion(&homa_pacer_kthread_done);
	atomic64_set(&homa->next_outgoing_id, 2);
	atomic64_set(&homa->link_idle_time, get_cycles());
	spin_lock_init(&homa->pacer_mutex);
	homa->pacer_fifo_fraction = 50;
	homa->pacer_fifo_count = 1;
	homa->pacer_wake_time = 0;
	spin_lock_init(&homa->throttle_lock);
	INIT_LIST_HEAD_RCU(&homa->throttled_rpcs);
	homa->throttle_add = 0;
	homa->throttle_min_bytes = 200;
	homa->next_client_port = HOMA_MIN_DEFAULT_PORT;
	homa->port_map = kmalloc(sizeof(*homa->port_map), GFP_KERNEL);
	homa_socktab_init(homa->port_map);
	homa->peers = kmalloc(sizeof(*homa->peers), GFP_KERNEL);
	err = homa_peertab_init(homa->peers);
	if (err) {
		pr_err("Couldn't initialize peer table (errno %d)\n", -err);
		return err;
	}

	/* Wild guesses to initialize configuration values... */
	homa->unsched_bytes = 40000;
	homa->window_param = 100000;
	homa->link_mbps = 25000;
	homa->fifo_grant_increment = 10000;
	homa->grant_fifo_fraction = 50;
	homa->max_overcommit = 8;
	homa->max_incoming = 400000;
	homa->max_rpcs_per_peer = 1;
	homa->resend_ticks = 5;
	homa->resend_interval = 5;
	homa->timeout_ticks = 100;
	homa->timeout_resends = 5;
	homa->request_ack_ticks = 2;
	homa->reap_limit = 10;
	homa->dead_buffs_limit = 5000;
	homa->max_dead_buffs = 0;
	homa->pacer_kthread = kthread_run(homa_pacer_main, homa,
					  "homa_pacer");
	if (IS_ERR(homa->pacer_kthread)) {
		err = PTR_ERR(homa->pacer_kthread);
		homa->pacer_kthread = NULL;
		pr_err("couldn't create homa pacer thread: error %d\n", err);
		return err;
	}
	homa->pacer_exit = false;
	homa->max_nic_queue_ns = 5000;
	homa->cycles_per_kbyte = 0;
	homa->max_gso_size = 10000;
	homa->gso_force_software = 0;
	homa->max_gro_skbs = 20;
	homa->gro_policy = HOMA_GRO_NORMAL;
	homa->busy_usecs = 100;
	homa->gro_busy_usecs = 5;
	homa->timer_ticks = 0;
	homa->flags = 0;
	homa->bpage_lease_usecs = 10000;
	homa->next_id = 0;
	homa_outgoing_sysctl_changed(homa);
	homa_incoming_sysctl_changed(homa);
	return 0;
}

/**
 * homa_destroy() -  Destructor for homa objects.
 * @homa:      Object to destroy.
 */
void homa_destroy(struct homa *homa)
{
	if (homa->pacer_kthread) {
		homa_pacer_stop(homa);
		wait_for_completion(&homa_pacer_kthread_done);
	}

	/* The order of the following 2 statements matters! */
	homa_socktab_destroy(homa->port_map);
	kfree(homa->port_map);
	homa->port_map = NULL;
	homa_peertab_destroy(homa->peers);
	kfree(homa->peers);
	homa->peers = NULL;
}

/**
 * homa_spin() - Delay (without sleeping) for a given time interval.
 * @ns:   How long to delay (in nanoseconds)
 */
void homa_spin(int ns)
{
	__u64 end;

	end = get_cycles() + (ns * cpu_khz) / 1000000;
	while (get_cycles() < end)
		/* Empty loop body.*/
		;
}

/**
 * homa_throttle_lock_slow() - This function implements the slow path for
 * acquiring the throttle lock. It is invoked when the lock isn't immediately
 * available. It waits for the lock, but also records statistics about
 * the waiting time.
 * @homa:    Overall data about the Homa protocol implementation.
 */
void homa_throttle_lock_slow(struct homa *homa)
{
	spin_lock_bh(&homa->throttle_lock);
}
