// SPDX-License-Identifier: GPL-2.0
/*
 * TCP ROCCET: An RTT-Oriented CUBIC Congestion Control
 * Extension for 5G and Beyond Networks
 *
 * TCP ROCCET is a new TCP congestion control
 * algorithm suited for current cellular 5G NR beyond networks.
 * It extends the kernel default congestion control CUBIC
 * and improves its performance, and additionally solves an
 * unwanted side effects of CUBIC’s implementation.
 * ROCCET uses its own Slow Start, called LAUNCH, where loss
 * is not considered as a congestion event.
 * The congestion avoidance phase, called ORBITER, uses
 * CUBIC's window growth function and adds, based on RTT
 * and ACK rate, congestion events.
 *
 * A peer-reviewed paper on TCP ROCCET will be presented
 * at the WONS 2026 conference.
 * A draft of the paper is available here:
 *		https://arxiv.org/abs/2510.25281
 *
 *
 * Further information about CUBIC:
 * TCP CUBIC: Binary Increase Congestion control for TCP v2.3
 * Home page:
 *	http://netsrv.csc.ncsu.edu/twiki/bin/view/Main/BIC
 * This is from the implementation of CUBIC TCP in
 * Sangtae Ha, Injong Rhee and Lisong Xu,
 *  "CUBIC: A New TCP-Friendly High-Speed TCP Variant"
 *  in ACM SIGOPS Operating System Review, July 2008.
 * Available from:
 *  http://netsrv.csc.ncsu.edu/export/cubic_a_new_tcp_2008.pdf
 *
 * CUBIC integrates a new slow start algorithm, called HyStart.
 * The details of HyStart are presented in
 *  Sangtae Ha and Injong Rhee,
 *  "Taming the Elephants: New TCP Slow Start", NCSU TechReport 2008.
 * Available from:
 *  http://netsrv.csc.ncsu.edu/export/hystart_techreport_2008.pdf
 *
 * All testing results are available from:
 * http://netsrv.csc.ncsu.edu/wiki/index.php/TCP_Testing
 *
 * Unless CUBIC is enabled and congestion window is large
 * this behaves the same as the original Reno.
 */

#include "linux/limits.h"
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <net/tcp.h>

/* Scale factor beta calculation (max_cwnd = snd_cwnd * beta) */
#define BICTCP_BETA_SCALE 1024

#define BICTCP_HZ 10 /* BIC HZ 2^10 = 1024 */

/* Alpha value for the sRrTT  multiplied by 100.
 * Here 20 represents a value of 0.2
 */
#define ROCCET_ALPHA_TIMES_100 20

/* min RTT probe period in seconds */
#define ROCCET_NEXT_MIN_RTT_PROBE 5000

/* State in which roccet currently operates */
enum roccet_state {
	LAUNCH,
	ORBITER,
	RTT_PROBE,
	RTT_PROBE_REFILL,
	DRAIN
};

/* TCP ROCCET struct based on the original BICTCP struct with
 * additions specific to the ROCCET-Algorithm.
 */
struct roccettcp {
	u32 cnt;	/* increase cwnd by 1 after ACKs */
	u32 last_max_cwnd;	/* last maximum snd_cwnd */
	u32 last_cwnd;	/* the last snd_cwnd */
	u32 last_time;	/* time when updated last_cwnd */
	u32 bic_origin_point;	/* origin point of bic function */
	u32 bic_K;	/* time to origin point from the
			 * beginning of the current epoch
			 */
	u32 delay_min;	/* min delay (usec) */
	u32 epoch_start;	/* beginning of an epoch */
	u32 ack_cnt;	/* number of acks */
	u32 tcp_cwnd;	/* estimated tcp cwnd */
	u32 curr_rtt;	/* last sample rtt of current round */

	u32 roccet_last_event_time_us; /* The last time ROCCET was triggered */
	u32 curr_min_rtt;	/* The current observed minRTT */
	u32 next_min_rtt_probe;	/* Next time to probe the minRTT */
	u32 probe_min_rtt_until;	/* End of minRTT probing period */
	u32 refill_until;	/* End of pipe refill after minRTT probe */
	u32 cwnd_before_min_rtt_probe;	/* cwnd before min RTT probeing */
	u32 curr_srrtt;	/* srRTT calculated based on the latest ACK */
	u32 next_srrtt_check;	/* Next check for srRTT */
	u32 last_rtt;	/* sample rtt of previous round.
			 * Used for jitter calculation
			 */

	u32 interval_snd_seq_start;
	u32 interval_una_seq_start;

	u32 ack_rate_last_rate_time;	/* Timestamp of the last ACK-rate */
	u16 ack_rate_last_rate;	/* Last ACK-rate */
	u16 ack_rate_curr_rate;	/* Current ACK-rate */
	u16 ack_rate_cnt;	/* Used for counting acks */

	bool ece_received;	/* Set to true if an ECE bit was received */
	enum roccet_state state; /* State in which roccet currently operates */
};

/* Parameters that are specific to the ROCCET-Algorithm */
static uint sr_rtt_upper_bound __read_mostly = 100;
static int ack_rate_diff_ss __read_mostly = 10;

module_param(sr_rtt_upper_bound, uint, 0644);
MODULE_PARM_DESC(sr_rtt_upper_bound, "ROCCET's upper bound for srRTT.");
module_param(ack_rate_diff_ss, int, 0644);
MODULE_PARM_DESC(ack_rate_diff_ss,
		 "ROCCET's threshold to exit slow start if ACK-rate defer by given amount of segments.");

static int fast_convergence __read_mostly = 1;
static int beta __read_mostly = 717; /* = 717/1024 (BICTCP_BETA_SCALE) */
static int initial_ssthresh __read_mostly;
static int bic_scale __read_mostly = 41;
static int tcp_friendliness __read_mostly = 1;

static u32 cube_rtt_scale __read_mostly;
static u32 beta_scale __read_mostly;
static u64 cube_factor __read_mostly;

/* Note parameters that are used for precomputing scale factors are read-only */
module_param(fast_convergence, int, 0644);
MODULE_PARM_DESC(fast_convergence, "turn on/off fast convergence");
module_param(beta, int, 0644);
MODULE_PARM_DESC(beta, "beta for multiplicative increase");
module_param(initial_ssthresh, int, 0644);
MODULE_PARM_DESC(initial_ssthresh, "initial value of slow start threshold");
module_param(bic_scale, int, 0444);
MODULE_PARM_DESC(bic_scale,
		 "scale (scaled by 1024) value for bic function (bic_scale/1024)");
module_param(tcp_friendliness, int, 0644);
MODULE_PARM_DESC(tcp_friendliness, "turn on/off tcp friendliness");

static __always_inline void roccettcp_reset(struct roccettcp *ca)
{
	memset(ca, 0, sizeof(struct roccettcp));
	ca->next_srrtt_check = 0;
	ca->curr_min_rtt = ~0U;
	ca->last_rtt = 0;
	ca->ece_received = false;

	ca->roccet_last_event_time_us = 0;
	ca->ack_rate_last_rate = 0;
	/* Initialize to current time to avoid an
	 * overflow in the ack rate calculation
	 */
	ca->ack_rate_last_rate_time = jiffies_to_usecs(tcp_jiffies32);
	ca->ack_rate_curr_rate = 0;
	ca->ack_rate_cnt = 0;

	/* Start state is LAUNCH */
	ca->state = LAUNCH;
}

/* Return true if ROCCET is in min RTT probing.
 */
static __always_inline bool is_in_min_rtt_probing(struct roccettcp *ca, u32 now)
{
	if (ca->probe_min_rtt_until == 0)
		return false;
	return before(now, ca->probe_min_rtt_until);
}

static __always_inline void update_min_rtt(struct sock *sk)
{
	struct roccettcp *ca = inet_csk_ca(sk);

	/* Check if new lower min RTT was found. If so, set it directly */
	if (ca->curr_rtt < ca->curr_min_rtt) {
		ca->curr_min_rtt = max(ca->curr_rtt, 1);
		/* Probe for the min RTT in ROCCET_NEXT_MIN_RTT_PROBE seconds
		 * if no other update occurs.
		 */
		ca->next_min_rtt_probe =
			jiffies_to_usecs(tcp_jiffies32) +
			ROCCET_NEXT_MIN_RTT_PROBE * USEC_PER_MSEC;
	}
}

/* Return difference between last and current ack rate.
 */
static __always_inline s32 get_ack_rate_diff(struct roccettcp *ca)
{
	if (ca->ack_rate_curr_rate < ca->ack_rate_last_rate)
		return 0;
	return (s32)(ca->ack_rate_curr_rate - ca->ack_rate_last_rate);
}

/* Update ack rate sampled by 100ms.
 */
static __always_inline void update_ack_rate(struct sock *sk, u32 acked, u32 now)
{
	struct roccettcp *ca = inet_csk_ca(sk);
	s32 interval = USEC_PER_MSEC * 100;

	s32 time_delta = (s32)(ca->ack_rate_last_rate_time - now);
	const s32 idle_threshold = USEC_PER_SEC * 2;

	// Check if the time has arrived in the new interval
	if (time_delta < -interval) {
		/* Check if the connection was idle for X seconds
		 * (e.g. no ACK for X seconds)
		 */
		if (time_delta < -idle_threshold) {
			/* Reset ack counting as if a new
			 * connection was created
			 */
			ca->ack_rate_last_rate = 0;
			ca->ack_rate_last_rate_time =
				jiffies_to_usecs(tcp_jiffies32);
			ca->ack_rate_curr_rate = 0;
			ca->ack_rate_cnt = 0;
		} else {
			ca->ack_rate_last_rate_time = now;
			ca->ack_rate_last_rate = ca->ack_rate_curr_rate;
			ca->ack_rate_curr_rate = ca->ack_rate_cnt;
			ca->ack_rate_cnt =
				acked; // start counting for the new interval
		}
	} else {
		// Cap the ack count to avoid overflow
		ca->ack_rate_cnt = min_t(u32, ca->ack_rate_cnt + acked,
					 U16_MAX);
	}
}

/* Compute srRTT.
 */
static __always_inline void update_srrtt(struct sock *sk)
{
	struct roccettcp *ca = inet_csk_ca(sk);

	/* Avoid integer overflow in the calculation below.
	 * This could occur in cases where we have not yet
	 * received an RTT sample. In these cases, set the
	 * rtt to a safe value.
	 */
	if (ca->curr_rtt < ca->curr_min_rtt) {
		ca->curr_rtt = max(ca->curr_rtt, 1);
		ca->curr_min_rtt = ca->curr_rtt;
	}

	/* Avoid division by zero */
	if (ca->curr_min_rtt == 0) {
		ca->curr_min_rtt = max(ca->curr_min_rtt, 1);
		return; // skip srRTT update
	}

	/* Calculate the new rRTT (Scaled by 100).
	 * 100 * ((sRTT - sRTT_min) / sRTT_min).
	 *
	 * curr_min_rtt_timed.rtt is always <= than curr_rtt,
	 * since this is the minimum of the rtt.
	 *
	 * 0 is a valid value for rrtt.
	 */
	u32 rrtt = div_u64(100 * (u64)(ca->curr_rtt - ca->curr_min_rtt),
			   ca->curr_min_rtt);

	// (1 - alpha) * srRTT + alpha * rRTT
	ca->curr_srrtt = ((100 - ROCCET_ALPHA_TIMES_100) * ca->curr_srrtt +
			  ROCCET_ALPHA_TIMES_100 * rrtt) /
			 100;
}

/* Do a ROCCET congestion event.
 */
static __always_inline void roccet_congestion_event(struct sock *sk, u32 now)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct roccettcp *ca = inet_csk_ca(sk);

	ca->epoch_start = 0;
	ca->roccet_last_event_time_us = now;
	ca->cnt = 100 * tcp_snd_cwnd(tp);
	/*Set W_max only if the current cwnd is larger */
	if (tcp_snd_cwnd(tp) > ca->last_max_cwnd)
		ca->last_max_cwnd = tcp_snd_cwnd(tp);
	tcp_snd_cwnd_set(tp,
			 min(tp->snd_cwnd_clamp,
			     max((tcp_snd_cwnd(tp) * beta)
				 / BICTCP_BETA_SCALE, 2U)));
	tp->snd_ssthresh = tcp_snd_cwnd(tp);
}

/* Do minimum RTT probing.
 */
static __always_inline void roccet_min_rtt_probe(struct sock *sk, u32 now)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct roccettcp *ca = inet_csk_ca(sk);
	u32 interval, probe_cwnd;

	/* Do nothing if we are probing */
	if (before(now, ca->probe_min_rtt_until) && ca->probe_min_rtt_until > 0)
		return;

	/* Start of min RTT probing*/
	if (ca->probe_min_rtt_until == 0) {
		/* Probe 1*RTT or at least 200ms */
		interval = max(200 * USEC_PER_MSEC, ca->curr_rtt);

		/* This is to handle deep shared buffers with loss-based
		 * congestion control like CUBIC. If the cwnd is not limited
		 * by the application but falsely detected (see ROCCET paper),
		 * we have to empty the pipe more.
		 * If the limit detection is correct this will cause no harm
		 * to the tcp flow because the cwnd is not fully utilized and
		 * we set the cwnd to its previous value after probing.
		 */
		probe_cwnd = max(tcp_snd_cwnd(tp) / 2, TCP_INIT_CWND);
		if (!tcp_is_cwnd_limited(sk))
			probe_cwnd = max(tcp_snd_cwnd(tp) / 3, TCP_INIT_CWND);

		ca->probe_min_rtt_until = now + interval;
		ca->cwnd_before_min_rtt_probe = tcp_snd_cwnd(tp);

		/* Half the cwnd to drain the buffer for probing.
		 * Set the ssthresh to the probing cwnd otherwise
		 * the TCP state machine is in slow start.
		 */
		tcp_snd_cwnd_set(tp, probe_cwnd);
		tcp_sk(sk)->snd_ssthresh = tcp_snd_cwnd(tp);

		/* Reset current min RTT to allow probing for
		 * a new lower and higher minimum RTT.
		 */
		ca->curr_min_rtt = ~0U;

		/* Refill the pipe after probing.
		 * To this end we need the previous cwnd over
		 * the probing interval.
		 */
		ca->refill_until = ca->probe_min_rtt_until + interval;
	} else if (before(now, ca->refill_until)) {
		/* Reset cwnd and refill the pipe. */
		if (ca->state != RTT_PROBE_REFILL) {
			tcp_snd_cwnd_set(tp, ca->cwnd_before_min_rtt_probe);
			tcp_sk(sk)->snd_ssthresh = tcp_snd_cwnd(tp);
			ca->state = RTT_PROBE_REFILL;
		}
	} else {
		/* End min RTT probing phase. */
		ca->probe_min_rtt_until = 0;
		ca->state = ORBITER;
	}
}

static void roccettcp_init(struct sock *sk)
{
	struct roccettcp *ca = inet_csk_ca(sk);

	roccettcp_reset(ca);

	if (initial_ssthresh)
		tcp_sk(sk)->snd_ssthresh = initial_ssthresh;

	cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
	//WRITE_ONCE(sk->sk_pacing_rate, 0);
}

static void roccettcp_cwnd_event_tx_start(struct sock *sk)
{
	struct roccettcp *ca = inet_csk_ca(sk);
	u32 now = tcp_jiffies32;
	s32 delta;

	delta = now - tcp_sk(sk)->lsndtime;

	/* We were application limited (idle) for a while.
	 * Shift epoch_start to keep cwnd growth to cubic curve.
	 */
	if (ca->epoch_start && delta > 0) {
		ca->epoch_start += delta;
		if (after(ca->epoch_start, now))
			ca->epoch_start = now;
	}
}

/* calculate the cubic root of x using a table lookup followed by one
 * Newton-Raphson iteration.
 * Avg err ~= 0.195%
 */
static u32 cubic_root(u64 a)
{
	u32 x, b, shift;
	/* cbrt(x) MSB values for x MSB values in [0..63].
	 * Precomputed then refined by hand - Willy Tarreau
	 *
	 * For x in [0..63],
	 *   v = cbrt(x << 18) - 1
	 *   cbrt(x) = (v[x] + 10) >> 6
	 */
	static const u8 v[] = {
		/* 0x00 */ 0,	54,  54,  54,  118, 118, 118, 118,
		/* 0x08 */ 123, 129, 134, 138, 143, 147, 151, 156,
		/* 0x10 */ 157, 161, 164, 168, 170, 173, 176, 179,
		/* 0x18 */ 181, 185, 187, 190, 192, 194, 197, 199,
		/* 0x20 */ 200, 202, 204, 206, 209, 211, 213, 215,
		/* 0x28 */ 217, 219, 221, 222, 224, 225, 227, 229,
		/* 0x30 */ 231, 232, 234, 236, 237, 239, 240, 242,
		/* 0x38 */ 244, 245, 246, 248, 250, 251, 252, 254,
	};

	b = fls64(a);
	if (b < 7) {
		/* a in [0..63] */
		return ((u32)v[(u32)a] + 35) >> 6;
	}

	b = ((b * 84) >> 8) - 1;
	shift = (a >> (b * 3));

	x = ((u32)(((u32)v[shift] + 10) << b)) >> 6;

	/* Newton-Raphson iteration
	 *                         2
	 * x    = ( 2 * x  +  a / x  ) / 3
	 *  k+1          k         k
	 */
	x = (2 * x + (u32)div64_u64(a, (u64)x * (u64)(x - 1)));
	x = ((x * 341) >> 10);
	return x;
}

/* Compute congestion window to use.
 */
static __always_inline void bictcp_update(struct roccettcp *ca, u32 cwnd,
					  u32 acked)
{
	u32 delta, bic_target, max_cnt;
	u64 offs, t;

	ca->ack_cnt += acked; /* count the number of ACKed packets */

	if (ca->last_cwnd == cwnd &&
	    (s32)(tcp_jiffies32 - ca->last_time) <= HZ / 32)
		return;

	/* The CUBIC function can update ca->cnt at most once per jiffy.
	 * On all cwnd reduction events, ca->epoch_start is set to 0,
	 * which will force a recalculation of ca->cnt.
	 */
	if (ca->epoch_start && tcp_jiffies32 == ca->last_time)
		goto tcp_friendliness;

	ca->last_cwnd = cwnd;
	ca->last_time = tcp_jiffies32;

	if (ca->epoch_start == 0) {
		ca->epoch_start = tcp_jiffies32; /* record beginning */
		ca->ack_cnt = acked; /* start counting */
		ca->tcp_cwnd = cwnd; /* syn with cubic */

		if (ca->last_max_cwnd <= cwnd) {
			ca->bic_K = 0;
			ca->bic_origin_point = cwnd;
		} else {
			/* Compute new K based on
			 * (wmax-cwnd) * (srtt>>3 / HZ) / c * 2^(3*bictcp_HZ)
			 */
			ca->bic_K = cubic_root(cube_factor *
					       (ca->last_max_cwnd - cwnd));
			ca->bic_origin_point = ca->last_max_cwnd;
		}
	}

	/* cubic function - calc */
	/* calculate c * time^3 / rtt,
	 *  while considering overflow in calculation of time^3
	 * (so time^3 is done by using 64 bit)
	 * and without the support of division of 64bit numbers
	 * (so all divisions are done by using 32 bit)
	 *  also NOTE the unit of those variables
	 *	  time  = (t - K) / 2^bictcp_HZ
	 *	  c = bic_scale >> 10
	 * rtt  = (srtt >> 3) / HZ
	 * !!! The following code does not have overflow problems,
	 * if the cwnd < 1 million packets !!!
	 */

	t = (s32)(tcp_jiffies32 - ca->epoch_start);
	t += usecs_to_jiffies(ca->delay_min);

	/* change the unit from HZ to bictcp_HZ */
	t <<= BICTCP_HZ;
	do_div(t, HZ);

	if (t < ca->bic_K) /* t - K */
		offs = ca->bic_K - t;
	else
		offs = t - ca->bic_K;

	/* c/rtt * (t-K)^3 */
	delta = (cube_rtt_scale * offs * offs * offs) >> (10 + 3 * BICTCP_HZ);
	if (t < ca->bic_K) /* below origin*/
		bic_target = ca->bic_origin_point - delta;
	else /* above origin*/
		bic_target = ca->bic_origin_point + delta;

	/* cubic function - calc bictcp_cnt*/
	if (bic_target > cwnd)
		ca->cnt = cwnd / (bic_target - cwnd);
	else
		ca->cnt = 100 * cwnd; /* very small increment*/

	/* The initial growth of cubic function may be too conservative
	 * when the available bandwidth is still unknown.
	 */
	if (ca->last_max_cwnd == 0 && ca->cnt > 20)
		ca->cnt = 20; /* increase cwnd 5% per RTT */

tcp_friendliness:
	/* TCP Friendly */
	if (tcp_friendliness) {
		u32 scale = beta_scale;

		delta = (cwnd * scale) >> 3;
		while (ca->ack_cnt > delta) { /* update tcp cwnd */
			ca->ack_cnt -= delta;
			ca->tcp_cwnd++;
		}

		if (ca->tcp_cwnd > cwnd) { /* if bic is slower than tcp */
			delta = ca->tcp_cwnd - cwnd;
			max_cnt = cwnd / delta;
			if (ca->cnt > max_cnt)
				ca->cnt = max_cnt;
		}
	}

	/* The maximum rate of cwnd increase CUBIC allows is 1 packet per
	 * 2 packets ACKed, meaning cwnd grows at 1.5x per RTT.
	 */
	ca->cnt = max(ca->cnt, 2U);
}

static void roccettcp_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct roccettcp *ca = inet_csk_ca(sk);

	u32 now = jiffies_to_usecs(tcp_jiffies32);
	bool evaluate_srrtt = false;
	bool send_more_than_acked = false;
	u32 roccet_xj;
	u32 jitter;
	u32 send, received;

	if (ca->state == LAUNCH) {
		/* LAUNCH: Detect an exit point for tcp slow start
		 * in networks with large buffers of multiple BDP
		 * Like in cellular networks (5G, ...).
		 * Or exit LAUNCH if cwnd is too large for application layer
		 * data rate (tcp cwnd validation).
		 */
		if ((ca->curr_srrtt > sr_rtt_upper_bound &&
		     get_ack_rate_diff(ca) <= ack_rate_diff_ss) ||
		    !tcp_is_cwnd_limited(sk)) {
			ca->epoch_start = 0;

			/* Handle initial slow start.
			 * Most bufferbloat occurs here
			 */
			if (tp->snd_ssthresh == TCP_INFINITE_SSTHRESH) {
				tcp_sk(sk)->snd_ssthresh = tcp_snd_cwnd(tp)
								/ 2;
				/* since this is the initial slow start,
				 * the min cwnd won't be 1, so the window
				 * can't be set to 0 by accident.
				 * Halfing the cwnd will undo the previous step
				 * of slow start. Which is fine since the pipe
				 * is already full.
				 */
				tcp_snd_cwnd_set(tp, max(tcp_snd_cwnd(tp) / 2,
							 TCP_INIT_CWND));
			} else {
				tcp_sk(sk)->snd_ssthresh =
					tcp_snd_cwnd(tp) -
					(tcp_snd_cwnd(tp) / 3);
				tcp_snd_cwnd_set(tp, tcp_snd_cwnd(tp) -
						 (tcp_snd_cwnd(tp) / 3));
			}
			ca->roccet_last_event_time_us = now;
			return;
		}

		acked = tcp_slow_start(tp, acked);
		if (!acked)
			return;

	} else if (ca->state == ORBITER) {
		/* ORBITER: Increase the cwnd by using the CUBIC
		 * cwnd growth function, if no roccet congestion
		 * event is detechted.
		 */

		/* Calculate jitter */
		if ((s32)(ca->curr_rtt - ca->last_rtt) < 0)
			jitter = ca->last_rtt - ca->curr_rtt;
		else
			jitter = ca->curr_rtt - ca->last_rtt;

		if (ca->next_srrtt_check == 0)
			ca->next_srrtt_check = now + 5 * ca->curr_rtt;

		/* Calculate if more bytes was send than received
		 * in the time interval.
		 */
		if (before(tp->snd_nxt, ca->interval_snd_seq_start)) {
			/* We had a wrap around in seq no counter */
			send = (~0U - ca->interval_snd_seq_start + tp->snd_nxt);
		} else {
			send = (tp->snd_nxt - ca->interval_snd_seq_start);
		}
		if (before(tp->snd_una, ca->interval_una_seq_start)) {
			/* We had a wrap around in seq no counter */
			received = (~0U - ca->interval_una_seq_start +
				    tp->snd_una);
		} else {
			received = (tp->snd_una - ca->interval_una_seq_start);
		}

		/* Here we use a guard space of 1% of the current cwnd.
		 * We do this to avoid a false positive evaluation due
		 * to delays caused by jitter or scheduling.
		 */
		send_more_than_acked =
			send >
			received + ((tcp_snd_cwnd(tp) * tp->mss_cache) / 100);

		/* Check if it's time to evaluate the srRTT */
		if ((s32)(ca->next_srrtt_check - now) < 0) {
			evaluate_srrtt = true;

			/* reset struct and set next end of period */
			ca->next_srrtt_check = now + 5 * ca->curr_rtt;

			/* Reset Rate calculation */
			ca->interval_snd_seq_start = tp->snd_nxt;
			ca->interval_una_seq_start = tp->snd_una;
		}

		/* Respects the jitter of the connection and add it on top of
		 * the upper bound for the srRTT.
		 */
		roccet_xj = div_u64((u64)jitter * 100, ca->curr_min_rtt) +
			    sr_rtt_upper_bound;
		if (roccet_xj < sr_rtt_upper_bound)
			roccet_xj = sr_rtt_upper_bound;

		/* The srRTT exceeds the upper bound if bufferbloat happens.
		 * Here, we want to reduce the cwnd and drain the buffer.
		 */
		if (ca->curr_srrtt > roccet_xj && evaluate_srrtt &&
		    send_more_than_acked) {
			roccet_congestion_event(sk, now);
			return;
		}

		/* Terminates this function if cwnd is not fully utilized.
		 * In mobile networks like 5G, this termination causes the
		 * cwnd to be frozen at an excessively high value. This is
		 * because slow start or HyStart massively exceed the available
		 * bandwidth and leave the cwnd at an excessively high value.
		 * The cwnd cannot therefore be fully utilized because it is
		 * limited by the connection capacity.
		 */
		if (!tcp_is_cwnd_limited(sk) || send_more_than_acked)
			return;

		bictcp_update(ca, tcp_snd_cwnd(tp), acked);
		tcp_cong_avoid_ai(tp, max(1, ca->cnt), acked);
	}
}

static u32 roccettcp_recalc_ssthresh(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct roccettcp *ca = inet_csk_ca(sk);
	u32 cwnd = tcp_snd_cwnd(tp);

	/* If a loss/ECN occurs in the refill phase of min RTT probing
	 * we reduce the cwnd and abort the refill.
	 */
	if (ca->state == RTT_PROBE_REFILL)
		ca->state = ORBITER;

	/* If ROCCET is in min RTT probing and a loss/ECN occurs,
	 * we use the cwnd before the probing interval to
	 * calculate the cwnd reduction and continue probing.
	 * After min RTT probing the cwnd is set to the reduced
	 * value. During min RTT probing it is very likely that
	 * congestion was caused by the cwnd value before min
	 * RTT probing.
	 */
	if (ca->state == RTT_PROBE) {
		/* Handle ECN as cubic congestion event in min
		 * RTT probe.
		 */
		ca->ece_received = false;

		ca->epoch_start = 0; /* end of epoch */

		/* Wmax and fast convergence */
		if (cwnd < ca->last_max_cwnd && fast_convergence)
			ca->last_max_cwnd =
				(cwnd * (BICTCP_BETA_SCALE + beta)) /
				(2 * BICTCP_BETA_SCALE);
		else
			ca->last_max_cwnd = cwnd;

		cwnd = ca->cwnd_before_min_rtt_probe;
		ca->cwnd_before_min_rtt_probe =
			max((cwnd * beta) / BICTCP_BETA_SCALE, 2U);

		return cwnd;
	}

	/* Handle ECN as ROCCET congestion event. */
	if (ca->ece_received) {
		ca->ece_received = false;
		roccet_congestion_event(sk, jiffies_to_usecs(tcp_jiffies32));
		return tcp_snd_cwnd(tp);
	}

	/* On loss in slow start enter congestion avoidance
	 * without a cwnd reduction. Additional slow start
	 * exit conditions with a cwnd reduction are handled
	 * in roccettcp_cong_avoid.
	 */
	if (tcp_in_slow_start(tp))
		return tcp_snd_cwnd(tp);

	/* CUBIC congestion event */
	ca->epoch_start = 0; /* end of epoch */

	/* Wmax and fast convergence */
	if (tcp_snd_cwnd(tp) < ca->last_max_cwnd && fast_convergence)
		ca->last_max_cwnd =
			(tcp_snd_cwnd(tp) * (BICTCP_BETA_SCALE + beta)) /
			(2 * BICTCP_BETA_SCALE);
	else
		ca->last_max_cwnd = tcp_snd_cwnd(tp);

	return max((tcp_snd_cwnd(tp) * beta) / BICTCP_BETA_SCALE, 2U);
}

static void roccettcp_state(struct sock *sk, u8 new_state)
{
	struct roccettcp *ca = inet_csk_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);

	if (new_state == TCP_CA_Loss) {
		roccettcp_reset(ca);
	} else if (new_state == TCP_CA_Recovery) {
		tcp_sk(sk)->snd_ssthresh = roccettcp_recalc_ssthresh(sk);
		tcp_snd_cwnd_set(tp, tcp_sk(sk)->snd_ssthresh);
	}
}

static void roccettcp_acked(struct sock *sk, const struct ack_sample *sample)
{
	struct roccettcp *ca = inet_csk_ca(sk);

	/* Some calls are for duplicates without timestamps */
	if (sample->rtt_us < 0)
		return;

	/* Discard delay samples right after fast recovery */
	if (ca->epoch_start && (s32)(tcp_jiffies32 - ca->epoch_start) < HZ)
		return;

	u32 delay = sample->rtt_us;

	if (delay == 0)
		delay = 1;

	/* first time call or link delay decreases */
	if (ca->delay_min == 0 || (s32)(delay - ca->delay_min) < 0)
		ca->delay_min = delay;

	/* Get valid sample for roccet */
	if (sample->rtt_us > 0) {
		ca->last_rtt = ca->curr_rtt;
		ca->curr_rtt = sample->rtt_us;
	}
}

static void roccet_in_ack_event(struct sock *sk, u32 flags)
{
	struct roccettcp *ca = inet_csk_ca(sk);

	/* Handle ECE bit.
	 * Processing of ECE events is done in roccettcp_recalc_ssthresh()
	 */
	if (flags & CA_ACK_ECE)
		ca->ece_received = true;
}

static void roccet_control(struct sock *sk, u32 ack, int flag,
			   const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct roccettcp *ca = inet_csk_ca(sk);

	u32 now = jiffies_to_usecs(tcp_jiffies32);
	u64 rate;

	/* Update roccet parameters */
	update_ack_rate(sk, rs->acked_sacked, now);
	update_min_rtt(sk);
	update_srrtt(sk);

	/* Set values for send and receive rate */
	if (ca->interval_snd_seq_start == 0) {
		ca->interval_snd_seq_start = tp->snd_nxt;
		ca->interval_una_seq_start = tp->snd_una;
	}

	/* Update roccet state */
	if (tcp_in_slow_start(tp)) {
		ca->state = LAUNCH;
	} else if ((s32)now - ca->roccet_last_event_time_us <=
		   100 * USEC_PER_MSEC) {
		ca->state = DRAIN;
	} else if (after(now, ca->next_min_rtt_probe) ||
		   ca->state == RTT_PROBE || ca->state == RTT_PROBE_REFILL) {
		if (ca->state != RTT_PROBE_REFILL)
			ca->state = RTT_PROBE;
		roccet_min_rtt_probe(sk, now);
	} else {
		ca->state = ORBITER;
	}

	/* If nothing was fully acked do not increase the cwnd */
	if (!rs->acked_sacked)
		return;

	/* Increase the cwnd.
	 * Loss recovery is handled in roccettcp_state()
	 */
	roccettcp_cong_avoid(sk, ack, rs->acked_sacked);

	/* Adjust pacing rate. The code here is similar to the
	 * pacing rate adjustments in tcp_input.c tcp_cong_control().
	 * In LAUNCH (slow start) we want a pacing of 200% and
	 * in ORBITER (congestion avoidance) we adjust the pacing
	 * to 100% and do not use the sysctl_tcp_pacing_ca_ratio.
	 */

	/* set sk_pacing_rate to 200 % of current rate (mss * cwnd / srtt) */
	rate = (u64)tp->mss_cache * ((USEC_PER_SEC / 100) << 3);

	/* current rate is (cwnd * mss) / srtt
	 * In Slow Start [1], set sk_pacing_rate to 200 % the current rate.
	 * In Congestion Avoidance phase, set it to 120 % the current rate.
	 *
	 * [1]: Normal Slow Start cond is (tp->snd_cwnd < tp->snd_ssthresh)
	 *	 If snd_cwnd >= (tp->snd_ssthresh / 2), we are approaching
	 *	 end of slow start and should slow down.
	 */
	if (tcp_snd_cwnd(tp) < tp->snd_ssthresh / 2)
		rate *= READ_ONCE
			(sock_net(sk)->ipv4.sysctl_tcp_pacing_ss_ratio);
	else
		/* Pacing rate of 100%
		 * (instead of ipv4.sysctl_tcp_pacing_ca_ratio)
		 */
		rate *= 100;

	rate *= max(tcp_snd_cwnd(tp), tp->packets_out);

	if (likely(tp->srtt_us))
		do_div(rate, tp->srtt_us);

	/* WRITE_ONCE() is needed because sch_fq fetches sk_pacing_rate
	 * without any lock. We want to make sure compiler won't store
	 * intermediate values in this location.
	 */
	WRITE_ONCE(sk->sk_pacing_rate,
		   min_t(u64, rate, READ_ONCE(sk->sk_max_pacing_rate)));
}

static struct tcp_congestion_ops roccet_tcp __read_mostly = {
	.init = roccettcp_init,
	.ssthresh = roccettcp_recalc_ssthresh,
	.set_state = roccettcp_state,
	.undo_cwnd = tcp_reno_undo_cwnd,
	.cwnd_event_tx_start = roccettcp_cwnd_event_tx_start,
	.pkts_acked = roccettcp_acked,
	.in_ack_event = roccet_in_ack_event,
	.cong_control = roccet_control,
	.owner = THIS_MODULE,
	.name = "roccet",
};

static int __init roccettcp_register(void)
{
	BUILD_BUG_ON(sizeof(struct roccettcp) > ICSK_CA_PRIV_SIZE);

	/*
	 * Validate parameters to avoid division by zero errors.
	 */
	if (beta <= 0 || beta >= BICTCP_BETA_SCALE) {
		pr_err("roccet: beta must be between 0 and %d\n",
		       BICTCP_BETA_SCALE);
		return -EINVAL;
	}

	if (bic_scale <= 0) {
		pr_err("roccet: bic_scale must be positive\n");
		return -EINVAL;
	}

	/* Precompute a bunch of the scaling factors that are used per-packet
	 * based on SRTT of 100ms
	 */
	beta_scale =
		8 * (BICTCP_BETA_SCALE + beta) / 3 / (BICTCP_BETA_SCALE - beta);

	cube_rtt_scale = (bic_scale * 10); /* 1024*c/rtt */

	/* calculate the "K" for (wmax-cwnd) = c/rtt * K^3
	 *  so K = cubic_root( (wmax-cwnd)*rtt/c )
	 * the unit of K is bictcp_HZ=2^10, not HZ
	 *
	 *  c = bic_scale >> 10
	 *  rtt = 100ms
	 *
	 * the following code has been designed and tested for
	 * cwnd < 1 million packets
	 * RTT < 100 seconds
	 * HZ < 1,000,00  (corresponding to 10 nano-second)
	 */

	/* 1/c * 2^2*bictcp_HZ * srtt */
	cube_factor = 1ull << (10 + 3 * BICTCP_HZ); /* 2^40 */

	/* divide by bic_scale and by constant Srtt (100ms) */
	do_div(cube_factor, bic_scale * 10);

	return tcp_register_congestion_control(&roccet_tcp);
}

static void __exit roccettcp_unregister(void)
{
	tcp_unregister_congestion_control(&roccet_tcp);
}

module_init(roccettcp_register);
module_exit(roccettcp_unregister);

MODULE_AUTHOR("Lukas Prause, Tim Füchsel");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ROCCET TCP");
