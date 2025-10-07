// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 *
 * This code is part of LunarKernel.
 * module init.
 *
 * File name: lse_main.c
 * Author: Cloud_Yun <1770669041@qq.com>
 * Version: v251003_Dev
 * Date: 2025/10/3 Friday
 */

#include <linux/module.h>
#include <linux/syscore_ops.h>

#include "include/lse_main.h"

#define CREATE_TRACE_POINTS
#include "trace_lse.h"

bool lse_clock_suspended;
u64 lse_clock_last;
unsigned int dump_info = LSE_DEBUG_PANIC;

noinline int lse_tracing_mark_write(const char *buf)
{
	trace_printk(buf);
	return 0;
}

static void lse_resume(void)
{
	lse_clock_suspended = false;
}

static int lse_suspend(void)
{
	lse_clock_last = sched_clock();
	lse_clock_suspended = true;
	return 0;
}

static struct syscore_ops lse_syscore_ops = {
	.resume		= lse_resume,
	.suspend	= lse_suspend
};

static int __init lunar_sched_ext_init(void)
{
    register_syscore_ops(&lse_syscore_ops);
    lse_task_struct_ext_init();
	lse_sched_cluster_init();
    lse_sysctl_init();
	lse_sched_stats_init();
    lse_cfs_hooks_register();
    lse_cpufreq_init();
	return 0;
}

module_init(lunar_sched_ext_init);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Cloud_Yun <1770669041@qq.com>");
MODULE_DESCRIPTION("LunarKernel Scheduling Extention");
