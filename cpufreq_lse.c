// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, LunarKernel Project. All rights reserved.
 *
 * This code is part of LunarKernel.
 * cpufreq governor.
 *
 * File name: cpufreq_lse.c
 * Author: Cloud_Yun <1770669041@qq.com>
 * Version: v251003_Dev
 * Date: 2025/10/3 Friday
 */

#include <linux/kmemleak.h>
#include <linux/cpufreq.h>
#include <linux/sched.h>
#include <linux/sched/cputime.h>
#include <uapi/linux/sched/types.h>
#include <trace/hooks/sched.h>

#include "include/lse_main.h"

unsigned int sysctl_lse_gov_debug;
static int cpufreq_gov_debug(void) {return sysctl_lse_gov_debug;}

/*debug level for lse_gov*/
#define DEBUG_SYSTRACE (1 << 0)
#define DEBUG_FTRACE   (1 << 1)
#define DEBUG_KMSG     (1 << 2)

#define lse_gov_debug(fmt, ...) \
	pr_info("[lse_gov][%s] "fmt, __func__, ##__VA_ARGS__)

#define lse_gov_err(fmt, ...) \
	pr_err("[lse_gov][%s] "fmt, __func__, ##__VA_ARGS__)

#define gov_trace_printk(fmt, args...)	\
do {										\
		trace_printk("[lse_gov] "fmt, args);	\
} while (0)

#define DEFAULT_TARGET_LOAD 90

static int gov_flag[MAX_LSE_CLUSTERS] = {0};
struct proc_dir_entry *lse_dir;
#define MAX_CLS_NUM 5

static struct irq_work lse_cpufreq_irq_work;

struct lse_gov_tunables {
	struct gov_attr_set		attr_set;
	unsigned int			target_loads;
	int				soft_freq_max;
	int				soft_freq_min;
	bool				apply_freq_immediately;
};

struct lse_gov_policy {
	struct cpufreq_policy	*policy;

	struct lse_gov_tunables	*tunables;
	struct list_head	tunables_hook;

	raw_spinlock_t		update_lock;	/* For shared policies */
	unsigned int		next_freq;
	unsigned int		freq_cached;
	/* The next fields are only needed if fast switch cannot be used: */
	struct kthread_work	work;
	struct mutex		work_lock;
	struct kthread_worker	worker;
	struct task_struct	*thread;
	bool			work_in_progress;
	unsigned int	target_load;
};

struct lse_gov_cpu {
	unsigned int		reasons;
	struct lse_gov_policy	*lg_policy;
	unsigned int		cpu;

	unsigned long		util;
	unsigned int		flags;
};

static DEFINE_PER_CPU(struct lse_gov_cpu, lse_gov_cpu);
static DEFINE_PER_CPU(struct lse_gov_tunables *, cached_tunables);
static DEFINE_MUTEX(global_tunables_lock);
static struct lse_gov_tunables *global_tunables;

static void lse_gov_work(struct kthread_work *work)
{
	struct lse_gov_policy *lg_policy = container_of(work, struct lse_gov_policy, work);
	unsigned int freq;
	unsigned long flags;

	/*
	 * Hold lg_policy->update_lock shortly to handle the case where:
	 * incase lg_policy->next_freq is read here, and then updated by
	 * lse_gov_deferred_update() just before work_in_progress is set to false
	 * here, we may miss queueing the new update.
	 *
	 * Note: If a work was queued after the update_lock is released,
	 * lse_gov_work() will just be called again by kthread_work code; and the
	 * request will be proceed before the lse_gov thread sleeps.
	 */
	raw_spin_lock_irqsave(&lg_policy->update_lock, flags);
	freq = lg_policy->next_freq;
	raw_spin_unlock_irqrestore(&lg_policy->update_lock, flags);

	mutex_lock(&lg_policy->work_lock);
	__cpufreq_driver_target(lg_policy->policy, freq, CPUFREQ_RELATION_L);
	mutex_unlock(&lg_policy->work_lock);
}

static inline void lse_irq_work_queue(struct irq_work *work)
{
	if (likely(cpu_online(raw_smp_processor_id())))
		irq_work_queue(work);
	else
		irq_work_queue_on(work, cpumask_any(cpu_online_mask));
}

void run_lse_irq_work_rollover(void)
{
	lse_irq_work_queue(&lse_cpufreq_irq_work);
}

/* next_freq = (max_freq * scale_time* 100)/(window_size * TL * arch_scale_cpu_capacity) */
static unsigned int get_next_freq(struct lse_gov_policy *lg_policy, u64 prev_runnable_sum)
{
	struct cpufreq_policy *policy = lg_policy->policy;
	unsigned int freq = policy->cpuinfo.max_freq, next_f;
	unsigned int window_size_tl, cluster_tl;
	u64 divisor;
	int cpu = cpumask_first(policy->cpus);
	cluster_tl = DEFAULT_TARGET_LOAD;
	if (lg_policy->tunables) {
		cluster_tl = lg_policy->tunables->target_loads;
	}

	window_size_tl = mult_frac(lse_sched_ravg_window, cluster_tl, 100);
	divisor = DIV64_U64_ROUNDUP(window_size_tl * arch_scale_cpu_capacity(cpu), freq);
	next_f = DIV64_U64_ROUNDUP(prev_runnable_sum << SCHED_CAPACITY_SHIFT, divisor);

	if (cpufreq_gov_debug() & DEBUG_FTRACE)
		gov_trace_printk("cluster[%d] max_freq[%d] win_tl[%d] cpu_cap[%lu] divisor[%llu] next_f[%d]\n",
			cpu, freq, window_size_tl, arch_scale_cpu_capacity(cpu), divisor, next_f);
	return next_f;
}

static unsigned int soft_freq_clamp(struct lse_gov_policy *lg_policy, unsigned int target_freq)
{
	struct cpufreq_policy *policy = lg_policy->policy;
	int soft_freq_max = lg_policy->tunables->soft_freq_max;
	int soft_freq_min = lg_policy->tunables->soft_freq_min;

	if (soft_freq_min >= 0 && soft_freq_min > target_freq) {
		target_freq = soft_freq_min;
	}
	if (soft_freq_max >= 0 && soft_freq_max < target_freq) {
		target_freq = soft_freq_max;
	}

	if (cpufreq_gov_debug() & DEBUG_FTRACE)
		gov_trace_printk("cluster[%d] max_freq[%d] min_freq[%d] freq[%d]\n",
			policy->cpu, soft_freq_max, soft_freq_min, target_freq);

	return target_freq;
}

void lse_gov_update_cpufreq(struct cpufreq_policy *policy, u64 prev_runnable_sum)
{
	unsigned int next_f;
	struct lse_gov_policy *lg_policy = policy->governor_data;
	unsigned long irq_flags;

	raw_spin_lock_irqsave(&lg_policy->update_lock, irq_flags);

	next_f = get_next_freq(lg_policy, prev_runnable_sum);
	next_f = soft_freq_clamp(lg_policy, next_f);
	next_f = cpufreq_driver_resolve_freq(policy, next_f);
	lg_policy->freq_cached = lg_policy->next_freq ? lg_policy->next_freq : next_f;
	if (lg_policy->next_freq == next_f)
		goto unlock;
	lg_policy->next_freq = next_f;
	if (cpufreq_gov_debug() & DEBUG_FTRACE)
		gov_trace_printk("cluster[%d] freq[%d] fast[%d]\n", policy->cpu, next_f, policy->fast_switch_enabled);
	if (policy->fast_switch_enabled)
		cpufreq_driver_fast_switch(policy, next_f);
	else
		kthread_queue_work(&lg_policy->worker, &lg_policy->work);

unlock:
	raw_spin_unlock_irqrestore(&lg_policy->update_lock, irq_flags);
}

void lse_gov_update_soft_limit_cpufreq(struct lse_gov_policy *lg_policy)
{
	unsigned int next_f;
	struct cpufreq_policy *policy = lg_policy->policy;
	unsigned long irq_flags;

	raw_spin_lock_irqsave(&lg_policy->update_lock, irq_flags);

	next_f = soft_freq_clamp(lg_policy, lg_policy->next_freq);
	next_f = cpufreq_driver_resolve_freq(policy, next_f);
	if (lg_policy->next_freq == next_f)
		goto unlock;
	lg_policy->next_freq = next_f;
	if (cpufreq_gov_debug() & DEBUG_FTRACE)
		gov_trace_printk("cluster[%d] freq[%d] fast[%d]\n",
			policy->cpu, next_f, policy->fast_switch_enabled);
	if (policy->fast_switch_enabled)
		cpufreq_driver_fast_switch(policy, next_f);
	else
		kthread_queue_work(&lg_policy->worker, &lg_policy->work);

unlock:
	raw_spin_unlock_irqrestore(&lg_policy->update_lock, irq_flags);
}

/************************** sysfs interface ************************/
static inline struct lse_gov_tunables *to_lse_gov_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct lse_gov_tunables, attr_set);
}

static DEFINE_MUTEX(min_rate_lock);


static ssize_t target_loads_show(struct gov_attr_set *attr_set, char *buf)
{
	struct lse_gov_tunables *tunables = to_lse_gov_tunables(attr_set);
	return sprintf(buf, "%d\n", tunables->target_loads);
}

static ssize_t target_loads_store(struct gov_attr_set *attr_set, const char *buf,
					size_t count)
{
	struct lse_gov_tunables *tunables = to_lse_gov_tunables(attr_set);
	unsigned int new_target_loads = DEFAULT_TARGET_LOAD;

	if (kstrtouint(buf, 10, &new_target_loads))
		return -EINVAL;

	tunables->target_loads = new_target_loads;
	return count;
}

static ssize_t soft_freq_max_show(struct gov_attr_set *attr_set, char *buf)
{
	struct lse_gov_tunables *tunables = to_lse_gov_tunables(attr_set);
	int soft_freq_max = tunables->soft_freq_max;

	if (soft_freq_max < 0) {
		return sprintf(buf, "max\n");
	} else {
		return sprintf(buf, "%d\n", soft_freq_max);
	}
}

static ssize_t soft_freq_max_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	struct lse_gov_tunables *tunables = to_lse_gov_tunables(attr_set);
	struct lse_gov_policy *lg_policy = list_first_entry(&attr_set->policy_list, struct lse_gov_policy, tunables_hook);
	int new_soft_freq_max = -1;

	if (kstrtoint(buf, 10, &new_soft_freq_max))
		return -EINVAL;

	if (tunables->soft_freq_max == new_soft_freq_max) {
		return count;
	}

	tunables->soft_freq_max = new_soft_freq_max;
	if (tunables->apply_freq_immediately) {
		lse_gov_update_soft_limit_cpufreq(lg_policy);
	}

	return count;
}

static ssize_t soft_freq_min_show(struct gov_attr_set *attr_set, char *buf)
{
	struct lse_gov_tunables *tunables = to_lse_gov_tunables(attr_set);
	int soft_freq_min = tunables->soft_freq_min;

	if (soft_freq_min < 0) {
		return sprintf(buf, "0\n");
	} else {
		return sprintf(buf, "%d\n", soft_freq_min);
	}
}

static ssize_t soft_freq_min_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	struct lse_gov_tunables *tunables = to_lse_gov_tunables(attr_set);
	struct lse_gov_policy *lg_policy = list_first_entry(&attr_set->policy_list, struct lse_gov_policy, tunables_hook);
	int new_soft_freq_min = -1;

	if (kstrtoint(buf, 10, &new_soft_freq_min))
		return -EINVAL;

	if (tunables->soft_freq_min == new_soft_freq_min) {
		return count;
	}

	tunables->soft_freq_min = new_soft_freq_min;
	if (tunables->apply_freq_immediately) {
		lse_gov_update_soft_limit_cpufreq(lg_policy);
	}

	return count;
}

static ssize_t soft_freq_cur_show(struct gov_attr_set *attr_set __maybe_unused, char *buf)
{
	return sprintf(buf, "none\n");
}

static ssize_t soft_freq_cur_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	struct lse_gov_tunables *tunables = to_lse_gov_tunables(attr_set);
	struct lse_gov_policy *lg_policy = list_first_entry(&attr_set->policy_list, struct lse_gov_policy, tunables_hook);
	int new_soft_freq_cur = -1;

	if (kstrtoint(buf, 10, &new_soft_freq_cur))
		return -EINVAL;

	if (tunables->soft_freq_max == new_soft_freq_cur && tunables->soft_freq_min == new_soft_freq_cur) {
		return count;
	}

	tunables->soft_freq_max = new_soft_freq_cur;
	tunables->soft_freq_min = new_soft_freq_cur;
	if (tunables->apply_freq_immediately) {
		lse_gov_update_soft_limit_cpufreq(lg_policy);
	}

	return count;
}

static ssize_t apply_freq_immediately_show(struct gov_attr_set *attr_set, char *buf)
{
	struct lse_gov_tunables *tunables = to_lse_gov_tunables(attr_set);
	return sprintf(buf, "%d\n", (int)tunables->apply_freq_immediately);
}

static ssize_t apply_freq_immediately_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	struct lse_gov_tunables *tunables = to_lse_gov_tunables(attr_set);
	int new_apply_freq_immediately = 0;

	if (kstrtoint(buf, 10, &new_apply_freq_immediately))
		return -EINVAL;

	tunables->apply_freq_immediately = new_apply_freq_immediately > 0;
	return count;
}

static struct governor_attr target_loads =
	__ATTR(target_loads, 0664, target_loads_show, target_loads_store);

static struct governor_attr soft_freq_max =
	__ATTR(soft_freq_max, 0664, soft_freq_max_show, soft_freq_max_store);

static struct governor_attr soft_freq_min =
	__ATTR(soft_freq_min, 0664, soft_freq_min_show, soft_freq_min_store);

static struct governor_attr soft_freq_cur =
	__ATTR(soft_freq_cur, 0664, soft_freq_cur_show, soft_freq_cur_store);

static struct governor_attr apply_freq_immediately =
	__ATTR(apply_freq_immediately, 0664, apply_freq_immediately_show, apply_freq_immediately_store);

static struct attribute *lse_gov_attrs[] = {
	&target_loads.attr,
	&soft_freq_max.attr,
	&soft_freq_min.attr,
	&soft_freq_cur.attr,
	&apply_freq_immediately.attr,
	NULL
};
ATTRIBUTE_GROUPS(lse_gov);

static struct kobj_type lse_gov_tunables_ktype = {
	.default_groups = lse_gov_groups,
	.sysfs_ops = &governor_sysfs_ops,
};

/********************** cpufreq governor interface *********************/

struct cpufreq_governor cpufreq_lse_gov;

static struct lse_gov_policy *lse_gov_policy_alloc(struct cpufreq_policy *policy)
{
	struct lse_gov_policy *lg_policy;

	lg_policy = kzalloc(sizeof(*lg_policy), GFP_KERNEL);
	if (!lg_policy)
		return NULL;

	lg_policy->policy = policy;
	raw_spin_lock_init(&lg_policy->update_lock);
	return lg_policy;
}

static inline void lse_gov_cpu_reset(struct lse_gov_policy *lg_policy)
{
	unsigned int cpu;

	for_each_cpu(cpu, lg_policy->policy->cpus) {
		struct lse_gov_cpu *lg_cpu = &per_cpu(lse_gov_cpu, cpu);

		lg_cpu->lg_policy = NULL;
	}
}

static void lse_gov_policy_free(struct lse_gov_policy *lg_policy)
{
	kfree(lg_policy);
}

static void lse_irq_work(struct irq_work *irq_work)
{
	cpumask_t lock_cpus;
	struct lse_sched_cluster *cluster;
	struct cpufreq_policy *policy;
	struct lse_sched_rq_stats *lrq;
	struct rq *rq;
	int cpu;
	int level = 0;
	u64 wc;
	unsigned long flags;
	struct lse_entity *lse;

	cpumask_copy(&lock_cpus, cpu_possible_mask);

	for_each_cpu(cpu, &lock_cpus) {
		if (level == 0)
			raw_spin_lock(&cpu_rq(cpu)->__lock);
		else
			raw_spin_lock_nested(&cpu_rq(cpu)->__lock, level);
		level++;
	}

	wc = lse_sched_clock();

	for_each_lse_cluster(cluster) {
		cpumask_t cluster_online_cpus;
		u64 prev_runnable_sum = 0;

		if (gov_flag[cluster->id] == 0)
			continue;
		cpumask_and(&cluster_online_cpus, &cluster->cpus, cpu_online_mask);
		for_each_cpu(cpu, &cluster_online_cpus) {
			rq = cpu_rq(cpu);
			lse = get_lunar_ext_entity(rq->curr);
			if (lse)
				lse_update_task_ravg(lse, rq->curr, rq, TASK_UPDATE, wc);
			lrq = &per_cpu(lse_sched_rq_stats, cpu);
			if (cpufreq_gov_debug() & DEBUG_FTRACE)
				gov_trace_printk("cpu[%d] prev_runnable_sum[%llu]\n", cpu, lrq->prev_runnable_sum);
			prev_runnable_sum = max(prev_runnable_sum, lrq->prev_runnable_sum);
		}

		policy = cpufreq_cpu_get_raw(cpumask_first(&cluster_online_cpus));
		if (policy == NULL)
			lse_gov_err("NULL policy [%d]\n", cpumask_first(&cluster_online_cpus));
		lse_gov_update_cpufreq(policy, prev_runnable_sum);
	}

	spin_lock_irqsave(&new_sched_ravg_window_lock, flags);
	if (unlikely(new_lse_sched_ravg_window != lse_sched_ravg_window)) {
		lrq = &per_cpu(lse_sched_rq_stats, smp_processor_id());
		if (wc < lrq->window_start + new_lse_sched_ravg_window) {
			lse_sched_ravg_window = new_lse_sched_ravg_window;
			lse_fixup_window_dep();
		}
	}
	spin_unlock_irqrestore(&new_sched_ravg_window_lock, flags);

	for_each_cpu(cpu, &lock_cpus) {
		raw_spin_unlock(&cpu_rq(cpu)->__lock);
	}
}

static int lse_gov_kthread_create(struct lse_gov_policy *lg_policy)
{
	struct task_struct *thread;
	struct sched_attr attr = {
		.size		= sizeof(struct sched_attr),
		.sched_policy	= SCHED_DEADLINE,
		.sched_flags	= SCHED_FLAG_SUGOV,
		.sched_nice	= 0,
		.sched_priority	= 0,
		/*
		 * Fake (unused) bandwidth; workaround to "fix"
		 * priority inheritance.
		 */
		.sched_runtime	=  1000000,
		.sched_deadline = 10000000,
		.sched_period	= 10000000,
	};
	struct cpufreq_policy *policy = lg_policy->policy;
	int ret;

	/* kthread only required for slow path */
	if (policy->fast_switch_enabled)
		return 0;

	kthread_init_work(&lg_policy->work, lse_gov_work);
	kthread_init_worker(&lg_policy->worker);
	thread = kthread_create(kthread_worker_fn, &lg_policy->worker,
				"lse_gov:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("failed to create lse_gov thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setattr_nocheck(thread, &attr);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_DEADLINE\n", __func__);
		return ret;
	}

	lg_policy->thread = thread;
	kthread_bind_mask(thread, policy->related_cpus);
	mutex_init(&lg_policy->work_lock);

	wake_up_process(thread);

	return 0;
}

static void lse_gov_kthread_stop(struct lse_gov_policy *lg_policy)
{
	/* kthread only required for slow path */
	if (lg_policy->policy->fast_switch_enabled)
		return;

	kthread_flush_worker(&lg_policy->worker);
	kthread_stop(lg_policy->thread);
	mutex_destroy(&lg_policy->work_lock);
}

static struct lse_gov_tunables *lse_gov_tunables_alloc(struct lse_gov_policy *lg_policy)
{
	struct lse_gov_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &lg_policy->tunables_hook);
		if (!have_governor_per_policy())
			global_tunables = tunables;
	}
	return tunables;
}

static void lse_gov_tunables_free(struct lse_gov_tunables *tunables)
{
	if (!have_governor_per_policy())
		global_tunables = NULL;

	kfree(tunables);
}

#define DEFAULT_HISPEED_LOAD 90
static void lse_gov_tunables_save(struct cpufreq_policy *policy,
		struct lse_gov_tunables *tunables)
{
	int cpu;
	struct lse_gov_tunables *cached = per_cpu(cached_tunables, policy->cpu);

	if (!cached) {
		cached = kzalloc(sizeof(*tunables), GFP_KERNEL);
		if (!cached)
			return;

		for_each_cpu(cpu, policy->related_cpus)
			per_cpu(cached_tunables, cpu) = cached;
	}
}

static int lse_gov_init(struct cpufreq_policy *policy)
{
	struct lse_gov_policy *lg_policy;
	struct lse_gov_tunables *tunables;
	int ret = 0;

	/* State should be equivalent to EXIT */
	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	lg_policy = lse_gov_policy_alloc(policy);
	if (!lg_policy) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = lse_gov_kthread_create(lg_policy);
	if (ret)
		goto free_lg_policy;

	mutex_lock(&global_tunables_lock);

	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto stop_kthread;
		}
		policy->governor_data = lg_policy;
		lg_policy->tunables = global_tunables;

		gov_attr_set_get(&global_tunables->attr_set, &lg_policy->tunables_hook);
		goto out;
	}

	tunables = lse_gov_tunables_alloc(lg_policy);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}

	tunables->target_loads = DEFAULT_TARGET_LOAD;
	tunables->soft_freq_max = -1;
	tunables->soft_freq_min = -1;
	tunables->apply_freq_immediately = true;

	policy->governor_data = lg_policy;
	lg_policy->tunables = tunables;

	ret = kobject_init_and_add(&tunables->attr_set.kobj, &lse_gov_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   cpufreq_lse_gov.name);
	if (ret)
		goto fail;

	policy->dvfs_possible_from_any_cpu = 1;

out:
	mutex_unlock(&global_tunables_lock);
	return 0;

fail:
	kobject_put(&tunables->attr_set.kobj);
	policy->governor_data = NULL;
	lse_gov_tunables_free(tunables);

stop_kthread:
	lse_gov_kthread_stop(lg_policy);
	mutex_unlock(&global_tunables_lock);

free_lg_policy:
	lse_gov_policy_free(lg_policy);

disable_fast_switch:
	cpufreq_disable_fast_switch(policy);

	pr_err("initialization failed (error %d)\n", ret);
	return ret;
}

static void lse_gov_exit(struct cpufreq_policy *policy)
{
	struct lse_gov_policy *lg_policy = policy->governor_data;
	struct lse_gov_tunables *tunables = lg_policy->tunables;
	unsigned int count;

	mutex_lock(&global_tunables_lock);

	count = gov_attr_set_put(&tunables->attr_set, &lg_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count) {
		lse_gov_tunables_save(policy, tunables);
		lse_gov_tunables_free(tunables);
	}

	mutex_unlock(&global_tunables_lock);

	lse_gov_kthread_stop(lg_policy);
	lse_gov_cpu_reset(lg_policy);
	lse_gov_policy_free(lg_policy);
	cpufreq_disable_fast_switch(policy);
}

static int lse_gov_start(struct cpufreq_policy *policy)
{
	struct lse_gov_policy *lg_policy = policy->governor_data;
	unsigned int cpu, cluster_id;

	lg_policy->next_freq = 0;

	for_each_cpu(cpu, policy->cpus) {
		struct lse_gov_cpu *lg_cpu = &per_cpu(lse_gov_cpu, cpu);

		memset(lg_cpu, 0, sizeof(*lg_cpu));
		lg_cpu->cpu			= cpu;
		lg_cpu->lg_policy		= lg_policy;
	}
	cpu = cpumask_first(policy->related_cpus);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	cluster_id = topology_cluster_id(cpu);
#else
    cluster_id = topology_physical_package_id(cpu);
#endif
	lse_gov_debug("start cluster[%d] cluster_id[%d] gov\n", cpu, cluster_id);
	if (cluster_id < MAX_LSE_CLUSTERS)
		gov_flag[cluster_id] = 1;

	return 0;
}

static void lse_gov_stop(struct cpufreq_policy *policy)
{
	struct lse_gov_policy *lg_policy = policy->governor_data;
	unsigned int cpu, cluster_id;
	if (!policy->fast_switch_enabled) {
		irq_work_sync(&lse_cpufreq_irq_work);
		kthread_cancel_work_sync(&lg_policy->work);
	}

	cpu = cpumask_first(policy->related_cpus);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	cluster_id = topology_cluster_id(cpu);
#else
    cluster_id = topology_physical_package_id(cpu);
#endif
	if (cluster_id < MAX_LSE_CLUSTERS)
		gov_flag[cluster_id] = 0;
	synchronize_rcu();
}

static void lse_gov_limits(struct cpufreq_policy *policy)
{
	struct lse_gov_policy *lg_policy = policy->governor_data;
	unsigned long flags, now;
	unsigned int freq, final_freq;

	if (!policy->fast_switch_enabled) {
		mutex_lock(&lg_policy->work_lock);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&lg_policy->work_lock);
	} else {
		raw_spin_lock_irqsave(&lg_policy->update_lock, flags);

		freq = lg_policy->next_freq;
		/*
		 * we have serval resources to update freq
		 * (1) scheduler to run callback
		 * (2) cpufreq_set_policy to call governor->limtis here
		 * so we have serveral times here and we must to keep them same
		 * here we using walt_sched_clock() to keep same with walt scheduler
		 */
		now = ktime_get_ns();

		/*
		 * cpufreq_driver_resolve_freq() has a clamp, so we do not need
		 * to do any sort of additional validation here.
		 */
		final_freq = cpufreq_driver_resolve_freq(policy, freq);
		cpufreq_driver_fast_switch(policy, final_freq);

		raw_spin_unlock_irqrestore(&lg_policy->update_lock, flags);
	}
}

struct cpufreq_governor cpufreq_lse_gov = {
	.name			= "lse",
	.owner			= THIS_MODULE,
	.flags			= CPUFREQ_GOV_DYNAMIC_SWITCHING,
	.init			= lse_gov_init,
	.exit			= lse_gov_exit,
	.start			= lse_gov_start,
	.stop			= lse_gov_stop,
	.limits			= lse_gov_limits,
};

int lse_cpufreq_init(void)
{
	int ret = 0;
	struct lse_sched_cluster *cluster = NULL;

	ret = cpufreq_register_governor(&cpufreq_lse_gov);
	if (ret)
		return ret;

	for_each_lse_cluster(cluster)
		lse_gov_debug("num_cluster=%d id=%d cpumask=%*pbl capacity=%lu num_cpus=%d\n",
			lse_num_sched_clusters, cluster->id, cpumask_pr_args(&cluster->cpus),
			arch_scale_cpu_capacity(cpumask_first(&cluster->cpus)),
			num_possible_cpus());

	init_irq_work(&lse_cpufreq_irq_work, lse_irq_work);
	return ret;
}
