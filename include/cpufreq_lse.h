// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 */

#ifndef _LSE_CPUFREQ_H_
#define _LSE_CPUFREQ_H_

extern unsigned int sysctl_lse_gov_debug;
extern void run_lse_irq_work_rollover(void);
extern int lse_cpufreq_init(void);

#endif /* _LSE_CPUFREQ_H_ */
