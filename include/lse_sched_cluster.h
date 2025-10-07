// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 */

#ifndef _LSE_SCHED_CLUSTER_H_
#define _LSE_SCHED_CLUSTER_H_

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/list_sort.h>
#include <linux/notifier.h>

#define MAX_LSE_CLUSTERS 4

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
DECLARE_PER_CPU(struct lse_sched_cluster *, lse_sched_cluster);

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

#endif /* _LSE_SCHED_CLUSTER_H_ */
