#include <linux/cgroup.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/printk.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/ehmp.h>

#include <trace/events/sched.h>
#include <linux/list.h>

#include "sched.h"
#include "tune.h"

#ifdef CONFIG_CGROUP_SCHEDTUNE
bool schedtune_initialized = false;
#endif

unsigned int sysctl_sched_cfs_boost __read_mostly;

/* We hold schedtune boost in effect for at least this long */
#define SCHEDTUNE_BOOST_HOLD_NS 50000000ULL

extern struct reciprocal_value schedtune_spc_rdiv;
extern struct target_nrg schedtune_target_nrg;

static int perf_threshold = 0;

int schedtune_perf_threshold(void)
{
	return perf_threshold + 1;
}

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
#define DYNAMIC_BOOST_SLOTS_COUNT 5
static DEFINE_MUTEX(boost_slot_mutex);
static DEFINE_MUTEX(stune_boost_mutex);
static struct schedtune *getSchedtune(char *st_name);
static int dynamic_boost(struct schedtune *st, int boost);
struct boost_slot {
	struct list_head list;
	int idx;
};
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

/* Performance Boost region (B) threshold params */
static int perf_boost_idx;

/* Performance Constraint region (C) threshold params */
static int perf_constrain_idx;

/**
 * Performance-Energy (P-E) Space thresholds constants
 */
struct threshold_params {
	int nrg_gain;
	int cap_gain;
};

/*
 * System specific P-E space thresholds constants
 */
static struct threshold_params
threshold_gains[] = {
	{ 0, 5 }, /*   < 10% */
	{ 1, 5 }, /*   < 20% */
	{ 2, 5 }, /*   < 30% */
	{ 3, 5 }, /*   < 40% */
	{ 4, 5 }, /*   < 50% */
	{ 5, 4 }, /*   < 60% */
	{ 5, 3 }, /*   < 70% */
	{ 5, 2 }, /*   < 80% */
	{ 5, 1 }, /*   < 90% */
	{ 5, 0 }  /* <= 100% */
};

static int
__schedtune_accept_deltas(int nrg_delta, int cap_delta,
			  int perf_boost_idx, int perf_constrain_idx)
{
	int payoff = -INT_MAX;
	int gain_idx = -1;

	/* Performance Boost (B) region */
	if (nrg_delta >= 0 && cap_delta > 0)
		gain_idx = perf_boost_idx;
	/* Performance Constraint (C) region */
	else if (nrg_delta < 0 && cap_delta <= 0)
		gain_idx = perf_constrain_idx;

	/* Default: reject schedule candidate */
	if (gain_idx == -1)
		return payoff;

	/*
	 * Evaluate "Performance Boost" vs "Energy Increase"
	 *
	 * - Performance Boost (B) region
	 *
	 *   Condition: nrg_delta > 0 && cap_delta > 0
	 *   Payoff criteria:
	 *     cap_gain / nrg_gain  < cap_delta / nrg_delta =
	 *     cap_gain * nrg_delta < cap_delta * nrg_gain
	 *   Note that since both nrg_gain and nrg_delta are positive, the
	 *   inequality does not change. Thus:
	 *
	 *     payoff = (cap_delta * nrg_gain) - (cap_gain * nrg_delta)
	 *
	 * - Performance Constraint (C) region
	 *
	 *   Condition: nrg_delta < 0 && cap_delta < 0
	 *   payoff criteria:
	 *     cap_gain / nrg_gain  > cap_delta / nrg_delta =
	 *     cap_gain * nrg_delta < cap_delta * nrg_gain
	 *   Note that since nrg_gain > 0 while nrg_delta < 0, the
	 *   inequality change. Thus:
	 *
	 *     payoff = (cap_delta * nrg_gain) - (cap_gain * nrg_delta)
	 *
	 * This means that, in case of same positive defined {cap,nrg}_gain
	 * for both the B and C regions, we can use the same payoff formula
	 * where a positive value represents the accept condition.
	 */
	payoff  = cap_delta * threshold_gains[gain_idx].nrg_gain;
	payoff -= nrg_delta * threshold_gains[gain_idx].cap_gain;

	return payoff;
}

#ifdef CONFIG_CGROUP_SCHEDTUNE

struct group_balancer {
	/* sum of task utilization in group */
	unsigned long util;

	/* group balancing threshold */
	unsigned long threshold;

	/* imbalance ratio by heaviest task */
	unsigned int imbalance_ratio;

	/* balance ratio by heaviest task */
	unsigned int balance_ratio;

	/* heaviest task utilization in group */
	unsigned long heaviest_util;

	/* group utilization update interval */
	unsigned long update_interval;

	/* next group utilization update time */
	unsigned long next_update_time;

	/*
	 * group imbalance time = imbalance_count * update_interval
	 * imbalance_count >= imbalance_duration -> need balance
	 */
	unsigned int imbalance_duration;
	unsigned int imbalance_count;

	/* utilization tracking window size */
	unsigned long window;

	/* group balancer locking */
	raw_spinlock_t lock;

	/* need group balancing? */
	bool need_balance;
};

/*
 * EAS scheduler tunables for task groups.
 */

/* SchdTune tunables for a group of tasks */
struct schedtune {
	/* SchedTune CGroup subsystem */
	struct cgroup_subsys_state css;

	/* Boost group allocated ID */
	int idx;

	/* Boost value for tasks on that SchedTune CGroup */
	int boost;

	/* Performance Boost (B) region threshold params */
	int perf_boost_idx;

	/* Performance Constraint (C) region threshold params */
	int perf_constrain_idx;

	/* Hint to bias scheduling of tasks on that SchedTune CGroup
	 * towards idle CPUs */
	int prefer_idle;

	/* Hint to bias scheduling of tasks on that SchedTune CGroup
	 * towards high performance CPUs */
	int prefer_perf;

	/* SchedTune group balancer */
	struct group_balancer gb;
#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/*
	 * This tracks the default boost value and is used to restore
	 * the value when Dynamic SchedTune Boost is reset.
	 */
	int boost_default;

	/* Sched Boost value for tasks on that SchedTune CGroup */
	int sched_boost;

	/* Number of ongoing boosts for this SchedTune CGroup */
	int boost_count;

	/* Lists of active and available boost slots */
	struct boost_slot active_boost_slots;
	struct boost_slot available_boost_slots;

	/* Array of tracked boost values of each slot */
	int slot_boost[DYNAMIC_BOOST_SLOTS_COUNT];
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */
};

static inline struct schedtune *css_st(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct schedtune, css) : NULL;
}

static inline struct schedtune *task_schedtune(struct task_struct *tsk)
{
	return css_st(task_css(tsk, schedtune_cgrp_id));
}

static inline struct schedtune *parent_st(struct schedtune *st)
{
	return css_st(st->css.parent);
}

/*
 * SchedTune root control group
 * The root control group is used to defined a system-wide boosting tuning,
 * which is applied to all tasks in the system.
 * Task specific boost tuning could be specified by creating and
 * configuring a child control group under the root one.
 * By default, system-wide boosting is disabled, i.e. no boosting is applied
 * to tasks which are not into a child control group.
 */
static struct schedtune
root_schedtune = {
	.boost	= 0,
	.perf_boost_idx = 0,
	.perf_constrain_idx = 0,
	.prefer_idle = 0,
	.prefer_perf = 0,
#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	.boost_default = 0,
	.sched_boost = 0,
	.boost_count = 0,
	.active_boost_slots = {
		.list = LIST_HEAD_INIT(root_schedtune.active_boost_slots.list),
		.idx = 0,
	},
	.available_boost_slots = {
		.list = LIST_HEAD_INIT(root_schedtune.available_boost_slots.list),
		.idx = 0,
	},
	.slot_boost = {0},
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */
};

int
schedtune_accept_deltas(int nrg_delta, int cap_delta,
			struct task_struct *task)
{
	struct schedtune *ct;
	int perf_boost_idx;
	int perf_constrain_idx;

	/* Optimal (O) region */
	if (nrg_delta < 0 && cap_delta > 0) {
		trace_sched_tune_filter(nrg_delta, cap_delta, 0, 0, 1, 0);
		return INT_MAX;
	}

	/* Suboptimal (S) region */
	if (nrg_delta > 0 && cap_delta < 0) {
		trace_sched_tune_filter(nrg_delta, cap_delta, 0, 0, -1, 5);
		return -INT_MAX;
	}

	/* Get task specific perf Boost/Constraints indexes */
	rcu_read_lock();
	ct = task_schedtune(task);
	perf_boost_idx = ct->perf_boost_idx;
	perf_constrain_idx = ct->perf_constrain_idx;
	rcu_read_unlock();

	return __schedtune_accept_deltas(nrg_delta, cap_delta,
			perf_boost_idx, perf_constrain_idx);
}

/*
 * Maximum number of boost groups to support
 * When per-task boosting is used we still allow only limited number of
 * boost groups for two main reasons:
 * 1. on a real system we usually have only few classes of workloads which
 *    make sense to boost with different values (e.g. background vs foreground
 *    tasks, interactive vs low-priority tasks)
 * 2. a limited number allows for a simpler and more memory/time efficient
 *    implementation especially for the computation of the per-CPU boost
 *    value
 */
#define BOOSTGROUPS_COUNT 7

/* Array of configured boostgroups */
static struct schedtune *allocated_group[BOOSTGROUPS_COUNT] = {
	&root_schedtune,
	NULL,
};

/* SchedTune boost groups
 * Keep track of all the boost groups which impact on CPU, for example when a
 * CPU has two RUNNABLE tasks belonging to two different boost groups and thus
 * likely with different boost values.
 * Since on each system we expect only a limited number of boost groups, here
 * we use a simple array to keep track of the metrics required to compute the
 * maximum per-CPU boosting value.
 */
struct boost_groups {
	/* Maximum boost value for all RUNNABLE tasks on a CPU */
	bool idle;
	int boost_max;
	u64 boost_ts;
	struct {
		/* The boost for tasks on that boost group */
		int boost;
		/* Count of RUNNABLE tasks on that boost group */
		unsigned tasks;
		/* Timestamp of boost activation */
		u64 ts;
	} group[BOOSTGROUPS_COUNT];
	/* CPU's boost group locking */
	raw_spinlock_t lock;
};

/* Boost groups affecting each CPU in the system */
DEFINE_PER_CPU(struct boost_groups, cpu_boost_groups);

static inline bool schedtune_boost_timeout(u64 now, u64 ts)
{
	return ((now - ts) > SCHEDTUNE_BOOST_HOLD_NS);
}

static inline bool
schedtune_boost_group_active(int idx, struct boost_groups* bg, u64 now)
{
	if (bg->group[idx].tasks)
		return true;

	return !schedtune_boost_timeout(now, bg->group[idx].ts);
}

static void
schedtune_cpu_update(int cpu, u64 now)
{
	struct boost_groups *bg;
	u64 boost_ts = now;
	int boost_max = INT_MIN;
	int idx;

	bg = &per_cpu(cpu_boost_groups, cpu);

	/* The root boost group is always active */
	boost_max = bg->group[0].boost;
	for (idx = 1; idx < BOOSTGROUPS_COUNT; ++idx) {
		/*
		 * A boost group affects a CPU only if it has
		 * RUNNABLE tasks on that CPU or it has hold
		 * in effect from a previous task.
		 */
		if (!schedtune_boost_group_active(idx, bg, now))
			continue;

		/* this boost group is active */
		if (boost_max > bg->group[idx].boost)
			continue;

		boost_max = bg->group[idx].boost;
		boost_ts =  bg->group[idx].ts;
	}
	/* Ensures boost_max is non-negative when all cgroup boost values
	 * are neagtive. Avoids under-accounting of cpu capacity which may cause
	 * task stacking and frequency spikes.*/
	boost_max = max(boost_max, 0);
	bg->boost_max = boost_max;
	bg->boost_ts = boost_ts;
}

static int
schedtune_boostgroup_update(int idx, int boost)
{
	struct boost_groups *bg;
	int cur_boost_max;
	int old_boost;
	int cpu;
	u64 now;

	/* Update per CPU boost groups */
	for_each_possible_cpu(cpu) {
		bg = &per_cpu(cpu_boost_groups, cpu);

		/*
		 * Keep track of current boost values to compute the per CPU
		 * maximum only when it has been affected by the new value of
		 * the updated boost group
		 */
		cur_boost_max = bg->boost_max;
		old_boost = bg->group[idx].boost;

		/* Update the boost value of this boost group */
		bg->group[idx].boost = boost;

		now = sched_clock_cpu(cpu);
		/*
		 * Check if this update increase current max.
		 */
		if (boost > cur_boost_max &&
			schedtune_boost_group_active(idx, bg, now)) {
			bg->boost_max = boost;
			bg->boost_ts = bg->group[idx].ts;

			trace_sched_tune_boostgroup_update(cpu, 1, bg->boost_max);
			continue;
		}

		/* Check if this update has decreased current max */
		if (cur_boost_max == old_boost && old_boost > boost) {
			schedtune_cpu_update(cpu, now);
			trace_sched_tune_boostgroup_update(cpu, -1, bg->boost_max);
			continue;
		}

		trace_sched_tune_boostgroup_update(cpu, 0, bg->boost_max);
	}

	return 0;
}

#define ENQUEUE_TASK  1
#define DEQUEUE_TASK -1

static inline bool
schedtune_update_timestamp(struct task_struct *p)
{
	if (sched_feat(SCHEDTUNE_BOOST_HOLD_ALL))
		return true;

	return task_has_rt_policy(p);
}

static inline void
schedtune_tasks_update(struct task_struct *p, int cpu, int idx, int task_count)
{
	struct boost_groups *bg = &per_cpu(cpu_boost_groups, cpu);
	int tasks = bg->group[idx].tasks + task_count;
	u64 now;

	/* Update boosted tasks count while avoiding to make it negative */
	bg->group[idx].tasks = max(0, tasks);

	/* Update timeout on enqueue */
	if (task_count > 0) {
		now = sched_clock_cpu(cpu);
		if (schedtune_update_timestamp(p))
			bg->group[idx].ts = now;

		/* Boost group activation or deactivation on that RQ */
		if (bg->group[idx].tasks == 1)
			schedtune_cpu_update(cpu, now);
	}

	trace_sched_tune_tasks_update(p, cpu, tasks, idx,
			bg->group[idx].boost, bg->boost_max,
			bg->group[idx].ts);
}

/*
 * NOTE: This function must be called while holding the lock on the CPU RQ
 */
void schedtune_enqueue_task(struct task_struct *p, int cpu)
{
	struct boost_groups *bg = &per_cpu(cpu_boost_groups, cpu);
	unsigned long irq_flags;
	struct schedtune *st;
	int idx;

	if (!unlikely(schedtune_initialized))
		return;

	/*
	 * When a task is marked PF_EXITING by do_exit() it's going to be
	 * dequeued and enqueued multiple times in the exit path.
	 * Thus we avoid any further update, since we do not want to change
	 * CPU boosting while the task is exiting.
	 */
	if (p->flags & PF_EXITING)
		return;

	/*
	 * Boost group accouting is protected by a per-cpu lock and requires
	 * interrupt to be disabled to avoid race conditions for example on
	 * do_exit()::cgroup_exit() and task migration.
	 */
	raw_spin_lock_irqsave(&bg->lock, irq_flags);
	rcu_read_lock();

	st = task_schedtune(p);
	idx = st->idx;

	schedtune_tasks_update(p, cpu, idx, ENQUEUE_TASK);

	rcu_read_unlock();
	raw_spin_unlock_irqrestore(&bg->lock, irq_flags);
}

int schedtune_can_attach(struct cgroup_taskset *tset)
{
	struct task_struct *task;
	struct cgroup_subsys_state *css;
	struct boost_groups *bg;
	struct rq_flags irq_flags;
	unsigned int cpu;
	struct rq *rq;
	int src_bg; /* Source boost group index */
	int dst_bg; /* Destination boost group index */
	int tasks;
	u64 now;

	if (!unlikely(schedtune_initialized))
		return 0;


	cgroup_taskset_for_each(task, css, tset) {

		/*
		 * Lock the CPU's RQ the task is enqueued to avoid race
		 * conditions with migration code while the task is being
		 * accounted
		 */
		rq = lock_rq_of(task, &irq_flags);

		if (!task->on_rq) {
			unlock_rq_of(rq, task, &irq_flags);
			continue;
		}

		/*
		 * Boost group accouting is protected by a per-cpu lock and requires
		 * interrupt to be disabled to avoid race conditions on...
		 */
		cpu = cpu_of(rq);
		bg = &per_cpu(cpu_boost_groups, cpu);
		raw_spin_lock(&bg->lock);

		dst_bg = css_st(css)->idx;
		src_bg = task_schedtune(task)->idx;

		/*
		 * Current task is not changing boostgroup, which can
		 * happen when the new hierarchy is in use.
		 */
		if (unlikely(dst_bg == src_bg)) {
			raw_spin_unlock(&bg->lock);
			unlock_rq_of(rq, task, &irq_flags);
			continue;
		}

		/*
		 * This is the case of a RUNNABLE task which is switching its
		 * current boost group.
		 */

		now = sched_clock_cpu(cpu);

		/* Move task from src to dst boost group */
		tasks = bg->group[src_bg].tasks - 1;
		bg->group[src_bg].tasks = max(0, tasks);
		bg->group[dst_bg].tasks += 1;
		bg->group[dst_bg].ts = now;

		/* update next time someone asks */
		bg->boost_ts = now - SCHEDTUNE_BOOST_HOLD_NS;

		raw_spin_unlock(&bg->lock);
		unlock_rq_of(rq, task, &irq_flags);
	}

	return 0;
}

void schedtune_cancel_attach(struct cgroup_taskset *tset)
{
	/* This can happen only if SchedTune controller is mounted with
	 * other hierarchies ane one of them fails. Since usually SchedTune is
	 * mouted on its own hierarcy, for the time being we do not implement
	 * a proper rollback mechanism */
	WARN(1, "SchedTune cancel attach not implemented");
}

/*
 * NOTE: This function must be called while holding the lock on the CPU RQ
 */
void schedtune_dequeue_task(struct task_struct *p, int cpu)
{
	struct boost_groups *bg = &per_cpu(cpu_boost_groups, cpu);
	unsigned long irq_flags;
	struct schedtune *st;
	int idx;

	if (!unlikely(schedtune_initialized))
		return;

	/*
	 * When a task is marked PF_EXITING by do_exit() it's going to be
	 * dequeued and enqueued multiple times in the exit path.
	 * Thus we avoid any further update, since we do not want to change
	 * CPU boosting while the task is exiting.
	 * The last dequeue is already enforce by the do_exit() code path
	 * via schedtune_exit_task().
	 */
	if (p->flags & PF_EXITING)
		return;

	/*
	 * Boost group accouting is protected by a per-cpu lock and requires
	 * interrupt to be disabled to avoid race conditions on...
	 */
	raw_spin_lock_irqsave(&bg->lock, irq_flags);
	rcu_read_lock();

	st = task_schedtune(p);
	idx = st->idx;

	schedtune_tasks_update(p, cpu, idx, DEQUEUE_TASK);

	rcu_read_unlock();
	raw_spin_unlock_irqrestore(&bg->lock, irq_flags);
}

void schedtune_exit_task(struct task_struct *tsk)
{
	struct schedtune *st;
	struct rq_flags irq_flags;
	unsigned int cpu;
	struct rq *rq;
	int idx;

	if (!unlikely(schedtune_initialized))
		return;

	rq = lock_rq_of(tsk, &irq_flags);
	rcu_read_lock();

	cpu = cpu_of(rq);
	st = task_schedtune(tsk);
	idx = st->idx;
	schedtune_tasks_update(tsk, cpu, idx, DEQUEUE_TASK);

	rcu_read_unlock();
	unlock_rq_of(rq, tsk, &irq_flags);
}

int schedtune_cpu_boost(int cpu)
{
	struct boost_groups *bg;
	u64 now;

	bg = &per_cpu(cpu_boost_groups, cpu);
	now = sched_clock_cpu(cpu);

	/* check to see if we have a hold in effect */
	if (schedtune_boost_timeout(now, bg->boost_ts))
		schedtune_cpu_update(cpu, now);

	return bg->boost_max;
}

int schedtune_task_boost(struct task_struct *p)
{
	struct schedtune *st;
	int task_boost;

	if (!unlikely(schedtune_initialized))
		return 0;

	/* Get task boost value */
	rcu_read_lock();
	st = task_schedtune(p);
	task_boost = st->boost;
	rcu_read_unlock();

	return task_boost;
}

int schedtune_prefer_idle(struct task_struct *p)
{
	struct schedtune *st;
	int prefer_idle;

	if (!unlikely(schedtune_initialized))
		return 0;

	/* Get prefer_idle value */
	rcu_read_lock();
	st = task_schedtune(p);
	prefer_idle = st->prefer_idle;
	rcu_read_unlock();

	return prefer_idle;
}

#ifdef CONFIG_SCHED_EHMP
static atomic_t kernel_prefer_perf_req[BOOSTGROUPS_COUNT];
int kernel_prefer_perf(int grp_idx)
{
	if (grp_idx >= BOOSTGROUPS_COUNT)
		return -EINVAL;

	return atomic_read(&kernel_prefer_perf_req[grp_idx]);
}

void request_kernel_prefer_perf(int grp_idx, int enable)
{
	if (grp_idx >= BOOSTGROUPS_COUNT)
		return;

	if (enable)
		atomic_inc(&kernel_prefer_perf_req[grp_idx]);
	else
		BUG_ON(atomic_dec_return(&kernel_prefer_perf_req[grp_idx]) < 0);
}
#else
static inline int kernel_prefer_perf(int grp_idx) { return 0; }
#endif


int schedtune_prefer_perf(struct task_struct *p)
{
	struct schedtune *st;
	int prefer_perf;

	if (!unlikely(schedtune_initialized))
		return 0;

	/* Get prefer_perf value */
	rcu_read_lock();
	st = task_schedtune(p);
	prefer_perf = max(st->prefer_perf, kernel_prefer_perf(st->idx));
	rcu_read_unlock();

	return prefer_perf;
}

int schedtune_need_group_balance(struct task_struct *p)
{
	bool balance;

	if (!unlikely(schedtune_initialized))
		return 0;

	rcu_read_lock();
	balance = task_schedtune(p)->gb.need_balance;
	rcu_read_unlock();

	return balance;
}

static inline void
check_need_group_balance(int group_idx, struct group_balancer *gb)
{
	int heaviest_ratio;

	if (!gb->util) {
		gb->imbalance_count = 0;
		gb->need_balance = false;

		goto out;
	}

	heaviest_ratio = gb->heaviest_util * 100 / gb->util;

	if (gb->need_balance) {
		if (gb->util < gb->threshold || heaviest_ratio < gb->balance_ratio) {
			gb->imbalance_count = 0;
			gb->need_balance = false;
		}

		goto out;
	}

	if (gb->util >= gb->threshold && heaviest_ratio > gb->imbalance_ratio) {
		gb->imbalance_count++;

		if (gb->imbalance_count >= gb->imbalance_duration)
			gb->need_balance = true;
	} else {
		gb->imbalance_count = 0;
	}

out:
	trace_sched_tune_check_group_balance(group_idx,
				gb->imbalance_count, gb->need_balance);
}

static void __schedtune_group_util_update(struct schedtune *st)
{
	struct group_balancer *gb = &st->gb;
	unsigned long now = cpu_rq(0)->clock_task;
	struct css_task_iter it;
	struct task_struct *p;
	struct task_struct *heaviest_p = NULL;
	unsigned long util_sum = 0;
	unsigned long heaviest_util = 0;
	unsigned int total = 0, accumulated = 0;

	if (!raw_spin_trylock(&gb->lock))
		return;

	if (!gb->update_interval)
		goto out;

	if (time_before(now, gb->next_update_time))
		goto out;

	css_task_iter_start(&st->css, &it);
	while ((p = css_task_iter_next(&it))) {
		unsigned long clock_task, delta, util;

		total++;

		clock_task = task_rq(p)->clock_task;
		delta = clock_task - p->se.avg.last_update_time;
		if (p->se.avg.last_update_time && delta > gb->window)
			continue;

		util = p->se.avg.util_avg;
		if (util > heaviest_util) {
			heaviest_util = util;
			heaviest_p = p;
		}

		util_sum += p->se.avg.util_avg;
		accumulated++;
	}
	css_task_iter_end(&it);

	gb->util = util_sum;
	gb->heaviest_util = heaviest_util;
	gb->next_update_time = now + gb->update_interval;

	/* if there is no task in group, heaviest_p is always NULL */
	if (heaviest_p)
		trace_sched_tune_grouputil_update(st->idx, total, accumulated,
				gb->util, heaviest_p, gb->heaviest_util);

	check_need_group_balance(st->idx, gb);
out:
	raw_spin_unlock(&gb->lock);
}

void schedtune_group_util_update(void)
{
	int idx;

	if (!unlikely(schedtune_initialized))
		return;

	rcu_read_lock();

	for (idx = 1; idx < BOOSTGROUPS_COUNT; idx++) {
		struct schedtune *st = allocated_group[idx];

		if (!st)
			continue;
		__schedtune_group_util_update(st);
	}

	rcu_read_unlock();
}

static u64
gb_util_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->gb.util;
}

static u64
gb_heaviest_ratio_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	if (!st->gb.util)
		return 0;

	return st->gb.heaviest_util * 100 / st->gb.util;
}

static u64
gb_threshold_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->gb.threshold;
}

static int
gb_threshold_write(struct cgroup_subsys_state *css, struct cftype *cft,
	    u64 threshold)
{
	struct schedtune *st = css_st(css);
	struct group_balancer *gb = &st->gb;

	gb->threshold = threshold;

	return 0;
}

static u64
gb_imbalance_ratio_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->gb.imbalance_ratio;
}

static int
gb_imbalance_ratio_write(struct cgroup_subsys_state *css, struct cftype *cft,
	    u64 ratio)
{
	struct schedtune *st = css_st(css);
	struct group_balancer *gb = &st->gb;

	ratio = min_t(u64, ratio, 100);

	gb->imbalance_ratio = ratio;

	return 0;
}

static u64
gb_balance_ratio_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->gb.balance_ratio;
}

static int
gb_balance_ratio_write(struct cgroup_subsys_state *css, struct cftype *cft,
	    u64 ratio)
{
	struct schedtune *st = css_st(css);
	struct group_balancer *gb = &st->gb;

	ratio = min_t(u64, ratio, 100);

	gb->balance_ratio = ratio;

	return 0;
}

static u64
gb_interval_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->gb.update_interval / NSEC_PER_USEC;
}

static int
gb_interval_write(struct cgroup_subsys_state *css, struct cftype *cft,
	    u64 interval_us)
{
	struct schedtune *st = css_st(css);
	struct group_balancer *gb = &st->gb;

	gb->update_interval = interval_us * NSEC_PER_USEC;
	if (!interval_us) {
		gb->util = 0;
		gb->need_balance = false;
	}

	return 0;
}

static u64
gb_duration_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->gb.imbalance_duration;
}

static int
gb_duration_write(struct cgroup_subsys_state *css, struct cftype *cft,
	    u64 duration)
{
	struct schedtune *st = css_st(css);
	struct group_balancer *gb = &st->gb;

	gb->imbalance_duration = duration;

	return 0;
}

static u64
gb_window_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->gb.window / NSEC_PER_MSEC;
}

static int
gb_window_write(struct cgroup_subsys_state *css, struct cftype *cft,
	    u64 window)
{
	struct schedtune *st = css_st(css);
	struct group_balancer *gb = &st->gb;

	gb->window = window * NSEC_PER_MSEC;

	return 0;
}

static u64
prefer_idle_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->prefer_idle;
}

static int
prefer_idle_write(struct cgroup_subsys_state *css, struct cftype *cft,
	    u64 prefer_idle)
{
	struct schedtune *st = css_st(css);
	st->prefer_idle = prefer_idle;

	return 0;
}

static u64
prefer_perf_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->prefer_perf;
}

static int
prefer_perf_write(struct cgroup_subsys_state *css, struct cftype *cft,
	    u64 prefer_perf)
{
	struct schedtune *st = css_st(css);
	st->prefer_perf = prefer_perf;

	return 0;
}

static s64
boost_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->boost;
}

static int
boost_write(struct cgroup_subsys_state *css, struct cftype *cft,
	    s64 boost)
{
	struct schedtune *st = css_st(css);
	unsigned threshold_idx;
	int boost_pct;

	if (boost < -100 || boost > 100)
		return -EINVAL;
	boost_pct = boost;

	/*
	 * Update threshold params for Performance Boost (B)
	 * and Performance Constraint (C) regions.
	 * The current implementatio uses the same cuts for both
	 * B and C regions.
	 */
	threshold_idx = clamp(boost_pct, 0, 99) / 10;
	st->perf_boost_idx = threshold_idx;
	st->perf_constrain_idx = threshold_idx;

	st->boost = boost;
#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	st->boost_default = boost;
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */
	if (css == &root_schedtune.css) {
		sysctl_sched_cfs_boost = boost;
		perf_boost_idx  = threshold_idx;
		perf_constrain_idx  = threshold_idx;
	}

	/* Update CPU boost */
	schedtune_boostgroup_update(st->idx, st->boost);

	trace_sched_tune_config(st->boost);

	return 0;
}

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
static s64
sched_boost_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct schedtune *st = css_st(css);

	return st->sched_boost;
}

static int
sched_boost_write(struct cgroup_subsys_state *css, struct cftype *cft,
	    s64 sched_boost)
{
	struct schedtune *st = css_st(css);
	st->sched_boost = sched_boost;

	return 0;
}

static void
boost_slots_init(struct schedtune *st)
{
	int i;
	struct boost_slot *slot;

	/* Initialize boost slots */
	INIT_LIST_HEAD(&(st->active_boost_slots.list));
	INIT_LIST_HEAD(&(st->available_boost_slots.list));

	/* Populate available_boost_slots */
	for (i = 0; i < DYNAMIC_BOOST_SLOTS_COUNT; ++i) {
		slot = kmalloc(sizeof(*slot), GFP_KERNEL);
		slot->idx = i;
		list_add_tail(&(slot->list), &(st->available_boost_slots.list));
	}
}

static void
boost_slots_release(struct schedtune *st)
{
	struct boost_slot *slot, *next_slot;

	list_for_each_entry_safe(slot, next_slot,
				 &(st->available_boost_slots.list), list) {
		list_del(&slot->list);
		pr_info("STUNE: freed!\n");
		kfree(slot);
	}
	list_for_each_entry_safe(slot, next_slot,
				 &(st->active_boost_slots.list), list) {
		list_del(&slot->list);
		pr_info("STUNE: freed!\n");
		kfree(slot);
	}
}
#endif // CONFIG_DYNAMIC_STUNE_BOOST

static struct cftype files[] = {
	{
		.name = "boost",
		.read_s64 = boost_read,
		.write_s64 = boost_write,
	},
	{
		.name = "prefer_idle",
		.read_u64 = prefer_idle_read,
		.write_u64 = prefer_idle_write,
	},
	{
		.name = "prefer_perf",
		.read_u64 = prefer_perf_read,
		.write_u64 = prefer_perf_write,
	},
	{
		.name = "gb_util",
		.read_u64 = gb_util_read,
	},
	{
		.name = "gb_heaviest_ratio",
		.read_u64 = gb_heaviest_ratio_read,
	},
	{
		.name = "gb_threshold",
		.read_u64 = gb_threshold_read,
		.write_u64 = gb_threshold_write,
	},
	{
		.name = "gb_imbalance_ratio",
		.read_u64 = gb_imbalance_ratio_read,
		.write_u64 = gb_imbalance_ratio_write,
	},
	{
		.name = "gb_balance_ratio",
		.read_u64 = gb_balance_ratio_read,
		.write_u64 = gb_balance_ratio_write,
	},
	{
		.name = "gb_interval_us",
		.read_u64 = gb_interval_read,
		.write_u64 = gb_interval_write,
	},
	{
		.name = "gb_duration",
		.read_u64 = gb_duration_read,
		.write_u64 = gb_duration_write,
	},
	{
		.name = "gb_window_ms",
		.read_u64 = gb_window_read,
		.write_u64 = gb_window_write,
	},
#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	{
		.name = "sched_boost",
		.read_s64 = sched_boost_read,
		.write_s64 = sched_boost_write,
	},
#endif // CONFIG_DYNAMIC_STUNE_BOOST
	{ }	/* terminate */
};

static int
schedtune_boostgroup_init(struct schedtune *st)
{
	struct boost_groups *bg;
	int cpu;
	int idx;

	/* Keep track of allocated boost groups */
	allocated_group[st->idx] = st;

	/* Initialize the per CPU boost groups */
	for_each_possible_cpu(cpu) {
		bg = &per_cpu(cpu_boost_groups, cpu);
		bg->group[st->idx].boost = 0;
		bg->group[st->idx].tasks = 0;
		bg->group[idx].ts = 0;

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	boost_slots_init(st);
#endif // CONFIG_DYNAMIC_STUNE_BOOST
	}

	return 0;
}

static void
schedtune_group_balancer_init(struct schedtune *st)
{
	raw_spin_lock_init(&st->gb.lock);

	st->gb.threshold = ULONG_MAX;
	st->gb.imbalance_ratio = 0;				/* 0% */
	st->gb.update_interval = 0;				/* disable update */
	st->gb.next_update_time = cpu_rq(0)->clock_task;

	st->gb.imbalance_duration = 0;
	st->gb.imbalance_count = 0;

	st->gb.window = 100 * NSEC_PER_MSEC;		/* 100ms */

}

#ifdef CONFIG_STUNE_ASSIST
static void write_default_values(struct cgroup_subsys_state *css)
{
	u8 i;
	struct groups_data {
		char *name;
		int boost;
		bool prefer_idle;
		bool colocate;
		bool no_override;
	};
	struct groups_data groups[3] = {
		{ "top-app",	5, 1, 0, 0 },
		{ "foreground", 0, 1, 0, 0 },
		{ "background", 0, 0, 0, 0 }};

	for (i = 0; i < ARRAY_SIZE(groups); i++) {
		if (!strcmp(css->cgroup->kn->name, groups[i].name)) {
			boost_write(css, NULL, groups[i].boost);
			prefer_idle_write(css, NULL, groups[i].prefer_idle);
			sched_colocate_write(css, NULL, groups[i].colocate);
			sched_boost_override_write(css, NULL, groups[i].no_override);
		}
	}
}
#endif

static struct cgroup_subsys_state *
schedtune_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct schedtune *st;
	int idx;

	if (!parent_css)
		return &root_schedtune.css;

	/* Allow only single level hierachies */
	if (parent_css != &root_schedtune.css) {
		pr_err("Nested SchedTune boosting groups not allowed\n");
		return ERR_PTR(-ENOMEM);
	}

	/* Allow only a limited number of boosting groups */
#ifdef CONFIG_STUNE_ASSIST
	for (idx = 1; idx < BOOSTGROUPS_COUNT; ++idx) {
		if (!allocated_group[idx])
			break;
		write_default_values(&allocated_group[idx]->css);
	}
#else
	for (idx = 1; idx < BOOSTGROUPS_COUNT; ++idx)
		if (!allocated_group[idx])
			break;
#endif
	if (idx == BOOSTGROUPS_COUNT) {
		pr_err("Trying to create more than %d SchedTune boosting groups\n",
		       BOOSTGROUPS_COUNT);
		return ERR_PTR(-ENOSPC);
	}

	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		goto out;

	schedtune_group_balancer_init(st);

	/* Initialize per CPUs boost group support */
	st->idx = idx;
	if (schedtune_boostgroup_init(st))
		goto release;

	return &st->css;

release:
	kfree(st);
out:
	return ERR_PTR(-ENOMEM);
}

static void
schedtune_boostgroup_release(struct schedtune *st)
{
	/* Reset this boost group */
	schedtune_boostgroup_update(st->idx, 0);
#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/* Free dynamic boost slots */
	boost_slots_release(st);
#endif // CONFIG_DYNAMIC_STUNE_BOOST

	/* Keep track of allocated boost groups */
	allocated_group[st->idx] = NULL;
}

static void
schedtune_css_free(struct cgroup_subsys_state *css)
{
	struct schedtune *st = css_st(css);

	schedtune_boostgroup_release(st);
	kfree(st);
}

struct cgroup_subsys schedtune_cgrp_subsys = {
	.css_alloc	= schedtune_css_alloc,
	.css_free	= schedtune_css_free,
	.can_attach     = schedtune_can_attach,
	.cancel_attach  = schedtune_cancel_attach,
	.legacy_cftypes	= files,
	.early_init	= 1,
};

static inline void
schedtune_init_cgroups(void)
{
	struct boost_groups *bg;
	int cpu;

	/* Initialize the per CPU boost groups */
	for_each_possible_cpu(cpu) {
		bg = &per_cpu(cpu_boost_groups, cpu);
		memset(bg, 0, sizeof(struct boost_groups));
		raw_spin_lock_init(&bg->lock);
	}

	pr_info("schedtune: configured to support %d boost groups\n",
		BOOSTGROUPS_COUNT);

	schedtune_initialized = true;
}

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
static struct schedtune *getSchedtune(char *st_name)
{
	int idx;

	for (idx = 0; idx < BOOSTGROUPS_COUNT; ++idx) {
		char name_buf[NAME_MAX + 1];
		struct schedtune *st = allocated_group[idx];

		if (!st) {
			pr_warn("SCHEDTUNE: Could not find %s\n", st_name);
			break;
		}

		cgroup_name(st->css.cgroup, name_buf, sizeof(name_buf));
		if (strncmp(name_buf, st_name, strlen(st_name)) == 0)
			return st;
	}

	return NULL;
}

static int dynamic_boost(struct schedtune *st, int boost)
{
	int ret;
	/* Backup boost_default */
	int boost_default_backup = st->boost_default;

	ret = boost_write(&st->css, NULL, boost);

	/* Restore boost_default */
	st->boost_default = boost_default_backup;

	return ret;
}

static inline bool is_valid_boost_slot(int slot)
{
	return slot >= 0 && slot < DYNAMIC_BOOST_SLOTS_COUNT;
}

static int activate_boost_slot(struct schedtune *st, int boost, int *slot)
{
	int ret = 0;
	struct boost_slot *curr_slot;
	struct list_head *head;
	*slot = -1;

	mutex_lock(&boost_slot_mutex);

	/* Check for slots in available_boost_slots */
	if (list_empty(&(st->available_boost_slots.list))) {
		ret = -EINVAL;
		goto exit;
	}

	/*
	 * Move one slot from available_boost_slots to active_boost_slots
	 */

	/* Get first slot from available_boost_slots */
	head = &(st->available_boost_slots.list);
	curr_slot = list_first_entry(head, struct boost_slot, list);

	/* Store slot value and boost value*/
	*slot = curr_slot->idx;
	st->slot_boost[*slot] = boost;

	/* Delete slot from available_boost_slots */
	list_del(&curr_slot->list);
	kfree(curr_slot);

	/* Create new slot with same value at tail of active_boost_slots */
	curr_slot = kmalloc(sizeof(*curr_slot), GFP_KERNEL);
	curr_slot->idx = *slot;
	list_add_tail(&(curr_slot->list),
		&(st->active_boost_slots.list));

exit:
	mutex_unlock(&boost_slot_mutex);
	return ret;
}

static int deactivate_boost_slot(struct schedtune *st, int slot)
{
	int ret = 0;
	struct boost_slot *curr_slot, *next_slot;

	mutex_lock(&boost_slot_mutex);

	if (!is_valid_boost_slot(slot)) {
		ret = -EINVAL;
		goto exit;
	}

	/* Delete slot from active_boost_slots */
	list_for_each_entry_safe(curr_slot, next_slot,
				 &(st->active_boost_slots.list), list) {
		if (curr_slot->idx == slot) {
			st->slot_boost[slot] = 0;
			list_del(&curr_slot->list);
			kfree(curr_slot);

			/* Create same slot at tail of available_boost_slots */
			curr_slot = kmalloc(sizeof(*curr_slot), GFP_KERNEL);
			curr_slot->idx = slot;
			list_add_tail(&(curr_slot->list),
				      &(st->available_boost_slots.list));

			goto exit;
		}
	}

	/* Reaching here means that we did not find the slot to delete */
	ret = -EINVAL;

exit:
	mutex_unlock(&boost_slot_mutex);
	return ret;
}

static int max_active_boost(struct schedtune *st)
{
	struct boost_slot *slot;
	int max_boost;

	mutex_lock(&boost_slot_mutex);
	mutex_lock(&stune_boost_mutex);

	/* Set initial value to default boost */
	max_boost = st->boost_default;

	/* Check for active boosts */
	if (list_empty(&(st->active_boost_slots.list))) {
		goto exit;
	}

	/* Get largest boost value */
	list_for_each_entry(slot, &(st->active_boost_slots.list), list) {
		int boost = st->slot_boost[slot->idx];
		if (boost > max_boost)
			max_boost = boost;
	}

exit:
	mutex_unlock(&stune_boost_mutex);
	mutex_unlock(&boost_slot_mutex);

	return max_boost;
}

static int _do_stune_boost(struct schedtune *st, int boost, int *slot)
{
	int ret = 0;

	/* Try to obtain boost slot */
	ret = activate_boost_slot(st, boost, slot);

	/* Check if boost slot obtained successfully */
	if (ret)
		return -EINVAL;

	/* Boost if new value is greater than current */
	mutex_lock(&stune_boost_mutex);
	if (boost > st->boost)
		ret = dynamic_boost(st, boost);
	mutex_unlock(&stune_boost_mutex);

	return ret;
}

int reset_stune_boost(char *st_name, int slot)
{
	int ret = 0;
	int boost = 0;
	struct schedtune *st = getSchedtune(st_name);

	if (!st)
		return -EINVAL;

	ret = deactivate_boost_slot(st, slot);
	if (ret) {
		return -EINVAL;
	}
	/* Find next largest active boost or reset to default */
	boost = max_active_boost(st);

	mutex_lock(&stune_boost_mutex);
	/* Boost only if value changed */
	if (boost != st->boost)
		ret = dynamic_boost(st, boost);
	mutex_unlock(&stune_boost_mutex);

	return ret;
}

int do_stune_sched_boost(char *st_name, int *slot)
{
	struct schedtune *st = getSchedtune(st_name);

	if (!st)
		return -EINVAL;

	return _do_stune_boost(st, st->sched_boost, slot);
}

int do_stune_boost(char *st_name, int boost, int *slot)
{
	struct schedtune *st = getSchedtune(st_name);

	if (!st)
		return -EINVAL;

	return _do_stune_boost(st, boost, slot);
}

#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

#else /* CONFIG_CGROUP_SCHEDTUNE */

int
schedtune_accept_deltas(int nrg_delta, int cap_delta,
			struct task_struct *task)
{
	/* Optimal (O) region */
	if (nrg_delta < 0 && cap_delta > 0) {
		trace_sched_tune_filter(nrg_delta, cap_delta, 0, 0, 1, 0);
		return INT_MAX;
	}

	/* Suboptimal (S) region */
	if (nrg_delta > 0 && cap_delta < 0) {
		trace_sched_tune_filter(nrg_delta, cap_delta, 0, 0, -1, 5);
		return -INT_MAX;
	}

	return __schedtune_accept_deltas(nrg_delta, cap_delta,
			perf_boost_idx, perf_constrain_idx);
}

#endif /* CONFIG_CGROUP_SCHEDTUNE */

int
sysctl_sched_cfs_boost_handler(struct ctl_table *table, int write,
			       void __user *buffer, size_t *lenp,
			       loff_t *ppos)
{
	int ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);
	unsigned threshold_idx;
	int boost_pct;

	if (ret || !write)
		return ret;

	if (sysctl_sched_cfs_boost < -100 || sysctl_sched_cfs_boost > 100)
		return -EINVAL;
	boost_pct = sysctl_sched_cfs_boost;

	/*
	 * Update threshold params for Performance Boost (B)
	 * and Performance Constraint (C) regions.
	 * The current implementatio uses the same cuts for both
	 * B and C regions.
	 */
	threshold_idx = clamp(boost_pct, 0, 99) / 10;
	perf_boost_idx = threshold_idx;
	perf_constrain_idx = threshold_idx;

	return 0;
}

#ifdef CONFIG_SCHED_DEBUG
static void
schedtune_test_nrg(unsigned long delta_pwr)
{
	unsigned long test_delta_pwr;
	unsigned long test_norm_pwr;
	int idx;

	/*
	 * Check normalization constants using some constant system
	 * energy values
	 */
	pr_info("schedtune: verify normalization constants...\n");
	for (idx = 0; idx < 6; ++idx) {
		test_delta_pwr = delta_pwr >> idx;

		/* Normalize on max energy for target platform */
		test_norm_pwr = reciprocal_divide(
					test_delta_pwr << SCHED_CAPACITY_SHIFT,
					schedtune_target_nrg.rdiv);

		pr_info("schedtune: max_pwr/2^%d: %4lu => norm_pwr: %5lu\n",
			idx, test_delta_pwr, test_norm_pwr);
	}
}
#else
#define schedtune_test_nrg(delta_pwr)
#endif

/*
 * Compute the min/max power consumption of a cluster and all its CPUs
 */
static void
schedtune_add_cluster_nrg(
		struct sched_domain *sd,
		struct sched_group *sg,
		struct target_nrg *ste)
{
	struct sched_domain *sd2;
	struct sched_group *sg2;

	struct cpumask *cluster_cpus;
	char str[32];

	unsigned long min_pwr;
	unsigned long max_pwr;
	int cpu;

	/* Get Cluster energy using EM data for the first CPU */
	cluster_cpus = sched_group_cpus(sg);
	snprintf(str, 32, "CLUSTER[%*pbl]",
		 cpumask_pr_args(cluster_cpus));

	min_pwr = sg->sge->idle_states[sg->sge->nr_idle_states - 1].power;
	max_pwr = sg->sge->cap_states[sg->sge->nr_cap_states - 1].power;
	pr_info("schedtune: %-17s min_pwr: %5lu max_pwr: %5lu\n",
		str, min_pwr, max_pwr);

	/*
	 * Keep track of this cluster's energy in the computation of the
	 * overall system energy
	 */
	ste->min_power += min_pwr;
	ste->max_power += max_pwr;

	/* Get CPU energy using EM data for each CPU in the group */
	for_each_cpu(cpu, cluster_cpus) {
		/* Get a SD view for the specific CPU */
		for_each_domain(cpu, sd2) {
			/* Get the CPU group */
			sg2 = sd2->groups;
			min_pwr = sg2->sge->idle_states[sg2->sge->nr_idle_states - 1].power;
			max_pwr = sg2->sge->cap_states[sg2->sge->nr_cap_states - 1].power;

			ste->min_power += min_pwr;
			ste->max_power += max_pwr;

			snprintf(str, 32, "CPU[%d]", cpu);
			pr_info("schedtune: %-17s min_pwr: %5lu max_pwr: %5lu\n",
				str, min_pwr, max_pwr);

			/*
			 * Assume we have EM data only at the CPU and
			 * the upper CLUSTER level
			 */
			BUG_ON(!cpumask_equal(
				sched_group_cpus(sg),
				sched_group_cpus(sd2->parent->groups)
				));
			break;
		}
	}
}

/*
 * Initialize the constants required to compute normalized energy.
 * The values of these constants depends on the EM data for the specific
 * target system and topology.
 * Thus, this function is expected to be called by the code
 * that bind the EM to the topology information.
 */
static int
schedtune_init(void)
{
	struct target_nrg *ste = &schedtune_target_nrg;
	unsigned long delta_pwr = 0;
	struct sched_domain *sd;
	struct sched_group *sg;

	pr_info("schedtune: init normalization constants...\n");
	ste->max_power = 0;
	ste->min_power = 0;

	rcu_read_lock();

	/*
	 * When EAS is in use, we always have a pointer to the highest SD
	 * which provides EM data.
	 */
	sd = rcu_dereference(per_cpu(sd_ea, cpumask_first(cpu_online_mask)));
	if (!sd) {
		pr_info("schedtune: no energy model data\n");
		goto nodata;
	}

	sg = sd->groups;
	do {
		schedtune_add_cluster_nrg(sd, sg, ste);
	} while (sg = sg->next, sg != sd->groups);

	rcu_read_unlock();

	pr_info("schedtune: %-17s min_pwr: %5lu max_pwr: %5lu\n",
		"SYSTEM", ste->min_power, ste->max_power);

	/* Compute normalization constants */
	delta_pwr = ste->max_power - ste->min_power;
	ste->rdiv = reciprocal_value(delta_pwr);
	pr_info("schedtune: using normalization constants mul: %u sh1: %u sh2: %u\n",
		ste->rdiv.m, ste->rdiv.sh1, ste->rdiv.sh2);

	schedtune_test_nrg(delta_pwr);

#ifdef CONFIG_CGROUP_SCHEDTUNE
	schedtune_init_cgroups();
#else
	pr_info("schedtune: configured to support global boosting only\n");
#endif

	schedtune_spc_rdiv = reciprocal_value(100);

	perf_threshold = find_second_max_cap();

	return 0;

nodata:
	pr_warning("schedtune: disabled!\n");
	rcu_read_unlock();
	return -EINVAL;
}
postcore_initcall(schedtune_init);
