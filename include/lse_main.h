// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 */

#ifndef _LSE_MAIN_H_
#define _LSE_MAIN_H_

#include <asm/processor.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/cgroup-defs.h>
#include <linux/kernel.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/cgroup.h>
#include <linux/tick.h>
#include <linux/kmemleak.h>
#include <linux/percpu.h>
#include <linux/sched/clock.h>

#include <trace/hooks/cpufreq.h>
#include <trace/hooks/sched.h>

#include <../kernel/sched/sched.h>
#include <../kernel/time/tick-sched.h>

#include "cpufreq_lse.h"
#include "lse_task_struct_ext.h"
#include "lse_sched_cluster.h"
#include "lse_sysctl.h"
#include "lse_util_track.h"
#include "lse_cfs.h"

#define LSE_DEBUG_FTRACE		(1 << 0)
#define LSE_DEBUG_SYSTRACE		(1 << 1)
#define LSE_DEBUG_PRINTK		(1 << 2)
#define LSE_DEBUG_PANIC			(1 << 3)

#define LSE_BUG(fmt, ...)		\
do {										\
	printk_deferred("lunar_sched_ext[%s]:"fmt, __func__, ##__VA_ARGS__);	\
	if (dump_info & LSE_DEBUG_PANIC)			\
		BUG_ON(-1);								\
} while (0)

#define DIV64_U64_ROUNDUP(X, Y) div64_u64((X) + (Y - 1), Y)

static inline struct lse_entity *get_lunar_ext_entity(struct task_struct *p)
{
	struct lse_task_struct *lts = get_lse_task_struct(p);
	if (!lts) {
		WARN_ONCE(1, "lse_sched_ext:get_lunar_ext_entity NULL!");
		return NULL;
	}
	return &lts->lse;
}

extern bool lse_clock_suspended;
extern u64 lse_clock_last;
extern unsigned int dump_info;
extern noinline int lse_tracing_mark_write(const char *buf);

static inline u64 lse_sched_clock(void)
{
	if (unlikely(lse_clock_suspended))
		return lse_clock_last;
	return sched_clock();
}

static inline u64 lse_rq_clock(struct rq *rq)
{
	struct lse_sched_rq_stats *lrq = &per_cpu(lse_sched_rq_stats, cpu_of(rq));

	if (unlikely(lse_clock_suspended))
		return lse_clock_last;

	if (unlikely(!raw_spin_is_locked(&rq->__lock)))
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

#endif /* _LSE_MAIN_H_ */
