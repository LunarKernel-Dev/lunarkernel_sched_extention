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
#include <linux/version.h>
#include <linux/list_sort.h>
#include <linux/notifier.h>
#include <linux/slab.h>
#include <linux/sched/clock.h>
#include <linux/sched/cputime.h>
#include <trace/hooks/sched.h>

#include <../kernel/sched/sched.h>
#include <../kernel/time/tick-sched.h>

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


/*Sysctl related interface*/
#define WINDOW_STATS_RECENT		0
#define WINDOW_STATS_MAX		1
#define WINDOW_STATS_MAX_RECENT_AVG	2
#define WINDOW_STATS_AVG		3
#define WINDOW_STATS_INVALID_POLICY	4

#define LSE_SCHED_CAPACITY_SHIFT  10
#define SCHED_ACCOUNT_WAIT_TIME 0

#define lts_to_ts(lts)	(lts->task)
#define LTS_IDX 63
#define RAVG_HIST_SIZE 	5

#define DEFAULT_SCHED_RAVG_WINDOW 8000000
#define MAX_LSE_CLUSTERS 4

enum task_event {
	PUT_PREV_TASK   = 0,
	PICK_NEXT_TASK  = 1,
	TASK_WAKE       = 2,
	TASK_MIGRATE    = 3,
	TASK_UPDATE     = 4,
	IRQ_UPDATE      = 5,
};

struct lse_task_struct {
	struct task_struct *task;

	u64	mark_start;
	u64	window_start;
	u32	sum;
	u32	sum_history[RAVG_HIST_SIZE];
	int	cidx;
	u32	demand;
	u16	demand_scaled;
} ____cacheline_aligned;

struct lse_sched_cluster {
	raw_spinlock_t load_lock;
	struct list_head list;
	struct cpumask cpus;
	int id;
	int max_possible_capacity;
	unsigned int cur_freq, max_freq, min_freq;
	unsigned int capacity_margin;
	unsigned int sd_capacity_margin;
	struct notifier_block nb_min;
	struct notifier_block nb_max;
	bool freq_init_done;
};

struct lse_rq {
	struct lse_sched_cluster *cluster;
    struct cpumask freq_domain_cpumask;

	u64			window_start;
	u64			latest_clock;
	u32			prev_window_size;
	u64			task_exec_scale;
	u64			prev_runnable_sum;
	u64			curr_runnable_sum;
};
DECLARE_PER_CPU(struct lse_rq, lse_rq);

/* lse_main.c */
extern bool lse_clock_suspended;
extern u64 lse_clock_last;
extern unsigned int dump_info;
extern noinline int lse_tracing_mark_write(const char *buf);
extern u64 lse_sched_clock(void);

/* lse_task_struct_ext.c */
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

/* lse_sched_cluster.c */
extern int lse_num_sched_clusters;
extern struct list_head lse_cluster_head;
extern void lse_sched_cluster_init(void);

/* Iterate in increasing order of cluster max possible capacity */
#define for_each_lse_cluster(cluster) \
	list_for_each_entry(cluster, &lse_cluster_head, list)

#define for_each_lse_cluster_reverse(cluster) \
	list_for_each_entry_reverse(cluster, &lse_cluster_head, list)

#define min_cap_cluster()	\
	list_first_entry(&lse_cluster_head, struct lse_sched_cluster, list)
#define max_cap_cluster()	\
	list_last_entry(&lse_cluster_head, struct lse_sched_cluster, list)


/* lse_sysctl.c */
extern int slim_walt_ctrl;
extern int frame_per_sec;
extern void lse_sysctl_init(void);

/* lse_util_track.c */
extern atomic64_t lse_run_rollover_lastq_ws;
extern int sched_window_stats_policy;
extern u64 tick_sched_clock;
extern unsigned int lse_sched_ravg_window;
extern unsigned int new_lse_sched_ravg_window;
extern unsigned int lse_scale_demand_divisor;
extern spinlock_t new_sched_ravg_window_lock;
extern void lse_update_task_ravg(struct lse_task_struct *lts, struct task_struct *p, struct rq *rq, int event, u64 wallclock);
extern u16 lse_cpu_util(int cpu);
extern void lse_sched_stats_init(void);
extern void sched_ravg_window_change(int frame_per_sec);

/*util = runtime * 1024 / window_size */
static inline u64 lse_scale_time_to_util(u64 d)
{
	/*
	 * The denominator at most could be (8 * tick_size) >> SCHED_CAPACITY_SHIFT,
	 * a value that easily fits a 32bit integer.
	 */
	do_div(d, lse_scale_demand_divisor);
	return d;
}

static inline void lse_fixup_window_dep(void)
{
	lse_scale_demand_divisor = lse_sched_ravg_window >> SCHED_CAPACITY_SHIFT;
}


/* lse_cfs.c */
extern void lse_scheduler_tick(void);
extern void lse_tick_entry(void *unused, struct rq *rq);
extern void lse_cfs_hooks_register(void);

/* cpufreq_lse.c */
extern unsigned int sysctl_lse_gov_debug;
extern void run_lse_irq_work_rollover(void);
extern int lse_cpufreq_init(void);

#endif /* _LSE_MAIN_H_ */
