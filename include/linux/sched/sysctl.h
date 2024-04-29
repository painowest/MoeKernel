/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_SYSCTL_H
#define _LINUX_SCHED_SYSCTL_H

#include <linux/types.h>

struct ctl_table;

#ifdef CONFIG_DETECT_HUNG_TASK

#ifdef CONFIG_SMP
extern unsigned int sysctl_hung_task_all_cpu_backtrace;
#else
#define sysctl_hung_task_all_cpu_backtrace 0
#endif /* CONFIG_SMP */

extern int	     sysctl_hung_task_check_count;
extern unsigned int  sysctl_hung_task_panic;
extern unsigned long sysctl_hung_task_timeout_secs;
extern unsigned long sysctl_hung_task_check_interval_secs;
extern int sysctl_hung_task_warnings;
int proc_dohung_task_timeout_secs(struct ctl_table *table, int write,
		void *buffer, size_t *lenp, loff_t *ppos);
#else
/* Avoid need for ifdefs elsewhere in the code */
enum { sysctl_hung_task_timeout_secs = 0 };
#endif

extern unsigned int sysctl_sched_child_runs_first;
extern unsigned int sysctl_sched_force_lb_enable;
#ifdef CONFIG_SCHED_WALT
extern unsigned int sysctl_sched_capacity_margin_up[MAX_MARGIN_LEVELS];
extern unsigned int sysctl_sched_capacity_margin_down[MAX_MARGIN_LEVELS];
extern unsigned int sysctl_sched_user_hint;
extern const int sched_user_hint_max;
extern unsigned int sysctl_sched_cpu_high_irqload;
extern unsigned int sysctl_sched_boost;
extern unsigned int sysctl_sched_group_upmigrate_pct;
extern unsigned int sysctl_sched_group_downmigrate_pct;
extern unsigned int sysctl_sched_conservative_pl;
extern unsigned int sysctl_sched_many_wakeup_threshold;
extern unsigned int sysctl_sched_walt_rotate_big_tasks;
extern unsigned int sysctl_sched_min_task_util_for_boost;
extern unsigned int sysctl_sched_min_task_util_for_colocation;
extern unsigned int sysctl_sched_asym_cap_sibling_freq_match_pct;
extern unsigned int sysctl_sched_coloc_downmigrate_ns;
extern unsigned int sysctl_sched_task_unfilter_period;
extern unsigned int sysctl_sched_busy_hyst_enable_cpus;
extern unsigned int sysctl_sched_busy_hyst;
extern unsigned int sysctl_sched_coloc_busy_hyst_enable_cpus;
extern unsigned int sysctl_sched_coloc_busy_hyst;
extern unsigned int sysctl_sched_coloc_busy_hyst_max_ms;
extern unsigned int sysctl_sched_window_stats_policy;
extern unsigned int sysctl_sched_ravg_window_nr_ticks;
extern unsigned int sysctl_sched_dynamic_ravg_window_enable;
extern unsigned int sysctl_sched_prefer_spread;

extern int
walt_proc_group_thresholds_handler(struct ctl_table *table, int write,
			 void __user *buffer, size_t *lenp,
			 loff_t *ppos);
extern int
walt_proc_user_hint_handler(struct ctl_table *table, int write,
			 void __user *buffer, size_t *lenp,
			 loff_t *ppos);

extern int
sched_ravg_window_handler(struct ctl_table *table, int write,
			 void __user *buffer, size_t *lenp,
			 loff_t *ppos);

extern int sched_updown_migrate_handler(struct ctl_table *table,
					int write, void __user *buffer,
					size_t *lenp, loff_t *ppos);
#endif

#if defined(CONFIG_PREEMPT_TRACER) || defined(CONFIG_DEBUG_PREEMPT)
extern unsigned int sysctl_preemptoff_tracing_threshold_ns;
#endif
#if defined(CONFIG_PREEMPTIRQ_EVENTS) && defined(CONFIG_IRQSOFF_TRACER) && \
		!defined(CONFIG_PROVE_LOCKING)
extern unsigned int sysctl_irqsoff_tracing_threshold_ns;
#endif

enum sched_tunable_scaling {
	SCHED_TUNABLESCALING_NONE,
	SCHED_TUNABLESCALING_LOG,
	SCHED_TUNABLESCALING_LINEAR,
	SCHED_TUNABLESCALING_END,
};

extern int sched_boost_handler(struct ctl_table *table, int write,
			void __user *buffer, size_t *lenp, loff_t *ppos);

/*
 *  control realtime throttling:
 *
 *  /proc/sys/kernel/sched_rt_period_us
 *  /proc/sys/kernel/sched_rt_runtime_us
 */
extern unsigned int sysctl_sched_rt_period;
extern int sysctl_sched_rt_runtime;

extern unsigned int sysctl_sched_dl_period_max;
extern unsigned int sysctl_sched_dl_period_min;

#ifdef CONFIG_UCLAMP_TASK
extern unsigned int sysctl_sched_uclamp_util_min;
extern unsigned int sysctl_sched_uclamp_util_max;
extern unsigned int sysctl_sched_uclamp_util_min_rt_default;
#endif

#ifdef CONFIG_CFS_BANDWIDTH
extern unsigned int sysctl_sched_cfs_bandwidth_slice;
#endif

#ifdef CONFIG_SCHED_AUTOGROUP
extern unsigned int sysctl_sched_autogroup_enabled;
#endif

extern int sysctl_sched_rr_timeslice;
extern int sched_rr_timeslice;

int sched_rr_handler(struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos);
int sched_rt_handler(struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos);
int sysctl_sched_uclamp_handler(struct ctl_table *table, int write,
		void *buffer, size_t *lenp, loff_t *ppos);
int sysctl_numa_balancing(struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos);
int sysctl_schedstats(struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos);

#ifdef CONFIG_SMP
extern unsigned int sysctl_sched_pelt_multiplier;

int sched_pelt_multiplier(struct ctl_table *table, int write, void *buffer,
		size_t *lenp, loff_t *ppos);
#endif

#if defined(CONFIG_ENERGY_MODEL) && defined(CONFIG_CPU_FREQ_GOV_SCHEDUTIL)
extern unsigned int sysctl_sched_energy_aware;
int sched_energy_aware_handler(struct ctl_table *table, int write,
		void *buffer, size_t *lenp, loff_t *ppos);
#endif

#endif /* _LINUX_SCHED_SYSCTL_H */
