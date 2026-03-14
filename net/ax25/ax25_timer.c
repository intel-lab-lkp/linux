// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 * Copyright (C) Alan Cox GW4PTS (alan@lxorguk.ukuu.org.uk)
 * Copyright (C) Jonathan Naylor G4KLX (g4klx@g4klx.demon.co.uk)
 * Copyright (C) Tomi Manninen OH2BNS (oh2bns@sral.fi)
 * Copyright (C) Darryl Miles G7LED (dlm@g7led.demon.co.uk)
 * Copyright (C) Joerg Reuter DL1BKE (jreuter@yaina.de)
 * Copyright (C) Frederic Rible F1OAT (frible@teaser.fr)
 * Copyright (C) 2002 Ralf Baechle DO1GRB (ralf@gnu.org)
 */
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <net/ax25.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <linux/uaccess.h>
#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/interrupt.h>

static void ax25_heartbeat_expiry(struct timer_list *);
static void ax25_t1timer_expiry(struct timer_list *);
static void ax25_t2timer_expiry(struct timer_list *);
static void ax25_t3timer_expiry(struct timer_list *);
static void ax25_idletimer_expiry(struct timer_list *);

static void ax25_mod_timer(struct timer_list *timer, ax25_cb *ax25,
			   unsigned long expires)
{
	if (!mod_timer(timer, expires))
		ax25_cb_hold(ax25);
}

static void ax25_del_timer(struct timer_list *timer, ax25_cb *ax25)
{
	if (timer_delete(timer))
		ax25_cb_put(ax25);
}

static void ax25_timer_expiry_start(ax25_cb *ax25)
{
	/*
	 * A pending timer holds one reference to the control block. Once the
	 * timer callback starts running that reference no longer belongs to a
	 * queued timer, so turn it into a temporary execution reference.
	 */
	ax25_cb_hold(ax25);
	ax25_cb_put(ax25);
}

static void ax25_timer_expiry_stop(ax25_cb *ax25)
{
	ax25_cb_put(ax25);
}

void ax25_setup_timers(ax25_cb *ax25)
{
	timer_setup(&ax25->timer, ax25_heartbeat_expiry, 0);
	timer_setup(&ax25->t1timer, ax25_t1timer_expiry, 0);
	timer_setup(&ax25->t2timer, ax25_t2timer_expiry, 0);
	timer_setup(&ax25->t3timer, ax25_t3timer_expiry, 0);
	timer_setup(&ax25->idletimer, ax25_idletimer_expiry, 0);
}

void ax25_start_heartbeat(ax25_cb *ax25)
{
	ax25_mod_timer(&ax25->timer, ax25, jiffies + 5 * HZ);
}

void ax25_start_t1timer(ax25_cb *ax25)
{
	ax25_mod_timer(&ax25->t1timer, ax25, jiffies + ax25->t1);
}

void ax25_start_t2timer(ax25_cb *ax25)
{
	ax25_mod_timer(&ax25->t2timer, ax25, jiffies + ax25->t2);
}

void ax25_start_t3timer(ax25_cb *ax25)
{
	if (ax25->t3 > 0)
		ax25_mod_timer(&ax25->t3timer, ax25,
			       jiffies + ax25->t3);
	else
		ax25_del_timer(&ax25->t3timer, ax25);
}

void ax25_start_idletimer(ax25_cb *ax25)
{
	if (ax25->idle > 0)
		ax25_mod_timer(&ax25->idletimer, ax25,
			       jiffies + ax25->idle);
	else
		ax25_del_timer(&ax25->idletimer, ax25);
}

void ax25_stop_heartbeat(ax25_cb *ax25)
{
	ax25_del_timer(&ax25->timer, ax25);
}

void ax25_stop_t1timer(ax25_cb *ax25)
{
	ax25_del_timer(&ax25->t1timer, ax25);
}

void ax25_stop_t2timer(ax25_cb *ax25)
{
	ax25_del_timer(&ax25->t2timer, ax25);
}

void ax25_stop_t3timer(ax25_cb *ax25)
{
	ax25_del_timer(&ax25->t3timer, ax25);
}

void ax25_stop_idletimer(ax25_cb *ax25)
{
	ax25_del_timer(&ax25->idletimer, ax25);
}

int ax25_t1timer_running(ax25_cb *ax25)
{
	return timer_pending(&ax25->t1timer);
}

unsigned long ax25_display_timer(struct timer_list *timer)
{
	long delta = timer->expires - jiffies;

	if (!timer_pending(timer))
		return 0;

	return max(0L, delta);
}

EXPORT_SYMBOL(ax25_display_timer);

static void ax25_heartbeat_expiry(struct timer_list *t)
{
	int proto = AX25_PROTO_STD_SIMPLEX;
	ax25_cb *ax25 = timer_container_of(ax25, t, timer);

	ax25_timer_expiry_start(ax25);

	if (ax25->ax25_dev)
		proto = ax25->ax25_dev->values[AX25_VALUES_PROTOCOL];

	switch (proto) {
	case AX25_PROTO_STD_SIMPLEX:
	case AX25_PROTO_STD_DUPLEX:
		ax25_std_heartbeat_expiry(ax25);
		break;

#ifdef CONFIG_AX25_DAMA_SLAVE
	case AX25_PROTO_DAMA_SLAVE:
		if (ax25->ax25_dev->dama.slave)
			ax25_ds_heartbeat_expiry(ax25);
		else
			ax25_std_heartbeat_expiry(ax25);
		break;
#endif
	}

	ax25_timer_expiry_stop(ax25);
}

static void ax25_t1timer_expiry(struct timer_list *t)
{
	ax25_cb *ax25 = timer_container_of(ax25, t, t1timer);

	ax25_timer_expiry_start(ax25);

	switch (ax25->ax25_dev->values[AX25_VALUES_PROTOCOL]) {
	case AX25_PROTO_STD_SIMPLEX:
	case AX25_PROTO_STD_DUPLEX:
		ax25_std_t1timer_expiry(ax25);
		break;

#ifdef CONFIG_AX25_DAMA_SLAVE
	case AX25_PROTO_DAMA_SLAVE:
		if (!ax25->ax25_dev->dama.slave)
			ax25_std_t1timer_expiry(ax25);
		break;
#endif
	}

	ax25_timer_expiry_stop(ax25);
}

static void ax25_t2timer_expiry(struct timer_list *t)
{
	ax25_cb *ax25 = timer_container_of(ax25, t, t2timer);

	ax25_timer_expiry_start(ax25);

	switch (ax25->ax25_dev->values[AX25_VALUES_PROTOCOL]) {
	case AX25_PROTO_STD_SIMPLEX:
	case AX25_PROTO_STD_DUPLEX:
		ax25_std_t2timer_expiry(ax25);
		break;

#ifdef CONFIG_AX25_DAMA_SLAVE
	case AX25_PROTO_DAMA_SLAVE:
		if (!ax25->ax25_dev->dama.slave)
			ax25_std_t2timer_expiry(ax25);
		break;
#endif
	}

	ax25_timer_expiry_stop(ax25);
}

static void ax25_t3timer_expiry(struct timer_list *t)
{
	ax25_cb *ax25 = timer_container_of(ax25, t, t3timer);

	ax25_timer_expiry_start(ax25);

	switch (ax25->ax25_dev->values[AX25_VALUES_PROTOCOL]) {
	case AX25_PROTO_STD_SIMPLEX:
	case AX25_PROTO_STD_DUPLEX:
		ax25_std_t3timer_expiry(ax25);
		break;

#ifdef CONFIG_AX25_DAMA_SLAVE
	case AX25_PROTO_DAMA_SLAVE:
		if (ax25->ax25_dev->dama.slave)
			ax25_ds_t3timer_expiry(ax25);
		else
			ax25_std_t3timer_expiry(ax25);
		break;
#endif
	}

	ax25_timer_expiry_stop(ax25);
}

static void ax25_idletimer_expiry(struct timer_list *t)
{
	ax25_cb *ax25 = timer_container_of(ax25, t, idletimer);

	ax25_timer_expiry_start(ax25);

	switch (ax25->ax25_dev->values[AX25_VALUES_PROTOCOL]) {
	case AX25_PROTO_STD_SIMPLEX:
	case AX25_PROTO_STD_DUPLEX:
		ax25_std_idletimer_expiry(ax25);
		break;

#ifdef CONFIG_AX25_DAMA_SLAVE
	case AX25_PROTO_DAMA_SLAVE:
		if (ax25->ax25_dev->dama.slave)
			ax25_ds_idletimer_expiry(ax25);
		else
			ax25_std_idletimer_expiry(ax25);
		break;
#endif
	}

	ax25_timer_expiry_stop(ax25);
}
