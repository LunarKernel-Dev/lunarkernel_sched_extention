// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 */

#ifndef _LSE_TASK_STRUCT_EXT_H_
#define _LSE_TASK_STRUCT_EXT_H_

#include <linux/slab.h>

#define lts_to_ts(lts)	(lts->task)
#define LTS_IDX 63

/*#ifdef CONFIG_HMBIRD_SCHED_GKI*/
#define RAVG_HIST_SIZE 	5
#define LSE_SLICE_DFL 	(1 * NSEC_PER_MSEC)
#define LSE_SLICE_INF	U64_MAX
#define DEFAULT_CGROUP_DL_IDX (8)
#define EXT_FLAG_RT_CHANGED  	(1 << 0)
#define EXT_FLAG_CFS_CHANGED 	(1 << 1)
struct lse_task_stats {
	u64	mark_start;
	u64	window_start;
	u32	sum;
	u32	sum_history[RAVG_HIST_SIZE];
	int	cidx;
	u32	demand;
	u16	demand_scaled;
	void	*sdsq;
};
/*
 * The following is embedded in task_struct and contains all fields necessary
 * for a task to be scheduled by LSE.
 */
struct lse_entity {
	//struct scx_dispatch_q	*dsq;
	struct {
		struct list_head	fifo;	/* dispatch order */
		struct rb_node		priq;	/* p->lse.dsq_vtime order */
	} dsq_node;
	u32			flags;		/* protected by rq lock */
	u32			dsq_flags;	/* protected by dsq lock */
	s32			sticky_cpu;
	unsigned long		runnable_at;
	u64			slice;
	u64			dsq_vtime;
	int			gdsq_idx;
	int 			ext_flags;
	int 			prio_backup;
	unsigned long		sched_prop;
	struct lse_task_stats 	lts;
};
/*#endif*/

struct lse_task_struct {
	struct task_struct *task;
/*#ifdef CONFIG_HMBIRD_SCHED_GKI*/
    struct lse_entity lse;
/*#endif*/
} ____cacheline_aligned;

static inline struct lse_task_struct *get_lse_task_struct(struct task_struct *t)
{
    struct lse_task_struct *lts = NULL;

    if (!t)
        return NULL;

    lts = (struct lse_task_struct *)smp_load_acquire(&t->android_vendor_data1[LTS_IDX]);
    if (!lts)
        return NULL;
    
    return lts;
}

extern int lse_task_struct_ext_init(void);

#endif /* _LSE_TASK_STRUCT_EXT_H_ */
