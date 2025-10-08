// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 *
 * This code is part of LunarKernel.
 * slim walt (window assisted load tracking).
 *
 * File name: lse_util_track.c
 * Author: Cloud_Yun <1770669041@qq.com>
 * Version: v251003_Dev
 * Date: 2025/10/3 Friday
 */

#include <linux/sched.h>
#include <linux/sched/cputime.h>

#include "include/lse_main.h"
#include "trace_lse.h"

static inline void window_rollover_systrace_c(void)
{
	char buf[256];
	static unsigned long window_count;

	window_count += 1;

	snprintf(buf, sizeof(buf), "C|9999|lse_window_rollover|%lu\n", window_count%2);
	lse_tracing_mark_write(buf);
}

static DEFINE_PER_CPU(u16, prev_cpu_util);
static inline void cpu_util_update_systrace_c(int cpu)
{
	char buf[256];
	u16 cpu_util = lse_cpu_util(cpu);

	if(cpu_util != per_cpu(prev_cpu_util, cpu)) {
		snprintf(buf, sizeof(buf), "C|9999|Cpu%d_util|%u\n",
						cpu, cpu_util);
		lse_tracing_mark_write(buf);
		per_cpu(prev_cpu_util, cpu) = cpu_util;
	}
}

__read_mostly int lse_sched_ravg_window = 8000000;
int new_lse_sched_ravg_window = 8000000;
DEFINE_SPINLOCK(new_sched_ravg_window_lock);
DEFINE_PER_CPU(struct lse_sched_rq_stats, lse_sched_rq_stats);

__read_mostly unsigned int lse_scale_demand_divisor;

atomic64_t lse_run_rollover_lastq_ws;
u64 tick_sched_clock;

int sched_window_stats_policy;

inline u64 scale_exec_time(u64 delta, struct rq *rq)
{
	struct lse_sched_rq_stats *lrq = &per_cpu(lse_sched_rq_stats, cpu_of(rq));

	return (delta * lrq->task_exec_scale) >> LSE_SCHED_CAPACITY_SHIFT;
}

static u64 add_to_task_demand(struct lse_entity *lse, struct rq *rq, struct task_struct *p, u64 delta)
{
	struct lse_task_stats *lts = &lse->lts;

	delta = scale_exec_time(delta, rq);
	lts->sum += delta;
	if (unlikely(lts->sum > lse_sched_ravg_window))
		lts->sum = lse_sched_ravg_window;

	return delta;
}


static int
account_busy_for_task_demand(struct rq *rq, struct task_struct *p, int event)
{
	/*
	 * No need to bother updating task demand for the idle task.
	 */
	if (is_idle_task(p))
		return 0;

	/*
	 * When a task is waking up it is completing a segment of non-busy
	 * time. Likewise, if wait time is not treated as busy time, then
	 * when a task begins to run or is migrated, it is not running and
	 * is completing a segment of non-busy time.
	 */
	if (event == TASK_WAKE || (!SCHED_ACCOUNT_WAIT_TIME &&
				(event == PICK_NEXT_TASK || event == TASK_MIGRATE)))
		return 0;

	/*
	 * The idle exit time is not accounted for the first task _picked_ up to
	 * run on the idle CPU.
	 */
	if (event == PICK_NEXT_TASK && rq->curr == rq->idle)
		return 0;

	/*
	 * TASK_UPDATE can be called on sleeping task, when its moved between
	 * related groups
	 */
	if (event == TASK_UPDATE) {
		if (rq->curr == p)
			return 1;

		return p->on_rq ? SCHED_ACCOUNT_WAIT_TIME : 0;
	}

	return 1;
}

static void rollover_cpu_window(struct rq *rq, bool full_window)
{
	struct lse_sched_rq_stats *lrq = &per_cpu(lse_sched_rq_stats, cpu_of(rq));
	u64 curr_sum = lrq->curr_runnable_sum;

	if (unlikely(full_window))
		curr_sum = 0;

	lrq->prev_runnable_sum = curr_sum;
	lrq->curr_runnable_sum = 0;
}

static u64
update_window_start(struct rq *rq, u64 wallclock)
{
	s64 delta;
	int nr_windows;
	bool full_window;

	struct lse_sched_rq_stats *lrq = &per_cpu(lse_sched_rq_stats, cpu_of(rq));
	u64 old_window_start = lrq->window_start;

	if (wallclock < lrq->latest_clock) {
		LSE_BUG("on CPU%d; wallclock=%llu(0x%llx) is lesser than latest_clock=%llu(0x%llx)",
			rq->cpu, wallclock, wallclock, lrq->latest_clock,
			lrq->latest_clock);
		wallclock = lrq->latest_clock;
	}
	delta = wallclock - lrq->window_start;
	if (delta < 0) {
		LSE_BUG("on CPU%d; wallclock=%llu(0x%llx) is lesser than window_start=%llu(0x%llx)",
			rq->cpu, wallclock, wallclock,
			lrq->window_start, lrq->window_start);
		delta = 0;
		wallclock = lrq->window_start;
	}
	lrq->latest_clock = wallclock;
	if (delta < lse_sched_ravg_window)
		return old_window_start;

	nr_windows = div64_u64(delta, lse_sched_ravg_window);
	lrq->window_start += (u64)nr_windows * (u64)lse_sched_ravg_window;

	lrq->prev_window_size = lse_sched_ravg_window;
	full_window = nr_windows > 1;
	rollover_cpu_window(rq, full_window);

	return old_window_start;
}

static inline unsigned int get_max_freq(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get_raw(cpu);

	return (policy == NULL) ? 0 : policy->cpuinfo.max_freq;
}

static inline unsigned int cpu_cur_freq(int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get_raw(cpu);

	return (policy == NULL) ? 0 : policy->cur;
}

static void
update_task_rq_cpu_cycles(struct task_struct *p, struct rq *rq, u64 wallclock)
{
	int cpu = cpu_of(rq);
	struct lse_sched_rq_stats *lrq = &per_cpu(lse_sched_rq_stats, cpu_of(rq));

	lrq->task_exec_scale = DIV64_U64_ROUNDUP(cpu_cur_freq(cpu) *
					arch_scale_cpu_capacity(cpu), get_max_freq(cpu));
}

/*
 * Called when new window is starting for a task, to record cpu usage over
 * recently concluded window(s). Normally 'samples' should be 1. It can be > 1
 * when, say, a real-time task runs without preemption for several windows at a
 * stretch.
 */
static void update_history(struct lse_entity *lse, struct rq *rq, struct task_struct *p,
			 u32 runtime, int samples, int event)
{
	struct lse_task_stats *lts = &lse->lts;
	u32 *hist = &lts->sum_history[0];
	int i;
	u32 max = 0, avg, demand;
	u64 sum = 0;
	u16 demand_scaled;
	int samples_old = samples;

	/* Ignore windows where task had no activity */
	if (!runtime || is_idle_task(p) || !samples)
		goto done;

	/* Push new 'runtime' value onto stack */
	for (; samples > 0; samples--) {
		hist[lts->cidx] = runtime;
		lts->cidx = ++(lts->cidx) % RAVG_HIST_SIZE;
	}

	for (i = 0; i < RAVG_HIST_SIZE; i++) {
		sum += hist[i];
		if (hist[i] > max)
			max = hist[i];
	}

	lts->sum = 0;
	avg = div64_u64(sum, RAVG_HIST_SIZE);

	switch (sched_window_stats_policy) {
	case WINDOW_STATS_RECENT:
		demand = runtime;
		break;
	case WINDOW_STATS_MAX:
		demand = max;
		break;
	case WINDOW_STATS_AVG:
		demand = avg;
		break;
	default:
		demand = max(avg, runtime);
	}

	demand_scaled = lse_scale_time_to_util(demand);

	lts->demand = demand;
	lts->demand_scaled = demand_scaled;

done:
	trace_lse_update_history(lse, rq, p, runtime, samples_old, event);
	return;
}


static u64
update_task_demand(struct lse_entity *lse, struct task_struct *p, struct rq *rq,
			       int event, u64 wallclock)
{
	struct lse_task_stats *lts = &lse->lts;

	u64 mark_start = lts->mark_start;
	struct lse_sched_rq_stats *lrq = &per_cpu(lse_sched_rq_stats, cpu_of(rq));


	u64 delta, window_start = lrq->window_start;
	int new_window, nr_full_windows;
	u32 window_size = lse_sched_ravg_window;
	u64 runtime;

	new_window = mark_start < window_start;
	if (!account_busy_for_task_demand(rq, p, event)) {
		if (new_window)
			/*
			 * If the time accounted isn't being accounted as
			 * busy time, and a new window started, only the
			 * previous window need be closed out with the
			 * pre-existing demand. Multiple windows may have
			 * elapsed, but since empty windows are dropped,
			 * it is not necessary to account those.
			 */
			update_history(lse, rq, p, lts->sum, 1, event);
		return 0;
	}

	if (!new_window) {
		/*
		 * The simple case - busy time contained within the existing
		 * window.
		 */
		return add_to_task_demand(lse, rq, p, wallclock - mark_start);
	}

	/*
	 * Busy time spans at least two windows. Temporarily rewind
	 * window_start to first window boundary after mark_start.
	 */
	delta = window_start - mark_start;
	nr_full_windows = div64_u64(delta, window_size);
	window_start -= (u64)nr_full_windows * (u64)window_size;

	/* Process (window_start - mark_start) first */
	runtime = add_to_task_demand(lse, rq, p, window_start - mark_start);

	/* Push new sample(s) into task's demand history */
	update_history(lse, rq, p, lts->sum, 1, event);
	if (nr_full_windows) {
		u64 scaled_window = scale_exec_time(window_size, rq);

		update_history(lse, rq, p, scaled_window, nr_full_windows, event);
		runtime += nr_full_windows * scaled_window;
	}

	/*
	 * Roll window_start back to current to process any remainder
	 * in current window.
	 */
	window_start += (u64)nr_full_windows * (u64)window_size;

	/* Process (wallclock - window_start) next */
	mark_start = window_start;
	runtime += add_to_task_demand(lse, rq, p, wallclock - mark_start);

	return runtime;
}

static inline int account_busy_for_cpu_time(struct rq *rq, struct task_struct *p,
				     int event)
{
	return !is_idle_task(p) && (event == PUT_PREV_TASK || event == TASK_UPDATE);
}


static void update_cpu_busy_time(struct lse_entity *lse, struct task_struct *p, struct rq *rq,
				 int event, u64 wallclock)
{
	int new_window, full_window = 0;
	struct lse_task_stats *lts = &lse->lts;
	u64 mark_start = lts->mark_start;
	struct lse_sched_rq_stats *lrq = &per_cpu(lse_sched_rq_stats, cpu_of(rq));
	u64 window_start = lrq->window_start;
	u32 window_size = lrq->prev_window_size;
	u64 delta;
	u64 *curr_runnable_sum = &lrq->curr_runnable_sum;
	u64 *prev_runnable_sum = &lrq->prev_runnable_sum;

	new_window = mark_start < window_start;
	if (new_window)
		full_window = (window_start - mark_start) >= window_size;


	if (!account_busy_for_cpu_time(rq, p, event))
		goto done;


	if (!new_window) {
		/*
		 * account_busy_for_cpu_time() = 1 so busy time needs
		 * to be accounted to the current window. No rollover
		 * since we didn't start a new window. An example of this is
		 * when a task starts execution and then sleeps within the
		 * same window.
		 */
		delta = wallclock - mark_start;

		delta = scale_exec_time(delta, rq);
		*curr_runnable_sum += delta;

		goto done;
	}

	/*
	 * situations below this need window rollover,
	 * Rollover of cpu counters (curr/prev_runnable_sum) should have already be done
	 * in update_window_start()
	 *
	 * For task counters curr/prev_window[_cpu] are rolled over in the early part of
	 * this function. If full_window(s) have expired and time since last update needs
	 * to be accounted as busy time, set the prev to a complete window size time, else
	 * add the prev window portion.
	 *
	 * For task curr counters a new window has begun, always assign
	 */

	/*
	 * account_busy_for_cpu_time() = 1 so busy time needs
	 * to be accounted to the current window. A new window
	 * must have been started in udpate_window_start()
	 * If any of these three above conditions are true
	 * then this busy time can't be accounted as irqtime.
	 *
	 * Busy time for the idle task need not be accounted.
	 *
	 * An example of this would be a task that starts execution
	 * and then sleeps once a new window has begun.
	 */

	/*
	 * A full window hasn't elapsed, account partial
	 * contribution to previous completed window.
	 */

	delta = full_window ? scale_exec_time(window_size, rq) :
					scale_exec_time(window_start - mark_start, rq);

	*prev_runnable_sum += delta;

	/* Account piece of busy time in the current window. */
	delta = scale_exec_time(wallclock - window_start, rq);
	*curr_runnable_sum += delta;

done:
	if((dump_info & LSE_DEBUG_SYSTRACE) && new_window)
		cpu_util_update_systrace_c(rq->cpu);
}

void lse_window_rollover_run_once(u64 old_window_start, struct rq *rq)
{
	u64 result;
	struct lse_sched_rq_stats *lrq = &per_cpu(lse_sched_rq_stats, cpu_of(rq));
	u64 new_window_start = lrq->window_start;

	if (old_window_start == new_window_start)
		return;

	result = atomic64_cmpxchg(&lse_run_rollover_lastq_ws, old_window_start, new_window_start);

	if (result != old_window_start)
		return;
	run_lse_irq_work_rollover();
	trace_lse_run_window_rollover(old_window_start, new_window_start);
	if (dump_info & LSE_DEBUG_SYSTRACE)
		window_rollover_systrace_c();
}

void lse_update_task_ravg(struct lse_entity *lse, struct task_struct *p, struct rq *rq, int event, u64 wallclock)
{
	struct lse_task_stats *lts = &lse->lts;
	struct lse_sched_rq_stats *lrq = &per_cpu(lse_sched_rq_stats, cpu_of(rq));
	u64 old_window_start;

	if(!slim_walt_ctrl)
		return;

	if(!lrq->window_start || lts->mark_start == wallclock)
		return;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
	if (unlikely(!raw_spin_is_locked(&rq->__lock)))
#else
	if (unlikely(!raw_spin_is_locked(&rq->lock)))
#endif
		LSE_BUG("on CPU%d: %s task %s(%d) unlocked access"
				 "for cpu=%d stack[%pS <== %pS <== %pS]\n",
				 raw_smp_processor_id(), __func__,
				 p->comm, p->pid, rq->cpu,
				 (void *)CALLER_ADDR0,
				 (void *)CALLER_ADDR1, (void *)CALLER_ADDR2);

	old_window_start = update_window_start(rq, wallclock);

	if(!lts->window_start)
		lts->window_start = lrq->window_start;

	if(!lts->mark_start)
		goto done;

	update_task_rq_cpu_cycles(p, rq, wallclock);
	update_task_demand(lse, p, rq, event, wallclock);
	update_cpu_busy_time(lse, p, rq, event, wallclock);

	lts->window_start = lrq->window_start;

done:
	lts->mark_start = wallclock;

	if (lts->mark_start > (lts->window_start + lse_sched_ravg_window))
		LSE_BUG("CPU%d: %s task %s(%d)'s ms=%llu is ahead of ws=%llu by more than 1 window on rq=%d event=%d\n",
			raw_smp_processor_id(), __func__, p->comm, p->pid,
			lts->mark_start, lts->window_start, rq->cpu, event);

	lse_window_rollover_run_once(old_window_start, rq);
}

u16 lse_cpu_util(int cpu)
{
	u64 prev_runnable_sum;
	struct lse_sched_rq_stats *lrq = &per_cpu(lse_sched_rq_stats, cpu);

	prev_runnable_sum = lrq->prev_runnable_sum;
	do_div(prev_runnable_sum, lrq->prev_window_size >> LSE_SCHED_CAPACITY_SHIFT);

	return (u16)prev_runnable_sum;
}

static void lse_sched_init_rq(struct rq *rq)
{
	struct lse_sched_rq_stats *lrq = &per_cpu(lse_sched_rq_stats, cpu_of(rq));

	lrq->local_dsq_s.nr_period_tasks = 0;
	lrq->local_dsq_s.nr_tasks = 0;
	lrq->local_dsq_s.cumulative_runnable_avg_scaled = 0;
	lrq->prev_window_size = lse_sched_ravg_window;
	lrq->task_exec_scale = 1024;
	lrq->window_start = 0;
}

void lse_sched_stats_init(void)
{
	int cpu;
	unsigned long flags;

	for_each_possible_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
		raw_spin_lock_irqsave(&rq->__lock, flags);
#else
		raw_spin_lock_irqsave(&rq->lock, flags);
#endif
		lse_sched_init_rq(rq);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
		raw_spin_unlock_irqrestore(&rq->__lock, flags);
#else
		raw_spin_unlock_irqrestore(&rq->lock, flags);
#endif
	}
	sched_window_stats_policy = WINDOW_STATS_MAX_RECENT_AVG;
}

void sched_ravg_window_change(int frame_per_sec)
{
	unsigned long flags;

	spin_lock_irqsave(&new_sched_ravg_window_lock, flags);
	new_lse_sched_ravg_window = NSEC_PER_SEC / frame_per_sec;
	spin_unlock_irqrestore(&new_sched_ravg_window_lock, flags);
}
