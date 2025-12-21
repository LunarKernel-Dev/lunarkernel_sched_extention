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

#include "lse_main.h"

static inline u64 lse_rq_clock(struct rq *rq)
{
	struct lse_rq *lrq = &per_cpu(lse_rq, cpu_of(rq));

	if (unlikely(lse_clock_suspended))
		return lse_clock_last;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
	if (unlikely(!raw_spin_is_locked(&rq->__lock)))
#else
	if (unlikely(!raw_spin_is_locked(&rq->lock)))
#endif
		LSE_BUG("on CPU%d: %s task %s(%d) unlocked access"
				 "for cpu=%d stack[%pS <== %pS <== %pS]\n",
				 raw_smp_processor_id(), __func__,
				 current->comm, current->pid, rq->cpu,
				 (void *)CALLER_ADDR0,
				 (void *)CALLER_ADDR1, (void *)CALLER_ADDR2);

	if (!(rq->clock_update_flags & RQCF_UPDATED))
		update_rq_clock(rq);

	return max(rq_clock(rq), lrq->latest_clock);
}

void lse_scheduler_tick(void)
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
		    struct lse_rq *lrq = &per_cpu(lse_rq, cpu);

			lrq->window_start = tick_sched_clock;
		}

		atomic64_set(&lse_run_rollover_lastq_ws, tick_sched_clock);
	}
}

static void lse_scheduler_tick_cb(void *unused, struct rq *rq)
{
	lse_scheduler_tick();
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
static void lse_schedule(void *unused, unsigned int sched_mode, struct task_struct *prev,
	            struct task_struct *next, struct rq *rq)
#else
static void lse_schedule(void *unused, struct task_struct *prev, struct task_struct *next,
	            struct rq *rq)
#endif
{
	struct lse_task_struct *prev_lts, *next_lts;

	if (!slim_walt_ctrl)
		return;

	prev_lts = get_lse_task_struct(prev);
	if (likely(prev != next)) {
		next_lts = get_lse_task_struct(next);

		if (prev_lts)
			lse_update_task_ravg(prev_lts, prev, rq, PUT_PREV_TASK, lse_rq_clock(rq));

		if (next_lts)
			lse_update_task_ravg(next_lts, next, rq, PICK_NEXT_TASK, lse_rq_clock(rq));
	} else if (prev_lts)
		lse_update_task_ravg(prev_lts, prev, rq, TASK_UPDATE, lse_rq_clock(rq));
}

void lse_tick_entry(void *unused, struct rq *rq)
{
	struct lse_task_struct *curr_lts;

	if (!slim_walt_ctrl)
		return;

	curr_lts = get_lse_task_struct(rq->curr);
	if (curr_lts)
		lse_update_task_ravg(curr_lts, rq->curr, rq, TASK_UPDATE, lse_rq_clock(rq));
}

void lse_cfs_hooks_register(void)
{
    register_trace_android_vh_scheduler_tick(lse_scheduler_tick_cb, NULL);
    register_trace_android_rvh_schedule(lse_schedule, NULL);
    register_trace_android_rvh_tick_entry(lse_tick_entry, NULL);
}
