/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Runtime Verification.
 *
 * For futher information, see: kernel/trace/rv/rv.c.
 */
#ifndef _LINUX_RV_H
#define _LINUX_RV_H

#define MAX_DA_NAME_LEN	24

#ifdef CONFIG_RV
#include <linux/types.h>

/*
 * Deterministic automaton per-object variables.
 */
struct da_monitor {
	bool		monitoring;
	unsigned int	curr_state;
};

enum ltl_truth_value {
	LTL_FALSE,
	LTL_TRUE,
	LTL_UNDETERMINED,
};

/*
 * In the future, if the number of atomic propositions or the custom data size is larger, we can
 * switch to dynamic allocation. For now, the code is simpler this way.
 */
#define RV_MAX_LTL_ATOM 10
#define RV_MAX_DATA_SIZE 16
struct ltl_monitor {
	unsigned int		state;
	enum ltl_truth_value	atoms[RV_MAX_LTL_ATOM];
	u8			data[RV_MAX_DATA_SIZE];
};

/*
 * Per-task RV monitors count. Nowadays fixed in RV_PER_TASK_MONITORS.
 * If we find justification for more monitors, we can think about
 * adding more or developing a dynamic method. So far, none of
 * these are justified.
 */
#define RV_PER_TASK_MONITORS		2
#define RV_PER_TASK_MONITOR_INIT	(RV_PER_TASK_MONITORS)

union rv_task_monitor {
	struct da_monitor	da_mon;
	struct ltl_monitor	ltl_mon;
};

#ifdef CONFIG_RV_REACTORS
struct rv_reactor {
	const char		*name;
	const char		*description;
	void			(*react)(const char *msg, ...);
};
#endif

struct rv_monitor {
	const char		*name;
	const char		*description;
	bool			enabled;
	int			(*enable)(void);
	void			(*disable)(void);
	void			(*reset)(void);
	void			(*react)(const char *msg, ...);
};

bool rv_monitoring_on(void);
int rv_unregister_monitor(struct rv_monitor *monitor);
int rv_register_monitor(struct rv_monitor *monitor);
int rv_get_task_monitor_slot(void);
void rv_put_task_monitor_slot(int slot);

#ifdef CONFIG_RV_REACTORS
bool rv_reacting_on(void);
int rv_unregister_reactor(struct rv_reactor *reactor);
int rv_register_reactor(struct rv_reactor *reactor);
#else
static inline bool rv_reacting_on(void)
{
	return false;
}
#endif /* CONFIG_RV_REACTORS */

#endif /* CONFIG_RV */
#endif /* _LINUX_RV_H */
