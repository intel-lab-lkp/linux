/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DEPT(DEPendency Tracker) - runtime dependency tracker
 *
 * Started by Byungchul Park <max.byungchul.park@gmail.com>:
 *
 *  Copyright (c) 2020 LG Electronics, Inc., Byungchul Park
 *  Copyright (c) 2024 SK hynix, Inc., Byungchul Park
 */

#ifndef __LINUX_DEPT_H
#define __LINUX_DEPT_H

#ifdef CONFIG_DEPT

struct task_struct;

#define DEPT_MAX_STACK_ENTRY		16
#define DEPT_MAX_WAIT_HIST		64
#define DEPT_MAX_ECXT_HELD		48

#define DEPT_MAX_SUBCLASSES		16
#define DEPT_MAX_SUBCLASSES_EVT		2
#define DEPT_MAX_SUBCLASSES_USR		(DEPT_MAX_SUBCLASSES / DEPT_MAX_SUBCLASSES_EVT)
#define DEPT_MAX_SUBCLASSES_CACHE	2

enum {
	DEPT_CXT_SIRQ = 0,
	DEPT_CXT_HIRQ,
	DEPT_CXT_IRQS_NR,
	DEPT_CXT_PROCESS = DEPT_CXT_IRQS_NR,
	DEPT_CXTS_NR
};

#define DEPT_SIRQF			(1UL << DEPT_CXT_SIRQ)
#define DEPT_HIRQF			(1UL << DEPT_CXT_HIRQ)

struct dept_key {
	union {
		/*
		 * Each byte-wise address will be used as its key.
		 */
		char			base[DEPT_MAX_SUBCLASSES];

		/*
		 * for caching the main class pointer
		 */
		struct dept_class	*classes[DEPT_MAX_SUBCLASSES_CACHE];
	};
};

struct dept_map {
	const char			*name;
	struct dept_key			*keys;

	/*
	 * subclass that can be set from user
	 */
	int				sub_u;

	/*
	 * It's local copy for fast access to the associated classes.
	 * Also used for dept_key for static maps.
	 */
	struct dept_key			map_key;

	/*
	 * wait timestamp associated to this map
	 */
	unsigned int			wgen;

	/*
	 * whether this map should be going to be checked or not
	 */
	bool				nocheck;
};

#define DEPT_MAP_INITIALIZER(n, k)					\
{									\
	.name = #n,							\
	.keys = (struct dept_key *)(k),					\
	.sub_u = 0,							\
	.map_key = { .classes = { NULL, } },				\
	.wgen = 0U,							\
	.nocheck = false,						\
}

struct dept_ecxt_held {
	/*
	 * associated event context
	 */
	struct dept_ecxt		*ecxt;

	/*
	 * unique key for this dept_ecxt_held
	 */
	struct dept_map			*map;

	/*
	 * class of the ecxt of this dept_ecxt_held
	 */
	struct dept_class		*class;

	/*
	 * the wgen when the event context started
	 */
	unsigned int			wgen;

	/*
	 * subclass that only works in the local context
	 */
	int				sub_l;
};

struct dept_wait_hist {
	/*
	 * associated wait
	 */
	struct dept_wait		*wait;

	/*
	 * unique id of all waits system-wise until wrapped
	 */
	unsigned int			wgen;

	/*
	 * local context id to identify IRQ context
	 */
	unsigned int			ctxt_id;
};

/*
 * for subsystems that requires compact use of memory e.g. struct page
 */
struct dept_ext_wgen {
	unsigned int wgen;
};

extern void dept_on(void);
extern void dept_off(void);
extern void dept_init(void);
extern void dept_task_init(struct task_struct *t);
extern void dept_task_exit(struct task_struct *t);
extern void dept_free_range(void *start, unsigned int sz);

extern void dept_map_init(struct dept_map *m, struct dept_key *k, int sub_u, const char *n);
extern void dept_map_reinit(struct dept_map *m, struct dept_key *k, int sub_u, const char *n);
extern void dept_ext_wgen_init(struct dept_ext_wgen *ewg);
extern void dept_map_copy(struct dept_map *to, struct dept_map *from);
extern void dept_wait(struct dept_map *m, unsigned long w_f, unsigned long ip, const char *w_fn, int sub_l, long timeout);
extern void dept_stage_wait(struct dept_map *m, struct dept_key *k, unsigned long ip, const char *w_fn, long timeout);
extern void dept_request_event_wait_commit(void);
extern void dept_clean_stage(void);
extern void dept_ttwu_stage_wait(struct task_struct *t, unsigned long ip);
extern void dept_ecxt_enter(struct dept_map *m, unsigned long e_f, unsigned long ip, const char *c_fn, const char *e_fn, int sub_l);
extern bool dept_ecxt_holding(struct dept_map *m, unsigned long e_f);
extern void dept_request_event(struct dept_map *m, struct dept_ext_wgen *ewg);
extern void dept_event(struct dept_map *m, unsigned long e_f, unsigned long ip, const char *e_fn, struct dept_ext_wgen *ewg);
extern void dept_ecxt_exit(struct dept_map *m, unsigned long e_f, unsigned long ip);
extern void dept_sched_enter(void);
extern void dept_sched_exit(void);
extern void dept_update_cxt(void);

static inline void dept_ecxt_enter_nokeep(struct dept_map *m)
{
	dept_ecxt_enter(m, 0UL, 0UL, NULL, NULL, 0);
}

/*
 * for users who want to manage external keys
 */
extern void dept_key_init(struct dept_key *k);
extern void dept_key_destroy(struct dept_key *k);
extern void dept_map_ecxt_modify(struct dept_map *m, unsigned long e_f, struct dept_key *new_k, unsigned long new_e_f, unsigned long new_ip, const char *new_c_fn, const char *new_e_fn, int new_sub_l);

extern void dept_softirq_enter(void);
extern void dept_hardirq_enter(void);
extern void dept_softirqs_on_ip(unsigned long ip);
extern void dept_hardirqs_on(void);
extern void dept_softirqs_off(void);
extern void dept_hardirqs_off(void);
#else /* !CONFIG_DEPT */
struct dept_key { };
struct dept_map { };
struct dept_ext_wgen { };

#define DEPT_MAP_INITIALIZER(n, k) { }

#define dept_on()					do { } while (0)
#define dept_off()					do { } while (0)
#define dept_init()					do { } while (0)
#define dept_task_init(t)				do { } while (0)
#define dept_task_exit(t)				do { } while (0)
#define dept_free_range(s, sz)				do { } while (0)

#define dept_map_init(m, k, su, n)			do { (void)(n); (void)(k); } while (0)
#define dept_map_reinit(m, k, su, n)			do { (void)(n); (void)(k); } while (0)
#define dept_ext_wgen_init(wg)				do { } while (0)
#define dept_map_copy(t, f)				do { } while (0)
#define dept_wait(m, w_f, ip, w_fn, sl, t)		do { (void)(w_fn); } while (0)
#define dept_stage_wait(m, k, ip, w_fn, t)		do { (void)(k); (void)(w_fn); } while (0)
#define dept_request_event_wait_commit()		do { } while (0)
#define dept_clean_stage()				do { } while (0)
#define dept_ttwu_stage_wait(t, ip)			do { } while (0)
#define dept_ecxt_enter(m, e_f, ip, c_fn, e_fn, sl)	do { (void)(c_fn); (void)(e_fn); } while (0)
#define dept_ecxt_holding(m, e_f)			false
#define dept_request_event(m, wg)			do { } while (0)
#define dept_event(m, e_f, ip, e_fn, wg)		do { (void)(e_fn); } while (0)
#define dept_ecxt_exit(m, e_f, ip)			do { } while (0)
#define dept_sched_enter()				do { } while (0)
#define dept_sched_exit()				do { } while (0)
#define dept_update_cxt()				do { } while (0)
#define dept_ecxt_enter_nokeep(m)			do { } while (0)
#define dept_key_init(k)				do { (void)(k); } while (0)
#define dept_key_destroy(k)				do { (void)(k); } while (0)
#define dept_map_ecxt_modify(m, e_f, n_k, n_e_f, n_ip, n_c_fn, n_e_fn, n_sl) do { (void)(n_k); (void)(n_c_fn); (void)(n_e_fn); } while (0)

#define dept_softirq_enter()				do { } while (0)
#define dept_hardirq_enter()				do { } while (0)
#define dept_softirqs_on_ip(ip)				do { } while (0)
#define dept_hardirqs_on()				do { } while (0)
#define dept_softirqs_off()				do { } while (0)
#define dept_hardirqs_off()				do { } while (0)
#endif
#endif /* __LINUX_DEPT_H */
