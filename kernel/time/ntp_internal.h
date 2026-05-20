/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_NTP_INTERNAL_H
#define _LINUX_NTP_INTERNAL_H

struct audit_ntp_data;

extern void ntp_init(void);
extern int ntp_get_status(void);
extern int ntp_get_time_state(void);
extern void ntp_clear(unsigned int tkid);
/* Returns how long ticks are at present, in ns / 2^NTP_SCALE_SHIFT. */
extern u64 ntp_tick_length(unsigned int tkid);
extern s64 ntp_get_skew_delta(unsigned int tkid);
extern s64 ntp_drain_time_offset(unsigned int tkid, s64 amount);
extern void ntp_set_time_offset(unsigned int tkid, s64 offset_ns);
extern s64 ntp_get_time_offset_ns(unsigned int tkid);
extern void ntp_set_tick_length(unsigned int tkid, u64 tick_length);
extern ktime_t ntp_get_next_leap(unsigned int tkid);
extern int second_overflow(unsigned int tkid, time64_t secs);
extern int ntp_adjtimex(unsigned int tkid, struct __kernel_timex *txc, const struct timespec64 *ts,
			s32 *time_tai, struct audit_ntp_data *ad);
extern void __hardpps(const struct timespec64 *phase_ts, const struct timespec64 *raw_ts);

#if defined(CONFIG_GENERIC_CMOS_UPDATE) || defined(CONFIG_RTC_SYSTOHC)
extern void ntp_notify_cmos_timer(bool offset_set);
#else
static inline void ntp_notify_cmos_timer(bool offset_set) { }
#endif

#endif /* _LINUX_NTP_INTERNAL_H */
