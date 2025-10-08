// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 *
 * This code is part of LunarKernel.
 * sysctl interface.
 *
 * File name: lse_sysctl.c
 * Author: Cloud_Yun <1770669041@qq.com>
 * Version: v251003_Dev
 * Date: 2025/10/3 Friday
 */

#include <linux/kernel.h>
#include <linux/sysctl.h>

#include "include/lse_main.h"

int slim_walt_ctrl = 1;
int frame_per_sec = 120;

static int window_stats_policy_minval = WINDOW_STATS_RECENT;
static int window_stats_policy_maxval = WINDOW_STATS_INVALID_POLICY;

static int lse_proc_sched_ravg_window_update(struct ctl_table *table,
				int write, void __user *buffer, size_t *lenp,
				loff_t *ppos)
{
	int ret = -EPERM;
	int val;
	static DEFINE_MUTEX(mutex);

	struct ctl_table tmp = {
		.data	= &val,
		.maxlen	= sizeof(val),
		.mode	= table->mode,
	};

	mutex_lock(&mutex);

	val = frame_per_sec;
	ret = proc_dointvec(&tmp, write, buffer, lenp, ppos);
	if (ret || !write || (val == frame_per_sec))
		goto unlock;
	frame_per_sec = val;

	sched_ravg_window_change(frame_per_sec);

unlock:
	mutex_unlock(&mutex);
	return ret;
}

struct ctl_table lse_table[] = {
    {
        .procname     = "slim_walt_ctrl",
        .data         = &slim_walt_ctrl,
        .maxlen       = sizeof(int),
        .mode         = 0644,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
        .proc_handler = proc_dobool,
#else
        .proc_handler = proc_dointvec,
#endif
    },
    {
        .procname     = "slim_walt_policy",
        .data         = &sched_window_stats_policy,
        .maxlen       = sizeof(int),
        .mode         = 0644,
        .proc_handler = proc_dointvec_minmax,
        .extra1       = &window_stats_policy_minval,
        .extra2       = &window_stats_policy_maxval,
    },
    {
        .procname     = "sched_ravg_window_frame_per_sec",
        .data         = &frame_per_sec,
        .maxlen       = sizeof(int),
        .mode         = 0644,
        .proc_handler = lse_proc_sched_ravg_window_update,
        .extra1       = SYSCTL_ZERO,
        .extra2       = SYSCTL_INT_MAX,
    },
	{
		.procname	= "lse_gov_debug",
		.data		= &sysctl_lse_gov_debug,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0666,
		.proc_handler	= proc_dointvec,
	},
    { },
};

void lse_sysctl_init(void)
{
    struct ctl_table_header *hdr;
    hdr = register_sysctl("lunar_sched_ext", lse_table);
    kmemleak_not_leak(hdr);
}
