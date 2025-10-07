// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 *
 * This code is part of LunarKernel.
 * task_struct extension.
 *
 * File name: lse_task_struct_ext.c
 * Author: Cloud_Yun <1770669041@qq.com>
 * Version: v251003_Dev
 * Date: 2025/10/3 Friday
 */

#include <linux/smp.h>
#include <linux/rwsem.h>
#include <trace/hooks/sched.h>
#include <../kernel/sched/sched.h>

#include "include/lse_task_struct_ext.h"

static struct kmem_cache *lse_task_struct_cachep;

static void init_lse_task_struct(struct lse_task_struct *lts, struct task_struct *tsk)
{
    memset(lts, 0, sizeof(struct lse_task_struct));
    lts->task = tsk;
/*#ifdef CONFIG_HMBIRD_SCHED_GKI*/
	INIT_LIST_HEAD(&lts->lse.dsq_node.fifo);
	RB_CLEAR_NODE(&lts->lse.dsq_node.priq);
	lts->lse.sticky_cpu = -1;
	lts->lse.runnable_at = INITIAL_JIFFIES;
	lts->lse.gdsq_idx = DEFAULT_CGROUP_DL_IDX;
/*#endif*/
}

static void alloc_lse_task_struct(void *unused, struct task_struct *tsk,
                                     struct task_struct *orig)
{
    struct lse_task_struct *lts;

    if (!tsk)
        return;

    if (smp_load_acquire(&tsk->android_vendor_data1[LTS_IDX]) != 0)
        return;

    lts = kmem_cache_alloc(lse_task_struct_cachep, GFP_ATOMIC);
    if (!lts)
        return;

    init_lse_task_struct(lts, tsk);

    smp_store_release(&tsk->android_vendor_data1[LTS_IDX], (u64)lts);
}

static void free_lse_task_struct(void *unused, struct task_struct *tsk)
{
    struct lse_task_struct *lts;

    if (!tsk)
        return;

    lts = (struct lse_task_struct *)smp_load_acquire(&tsk->android_vendor_data1[LTS_IDX]);
    if (!lts)
        return;

    smp_store_release(&tsk->android_vendor_data1[LTS_IDX], 0);

    kmem_cache_free(lse_task_struct_cachep, lts);
}

/*#ifdef CONFIG_HMBIRD_SCHED_GKI*/
static void lse_sched_fork(void *unused, struct task_struct *p)
{
	struct lse_task_struct *lts = get_lse_task_struct(p);
	struct lse_task_struct *curr_lts = get_lse_task_struct(current);
	if (!lts)
		return;

	//lts->lse.dsq = NULL;
	INIT_LIST_HEAD(&lts->lse.dsq_node.fifo);
	RB_CLEAR_NODE(&lts->lse.dsq_node.priq);
	lts->lse.flags = 0;
	lts->lse.dsq_flags = 0;
	lts->lse.sticky_cpu = -1;
	lts->lse.runnable_at = INITIAL_JIFFIES;
	lts->lse.slice = LSE_SLICE_DFL;
	lts->lse.sched_prop = 0;
	lts->lse.ext_flags = 0;
	lts->lse.prio_backup = 0;
	lts->lse.gdsq_idx = DEFAULT_CGROUP_DL_IDX;
	memset(&lts->lse.lts, 0, sizeof(struct lse_task_stats));
	if (curr_lts) {
		if ((curr_lts->lse.ext_flags & EXT_FLAG_RT_CHANGED) && !p->sched_reset_on_fork) {
			lts->lse.ext_flags |= EXT_FLAG_RT_CHANGED;
			lts->lse.prio_backup = curr_lts->lse.prio_backup;
		}
		if (curr_lts->lse.ext_flags & EXT_FLAG_CFS_CHANGED)
			lts->lse.ext_flags |= EXT_FLAG_CFS_CHANGED;
	}
}
/*#endif*/

static void alloc_lts_mem_for_all_threads(void)
{
    struct task_struct *p, *g;
    u32 iter_cpu;

    read_lock(&tasklist_lock);
    for_each_process_thread(g, p) {
        struct lse_task_struct *lts = NULL;

        lts = (struct lse_task_struct *)smp_load_acquire(&p->android_vendor_data1[LTS_IDX]);

        if (!lts) {
            lts = kmem_cache_alloc(lse_task_struct_cachep, GFP_ATOMIC);

            if (lts) {
                init_lse_task_struct(lts, p);
                smp_store_release(&p->android_vendor_data1[LTS_IDX], (u64)lts);
            }
        }
    }

    for_each_possible_cpu(iter_cpu) {
        struct lse_task_struct *lts = NULL;

        p = cpu_rq(iter_cpu)->idle;
        lts = (struct lse_task_struct *)smp_load_acquire(&p->android_vendor_data1[LTS_IDX]);

        if (!lts) {
            lts = kmem_cache_alloc(lse_task_struct_cachep, GFP_ATOMIC);

            if (lts) {
                init_lse_task_struct(lts, p);
                smp_store_release(&p->android_vendor_data1[LTS_IDX], (u64)lts);
            }
        }
    }
    read_unlock(&tasklist_lock);
}

int lse_task_struct_ext_init(void)
{
    lse_task_struct_cachep = kmem_cache_create("lse_task_struct",
            sizeof(struct lse_task_struct), 0,
            SLAB_PANIC | SLAB_ACCOUNT, NULL);

    if (!lse_task_struct_cachep)
        return -ENOMEM;

    alloc_lts_mem_for_all_threads();

    register_trace_android_vh_dup_task_struct(alloc_lse_task_struct, NULL);
    register_trace_android_vh_free_task(free_lse_task_struct, NULL);
/*#ifdef CONFIG_HMBIRD_SCHED_GKI*/
    register_trace_android_rvh_sched_fork(lse_sched_fork, NULL);
/*#endif*/

    return 0;
}
