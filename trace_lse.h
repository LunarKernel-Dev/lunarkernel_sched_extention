// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM lse

#if !defined(_TRACE_LSE_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_LSE_H_

#include <linux/sched.h>
#include <linux/types.h>
#include <linux/tracepoint.h>

#include "include/lse_main.h"

TRACE_EVENT(lse_update_history,

	TP_PROTO(struct lse_entity *lse, struct rq *rq, struct task_struct *p, u32 runtime, int samples, int event),

	TP_ARGS(lse, rq, p, runtime, samples, event),

	TP_STRUCT__entry(
		__array(char, comm, TASK_COMM_LEN)
		__field(pid_t, pid)
		__field(unsigned int, runtime)
		__field(int, samples)
		__field(int, event)
		__field(unsigned int, demand)
		__array(u32, hist, RAVG_HIST_SIZE)
		__field(u16, task_util)
		__field(int, cpu)),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid = p->pid;
		__entry->runtime = runtime;
		__entry->samples = samples;
		__entry->event = event;
		__entry->demand = lse->lts.demand;
		memcpy(__entry->hist, lse->lts.sum_history, RAVG_HIST_SIZE * sizeof(u32));
		__entry->task_util = lse->lts.demand_scaled,
		__entry->cpu = rq->cpu;),

	TP_printk("comm=%s[%d]: runtime %u samples %d event %d demand %u (hist: %u %u %u %u %u) task_util %u cpu %d",
		__entry->comm, __entry->pid,
		__entry->runtime, __entry->samples,
		__entry->event,
		__entry->demand,
		__entry->hist[0], __entry->hist[1],
		__entry->hist[2], __entry->hist[3],
		__entry->hist[4],
		__entry->task_util,
		__entry->cpu)
);

TRACE_EVENT(lse_run_window_rollover,

	TP_PROTO(u64 old_window_start, u64 new_window_start),

	TP_ARGS(old_window_start, new_window_start),

	TP_STRUCT__entry(
		__field(u64, old_window_start)
		__field(u64, new_window_start)
		__field(int, cpu)),

	TP_fast_assign(
		__entry->old_window_start = old_window_start;
		__entry->new_window_start = new_window_start;
		__entry->cpu = raw_smp_processor_id();),

	TP_printk("old_window_start=%llu new_window_start=%llu cpu=%d",
		__entry->old_window_start,
		__entry->new_window_start,
		__entry->cpu)
);

#endif /*_TRACE_LSE_H_ */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace_lse
/* This part must be outside protection */
#include <trace/define_trace.h>
