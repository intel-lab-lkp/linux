/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _PPP_CHANNEL_H_
#define _PPP_CHANNEL_H_
/*
 * Definitions for the interface between the generic PPP code
 * and a PPP channel.
 *
 * A PPP channel provides a way for the generic PPP code to send
 * and receive packets over some sort of communications medium.
 * Packets are stored in sk_buffs and have the 2-byte PPP protocol
 * number at the start, but not the address and control bytes.
 *
 * Copyright 1999 Paul Mackerras.
 *
 * ==FILEVERSION 20000322==
 */

#include <linux/list.h>
#include <linux/skbuff.h>
#include <linux/poll.h>
#include <net/net_namespace.h>

struct net_device_path;
struct net_device_path_ctx;
struct ppp_channel;

struct ppp_channel_ops {
	/* Send a packet (or multilink fragment) on this channel.
	   Returns 1 if it was accepted, 0 if not. */
	int	(*start_xmit)(void *private, struct sk_buff *skb);
	/* Handle an ioctl call that has come in via /dev/ppp. */
	int	(*ioctl)(void *private, unsigned int cmd, unsigned long arg);
	int	(*fill_forward_path)(struct net_device_path_ctx *ctx,
				     struct net_device_path *path,
				     void *private);
};

struct ppp_channel_conf {
	void		*private;	/* channel private data */
	const struct ppp_channel_ops *ops; /* operations for this channel */
	int		hdrlen;		/* amount of headroom channel needs */
	bool		direct_xmit;	/* no qdisc, xmit directly */
#ifdef CONFIG_PPP_MULTILINK
	int		speed;		/* transfer rate (bytes/second) */
	int		mtu;		/* max transmit packet size */
#endif
};

#ifdef __KERNEL__
/* Called by the channel when it can send some more data. */
void ppp_output_wakeup(struct ppp_channel *pch);

/* Called by the channel to process a received PPP packet.
   The packet should have just the 2-byte PPP protocol header. */
void ppp_input(struct ppp_channel *pch, struct sk_buff *skb);

/* Called by the channel when an input error occurs, indicating
   that we may have missed a packet. */
void ppp_input_error(struct ppp_channel *pch);

/* Create a new, unattached ppp channel for specified net. */
struct ppp_channel *ppp_register_net_channel(struct net *net,
					     const struct ppp_channel_conf *chan);

/* Create a new, unattached ppp channel. */
struct ppp_channel *ppp_register_channel(const struct ppp_channel_conf *chan);

/* Detach a channel from its PPP unit (e.g. on hangup). */
void ppp_unregister_channel(struct ppp_channel *pch);

/* Get the channel number for a channel */
int ppp_channel_index(struct ppp_channel *pch);

/* Get the unit number associated with a channel, or -1 if none */
int ppp_unit_number(struct ppp_channel *pch);

/* Get the device name associated with a channel, or NULL if none.
 * Caller must hold RCU read lock.
 */
char *ppp_dev_name(struct ppp_channel *pch);

/* Update the MTU of a multilink channel */
#ifdef CONFIG_PPP_MULTILINK
void ppp_channel_update_mtu(struct ppp_channel *pch, int mtu);
#else
static inline void ppp_channel_update_mtu(struct ppp_channel *pch, int mtu) {}
#endif

/*
 * SMP locking notes:
 * The channel code must ensure that when it calls ppp_unregister_channel,
 * nothing is executing in any of the procedures above, for that
 * channel.  The generic layer will ensure that nothing is executing
 * in the start_xmit and ioctl routines for the channel by the time
 * that ppp_unregister_channel returns.
 */

#endif /* __KERNEL__ */
#endif
