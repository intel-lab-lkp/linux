/* SPDX-License-Identifier: GPL-2.0 */
/*
 * TCP ROCCET congestion control interface
 */
#ifndef __TCP_ROCCET_H
#define __TCP_ROCCET_H 1

#include <linux/math64.h>

struct ack_rate {
	u16 last_rate;	/* Last ACK-rate */
	u32 last_rate_time;	/* Timestamp of the last ACK-rate */
	u16 curr_rate;	/* Current ACK-rate */
	u16 cnt;	/* Used for counting acks */
};

struct bandwidth_limit_detect {
	u32 sum_cwnd;	/* sum of cwnd during time interval */
	u32 sum_acked;	/* sum of received acks during time interval */
	u32 next_check;	/* end/upper bound of time interval */
};

struct timed_rtt {
	u32 time;	/* Time of recording */
	u32 rtt;	/* Measured RTT */
};

/* Based on the BICTCP struct with additions specific for the ROCCET-Algorithm */
struct roccettcp {
	u32 cnt;		/* increase cwnd by 1 after ACKs */
	u32 last_max_cwnd;	/* last maximum snd_cwnd */
	u32 last_cwnd;		/* the last snd_cwnd */
	u32 last_time;		/* time when updated last_cwnd */
	u32 bic_origin_point;	/* origin point of bic function */
	u32 bic_K;		/* time to origin point from the
				 * beginning of the current epoch
				 */
	u32 delay_min;		/* min delay (usec) */
	u32 epoch_start;	/* beginning of an epoch */
	u32 ack_cnt;		/* number of acks */
	u32 tcp_cwnd;		/* estimated tcp cwnd */
	u32 curr_rtt;		/* the minimum rtt of current round */

	u32 roccet_last_event_time_us;	/* The last time ROCCET was triggered */
	bool ece_received;	/* Set to true if an ECE bit was received */
	u32 curr_min_rtt;		/* The current observed minRTT */
	struct timed_rtt curr_min_rtt_timed;	/* The current observed minRTT with
						 * the timestamp when it was observed
						 */
	u32 curr_srrtt;			/* The srRTT calculated based on the latest ACK */
	struct ack_rate ack_rate;	/* The last and the current ACK rate */
	struct bandwidth_limit_detect bw_limit;
	u32 last_rtt;			/* Used for jitter calculation */
};

#endif /* __TCP_ROCCET_H */
