// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 */

#ifndef _LSE_CFS_H_
#define _LSE_CFS_H_

extern void lse_scheduler_tick(void);
extern void lse_tick_entry(void *unused, struct rq *rq);
extern void lse_cfs_hooks_register(void);

#endif /* _LSE_CFS_H_ */
