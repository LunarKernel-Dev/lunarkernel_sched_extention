// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 *
 * This code is part of LunarKernel.
 * cfs hooks.
 *
 * File name: lse_cfs.c
 * Author: Cloud_Yun <1770669041@qq.com>
 * Version: v251004_Dev
 * Date: 2025/10/4 Saturday
 */

#include <trace/hooks/sched.h>

#include "include/lse_main.h"

void lse_scheduler_tick(void *unused)
{
	int cpu = smp_processor_id();
	struct rq *rq = cpu_rq(cpu);

	if (!slim_walt_ctrl)
		return;

	if (unlikely(!tick_sched_clock)) {
		/*
		 * Let the window begin 20us prior to the tick,
		 * that way we are guaranteed a rollover when the tick occurs.
		 * Use rq->clock directly instead of rq_clock() since
		 * we do not have the rq lock and
		 * rq->clock was updated in the tick callpath.
		 */
		if (cmpxchg64(&tick_sched_clock, 0, rq->clock - 20000))
			return;
		for_each_possible_cpu(cpu) {
		    struct lse_sched_rq_stats *srq = &per_cpu(lse_sched_rq_stats, cpu);

			srq->window_start = tick_sched_clock;
		}
		atomic64_set(&lse_run_rollover_lastq_ws, tick_sched_clock);
	}
}

static void lse_scheduler_tick_cb(void *unused, struct rq *rq)
{
	lse_scheduler_tick(NULL);
}

static void lse_schedule(void *unused, unsigned int sched_mode, struct task_struct *prev,
	            struct task_struct *next, struct rq *rq)
{
	struct lse_entity *prev_lse, *next_lse;

	if (!slim_walt_ctrl)
		return;

	prev_lse = get_lunar_ext_entity(prev);
	if (likely(prev != next)) {
		next_lse = get_lunar_ext_entity(next);

		if (prev_lse)
			lse_update_task_ravg(prev_lse, prev, rq, PUT_PREV_TASK, lse_rq_clock(rq));

		if (next_lse)
			lse_update_task_ravg(next_lse, next, rq, PICK_NEXT_TASK, lse_rq_clock(rq));
	} else if (prev_lse)
		lse_update_task_ravg(prev_lse, prev, rq, TASK_UPDATE, lse_rq_clock(rq));
}

void lse_tick_entry(void *unused, struct rq *rq)
{
	struct lse_entity *curr_lse;

	if (!slim_walt_ctrl)
		return;

	curr_lse = get_lunar_ext_entity(rq->curr);
	if (curr_lse)
		lse_update_task_ravg(curr_lse, rq->curr, rq, TASK_UPDATE, lse_rq_clock(rq));
}

void lse_cfs_hooks_register(void)
{
    register_trace_android_vh_scheduler_tick(lse_scheduler_tick_cb, NULL);
    register_trace_android_rvh_schedule(lse_schedule, NULL);
    register_trace_android_rvh_tick_entry(lse_tick_entry, NULL);
}
