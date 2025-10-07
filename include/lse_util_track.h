// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 */

#ifndef _LSE_UTIL_TRACK_H_
#define _LSE_UTIL_TRACK_H_

#include <linux/percpu.h>

/*Sysctl related interface*/
#define WINDOW_STATS_RECENT		0
#define WINDOW_STATS_MAX		1
#define WINDOW_STATS_MAX_RECENT_AVG	2
#define WINDOW_STATS_AVG		3
#define WINDOW_STATS_INVALID_POLICY	4

#define LSE_SCHED_CAPACITY_SHIFT  10
#define SCHED_ACCOUNT_WAIT_TIME 0

enum task_event {
	PUT_PREV_TASK   = 0,
	PICK_NEXT_TASK  = 1,
	TASK_WAKE       = 2,
	TASK_MIGRATE    = 3,
	TASK_UPDATE     = 4,
	IRQ_UPDATE      = 5,
};

struct lse_dsq_stats {
	u64	cumulative_runnable_avg_scaled;
	int	nr_period_tasks;
	int	nr_tasks;
};

struct lse_sched_rq_stats {
	u64			window_start;
	u64			latest_clock;
	u32			prev_window_size;
	u64			task_exec_scale;
	u64			prev_runnable_sum;
	u64			curr_runnable_sum;
	int			iso_idx;
	struct lse_dsq_stats	local_dsq_s;
};
DECLARE_PER_CPU(struct lse_sched_rq_stats, lse_sched_rq_stats);

extern atomic64_t lse_run_rollover_lastq_ws;
extern int sched_window_stats_policy;
extern u64 tick_sched_clock;
extern int lse_sched_ravg_window;
extern int new_lse_sched_ravg_window;
extern unsigned int lse_scale_demand_divisor;
extern spinlock_t new_sched_ravg_window_lock;
extern void lse_update_task_ravg(struct lse_entity *lse, struct task_struct *p, struct rq *rq, int event, u64 wallclock);
extern u16 lse_cpu_util(int cpu);
extern void lse_sched_stats_init(void);
extern void sched_ravg_window_change(int frame_per_sec);

/*util = runtime * 1024 / window_size */
static inline u64 lse_scale_time_to_util(u64 d)
{
	do_div(d, lse_scale_demand_divisor);
	return d;
}

static inline void lse_fixup_window_dep(void)
{
	lse_scale_demand_divisor = lse_sched_ravg_window >> SCHED_CAPACITY_SHIFT;
}

#endif /* _LSE_UTIL_TRACK_H_ */
