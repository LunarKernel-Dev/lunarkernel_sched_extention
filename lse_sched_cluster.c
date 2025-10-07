// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 *
 * This code is part of LunarKernel.
 * sched topology.
 *
 * File name: lse_sched_cluster.c
 * Author: Cloud_Yun <1770669041@qq.com>
 * Version: v251003_Dev
 * Date: 2025/10/3 Friday
 */

#include <linux/slab.h>
#include <linux/cpufreq.h>

#include "include/lse_sched_cluster.h"

DEFINE_PER_CPU(struct lse_sched_cluster *, lse_sched_cluster);
DEFINE_PER_CPU(cpumask_t, freq_domain_cpumask);

int __read_mostly lse_num_sched_clusters;
struct list_head lse_cluster_head;

static int cpufreq_update_min(struct notifier_block *nb, unsigned long freq,
			      void *data)
{
	struct lse_sched_cluster *cluster = container_of(nb, struct lse_sched_cluster, nb_min);

	cluster->min_freq = freq;
	return 0;
}

static int cpufreq_update_max(struct notifier_block *nb, unsigned long freq,
			      void *data)
{
	struct lse_sched_cluster *cluster = container_of(nb, struct lse_sched_cluster, nb_max);

	cluster->max_freq = freq;
	return 0;
}

static struct lse_sched_cluster init_cluster = {
	.list = LIST_HEAD_INIT(init_cluster.list),
	.id = 0,
	.max_possible_capacity = SCHED_CAPACITY_SCALE,
	.cur_freq = 1,
	.max_freq = 1,
	.min_freq = 1,
	.capacity_margin = 1280,
	.sd_capacity_margin = 1280,
	.nb_min = {
		.notifier_call = cpufreq_update_min,
	},
	.nb_max = {
		.notifier_call = cpufreq_update_max,
	},
};

static void init_clusters(void)
{
    int cpu;

	init_cluster.cpus = *cpu_possible_mask;
	raw_spin_lock_init(&init_cluster.load_lock);
	INIT_LIST_HEAD(&lse_cluster_head);
	list_add(&init_cluster.list, &lse_cluster_head);

    for_each_possible_cpu(cpu) {
        per_cpu(lse_sched_cluster, cpu) = &init_cluster;
        cpumask_copy(&per_cpu(freq_domain_cpumask, cpu), cpumask_of(cpu));
    }
}

static void
insert_cluster(struct lse_sched_cluster *cluster, struct list_head *head)
{
	struct lse_sched_cluster *tmp;
	struct list_head *iter = head;

	list_for_each_entry(tmp, head, list) {
		if (arch_scale_cpu_capacity(cpumask_first(&cluster->cpus))
			    < arch_scale_cpu_capacity(cpumask_first(&tmp->cpus)))
			break;
		iter = &tmp->list;
	}

	list_add(&cluster->list, iter);
}

static struct lse_sched_cluster *alloc_new_cluster(const struct cpumask *cpus)
{
	struct lse_sched_cluster *cluster = NULL;

	cluster = kzalloc(sizeof(struct lse_sched_cluster), GFP_ATOMIC);
	BUG_ON(!cluster);

	INIT_LIST_HEAD(&cluster->list);
	cluster->max_possible_capacity = arch_scale_cpu_capacity(cpumask_first(cpus));
	cluster->nb_min.notifier_call = cpufreq_update_min;
	cluster->nb_max.notifier_call = cpufreq_update_max;
	cluster->cur_freq = 1;
	cluster->max_freq = 1;
	cluster->min_freq = 1;
	cluster->freq_init_done = false;
	cluster->capacity_margin = 1280;
	cluster->sd_capacity_margin = 1280;
	cluster->cpus = *cpus;
	raw_spin_lock_init(&cluster->load_lock);

	return cluster;
}

static void add_cluster(const struct cpumask *cpus, struct list_head *head)
{
	struct lse_sched_cluster *cluster = alloc_new_cluster(cpus);
	int i;

    BUG_ON(lse_num_sched_clusters >= MAX_LSE_CLUSTERS);

	for_each_cpu(i, cpus) {
		per_cpu(lse_sched_cluster, i) = cluster;
	}

	insert_cluster(cluster, head);
	lse_num_sched_clusters++;
}

static void cleanup_clusters(struct list_head *head)
{
	struct lse_sched_cluster *cluster, *tmp;
	int i;

	list_for_each_entry_safe(cluster, tmp, head, list) {
		for_each_cpu(i, &cluster->cpus) {
			per_cpu(lse_sched_cluster, i) = &init_cluster;
		}
		list_del(&cluster->list);
		lse_num_sched_clusters--;
		kfree(cluster);
	}
}

static inline void align_clusters(struct list_head *head)
{
	struct lse_sched_cluster *tmp;
	struct list_head *cluster1 = head, *cluster2 = head;
	unsigned long capacity1 = 0, capacity2 = 0;
	int i = 0;

	if (lse_num_sched_clusters != 4)
		return;

	list_for_each_entry(tmp, head, list) {
		if (i == 1) {
			cluster1 = &tmp->list;
			capacity1 = arch_scale_cpu_capacity(cpumask_first(&tmp->cpus));
		}
		if (i == 2) {
			cluster2 = &tmp->list;
			capacity2 = arch_scale_cpu_capacity(cpumask_first(&tmp->cpus));
		}
		i++;
	}

	if (capacity1 < capacity2)
		list_swap(cluster1, cluster2);
}

static inline void assign_cluster_ids(struct list_head *head)
{
	struct lse_sched_cluster *cluster;
	unsigned int cpu;

	list_for_each_entry(cluster, head, list) {
		cpu = cpumask_first(&cluster->cpus);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
		cluster->id = topology_cluster_id(cpu);
#else
		cluster->id = topology_physical_package_id(cpu);
#endif
	}
}

static void
move_list(struct list_head *dst, struct list_head *src, bool sync_rcu)
{
	struct list_head *first, *last;

	first = src->next;
	last = src->prev;

	if (sync_rcu) {
		INIT_LIST_HEAD_RCU(src);
		synchronize_rcu();
	}

	first->prev = dst;
	dst->prev = last;
	last->next = dst;

	/* Ensure list sanity before making the head visible to all CPUs. */
	smp_mb();
	dst->next = first;
}

static void get_possible_siblings(int cpuid, struct cpumask *cluster_cpus)
{
	int cpu;
	struct cpu_topology *cpu_topo, *cpuid_topo = &cpu_topology[cpuid];

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	if (cpuid_topo->cluster_id == -1)
#else
	if (cpuid_topo->package_id == -1)
#endif
		return;

	for_each_possible_cpu(cpu) {
		cpu_topo = &cpu_topology[cpu];

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
		if (cpuid_topo->cluster_id != cpu_topo->cluster_id)
#else
		if (cpuid_topo->package_id != cpu_topo->package_id)
#endif
			continue;
		cpumask_set_cpu(cpu, cluster_cpus);
	}
}

static void update_cluster_topology(void)
{
	struct cpumask cpus = *cpu_possible_mask;
	struct cpumask cluster_cpus;
	struct list_head new_head;
	int i;

	INIT_LIST_HEAD(&new_head);

	for_each_cpu(i, &cpus) {
		cpumask_clear(&cluster_cpus);
		get_possible_siblings(i, &cluster_cpus);
		if (cpumask_empty(&cluster_cpus)) {
			cleanup_clusters(&new_head);
			return;
		}
		cpumask_andnot(&cpus, &cpus, &cluster_cpus);
		add_cluster(&cluster_cpus, &new_head);
	}

	align_clusters(&new_head);
	assign_cluster_ids(&new_head);

	/*
	 * Ensure cluster ids are visible to all CPUs before making
	 * lse_cluster_head visible.
	 */
	move_list(&lse_cluster_head, &new_head, false);
}

static int cpufreq_notifier_policy(struct notifier_block *nb,
				   unsigned long val, void *data)
{
	struct cpufreq_policy *policy = (struct cpufreq_policy *)data;
	struct cpumask policy_cluster = *policy->related_cpus;
	struct lse_sched_cluster *cluster = NULL;
	int ret, i, j;

	switch (val) {
	case CPUFREQ_CREATE_POLICY:
		for_each_cpu(i, &policy_cluster) {
			cluster = per_cpu(lse_sched_cluster, i);
			cpumask_andnot(&policy_cluster, &policy_cluster,
				       &cluster->cpus);

			if (!cluster->freq_init_done) {
				for_each_cpu(j, &cluster->cpus)
					cpumask_copy(&per_cpu(freq_domain_cpumask, j),
						     policy->related_cpus);

				cluster->min_freq = policy->min;
				cluster->max_freq = policy->max;
				cluster->freq_init_done = true;
			}
		}

		ret = freq_qos_add_notifier(&policy->constraints, FREQ_QOS_MIN,
					    &cluster->nb_min);
		if (ret)
			pr_err("Failed to register MIN QoS notifier: %d (%*pbl)\n",
			       ret, cpumask_pr_args(policy->cpus));

		ret = freq_qos_add_notifier(&policy->constraints, FREQ_QOS_MAX,
					    &cluster->nb_max);
		if (ret)
			pr_err("Failed to register MAX QoS notifier: %d (%*pbl)\n",
			       ret, cpumask_pr_args(policy->cpus));

		break;
	case CPUFREQ_REMOVE_POLICY:
		freq_qos_remove_notifier(&policy->constraints, FREQ_QOS_MAX,
					 &cluster->nb_max);
		freq_qos_remove_notifier(&policy->constraints, FREQ_QOS_MIN,
					 &cluster->nb_min);
		ret = 0;
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int cpufreq_notifier_trans(struct notifier_block *nb,
				  unsigned long val, void *data)
{
	struct cpufreq_freqs *freq = (struct cpufreq_freqs *)data;
	unsigned int cpu = freq->policy->cpu;
	unsigned int new_freq = freq->new;
	struct lse_sched_cluster *cluster = NULL;
	struct cpumask policy_cpus = per_cpu(freq_domain_cpumask, cpu);
	int i;

	if (val != CPUFREQ_POSTCHANGE)
		return 0;

	BUG_ON(new_freq == 0);

	for_each_cpu(i, &policy_cpus) {
		cluster = per_cpu(lse_sched_cluster, i);
		cluster->cur_freq = new_freq;
		cpumask_andnot(&policy_cpus, &policy_cpus, &cluster->cpus);
	}

	return 0;
}

static struct notifier_block notifier_policy_block = {
	.notifier_call = cpufreq_notifier_policy
};

static struct notifier_block notifier_trans_block = {
	.notifier_call = cpufreq_notifier_trans
};

static int register_sched_callback(void)
{
	int ret;

	ret = cpufreq_register_notifier(&notifier_policy_block,
					CPUFREQ_POLICY_NOTIFIER);
	if (!ret)
		ret = cpufreq_register_notifier(&notifier_trans_block,
						CPUFREQ_TRANSITION_NOTIFIER);

	return ret;
}

void lse_sched_cluster_init(void)
{
    init_clusters();
    update_cluster_topology();
    register_sched_callback();
}
