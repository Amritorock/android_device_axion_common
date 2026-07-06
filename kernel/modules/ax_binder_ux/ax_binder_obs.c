/*
 * Copyright 2025-2026 AxionOS
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cgroup.h>
#include <linux/cred.h>
#include <linux/hashtable.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/uidgid.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <trace/hooks/binder.h>
#include <uapi/linux/android/binder.h>

#include "binder_internal.h"

#define OBS_LOOPER_STATE_BACKGROUND 0x40
#define OBS_BG_THREAD_INDEX 2
#define OBS_CHECK_CYCLE_NS 100000000ULL
#define OBS_AUTO_DISABLE_STREAK 3
#define OBS_FIRST_APPLICATION_UID 10000
#define OBS_UID_PER_USER 100000
#define OBS_STALL_TABLE_BITS 7
#define OBS_STALL_TABLE_CAP 256
#define OBS_STALL_DEFAULT_TIMEOUT_MS 8000
#define OBS_STALL_DEFAULT_MIN_THREADS 8
#define OBS_STALL_DEFAULT_KILL_COOLDOWN_MS 30000
#define OBS_STALL_DEFAULT_MIN_OOM_SCORE_ADJ 700

static unsigned int obs_enabled = 1;
module_param_named(obs_enabled, obs_enabled, uint, 0644);

static unsigned int obs_timeout_ms = 800;
module_param_named(obs_timeout_ms, obs_timeout_ms, uint, 0644);

static unsigned int obs_max_pending = 16;
module_param_named(obs_max_pending, obs_max_pending, uint, 0644);

static unsigned int obs_stall_kill_enabled = 1;
module_param_named(obs_stall_kill_enabled, obs_stall_kill_enabled, uint, 0644);

static unsigned int obs_stall_kill_foreground;
module_param_named(obs_stall_kill_foreground, obs_stall_kill_foreground, uint,
		   0644);

static unsigned int obs_stall_timeout_ms = OBS_STALL_DEFAULT_TIMEOUT_MS;
module_param_named(obs_stall_timeout_ms, obs_stall_timeout_ms, uint, 0644);

static unsigned int obs_stall_min_threads = OBS_STALL_DEFAULT_MIN_THREADS;
module_param_named(obs_stall_min_threads, obs_stall_min_threads, uint, 0644);

static unsigned int obs_stall_kill_cooldown_ms =
	OBS_STALL_DEFAULT_KILL_COOLDOWN_MS;
module_param_named(obs_stall_kill_cooldown_ms, obs_stall_kill_cooldown_ms,
		   uint, 0644);

static unsigned int obs_stall_max_pending = OBS_STALL_TABLE_CAP;
module_param_named(obs_stall_max_pending, obs_stall_max_pending, uint, 0644);

static int obs_stall_min_oom_score_adj =
	OBS_STALL_DEFAULT_MIN_OOM_SCORE_ADJ;
module_param_named(obs_stall_min_oom_score_adj,
		   obs_stall_min_oom_score_adj, int, 0644);

struct obs_target {
	struct binder_proc *proc;
	pid_t pid;
	pid_t bg_pid;
	u64 era_ns;
	u64 check_ns;
};

struct obs_stall {
	struct hlist_node node;
	struct list_head release_node;
	struct binder_transaction *txn;
	struct binder_thread *from_thread;
	struct binder_proc *from_proc;
	struct binder_proc *target_proc;
	struct task_struct *target_task;
	pid_t target_pid;
	uid_t target_uid;
	unsigned long start_jiffies;
};

static struct obs_target ob;
static LIST_HEAD(obs_works);
static DEFINE_HASHTABLE(obs_stall_table, OBS_STALL_TABLE_BITS);
static DEFINE_SPINLOCK(obs_stall_lock);
static struct hlist_head *obs_procs;
static struct mutex *obs_procs_lock;
static struct work_struct obs_find_target_work;
static struct delayed_work obs_drain_work;
static struct delayed_work obs_stall_work;
static unsigned int obs_stale_streak;
static unsigned int obs_stall_count;
static unsigned long obs_stall_last_kill;

static u64 obs_timeout_ns(void)
{
	return (u64)READ_ONCE(obs_timeout_ms) * NSEC_PER_MSEC;
}

static unsigned long obs_stall_timeout_jiffies(void)
{
	return max_t(unsigned long,
		     msecs_to_jiffies(max_t(unsigned int,
					    READ_ONCE(obs_stall_timeout_ms),
					    1)),
		     1);
}

static unsigned long obs_stall_cooldown_jiffies(void)
{
	return msecs_to_jiffies(READ_ONCE(obs_stall_kill_cooldown_ms));
}

static uid_t obs_task_uid(struct task_struct *task)
{
	return from_kuid(current_user_ns(), task_uid(task));
}

static bool obs_uid_is_app(uid_t uid)
{
	return uid % OBS_UID_PER_USER >= OBS_FIRST_APPLICATION_UID;
}

static bool obs_target_is(struct binder_proc *proc)
{
	return proc && proc == READ_ONCE(ob.proc);
}

static bool obs_task_in_background(struct task_struct *task)
{
	struct cgroup_subsys_state *css;
	bool background = false;

	rcu_read_lock();
	css = task_css(task, cpu_cgrp_id);
	if (css && css->cgroup && css->cgroup->kn)
		background = !strcmp(css->cgroup->kn->name, "background");
	rcu_read_unlock();

	return background;
}

static bool obs_stall_target_allowed(struct task_struct *task)
{
	if (!task || !obs_uid_is_app(obs_task_uid(task)))
		return false;

	if (!READ_ONCE(obs_stall_kill_foreground) &&
	    !obs_task_in_background(task))
		return false;

	return READ_ONCE(task->signal->oom_score_adj) >=
		READ_ONCE(obs_stall_min_oom_score_adj);
}

static bool obs_work_restricted(struct binder_transaction *t)
{
	if (!t->from || !t->from->proc || !t->from->task)
		return false;

	if (t->from->proc->pid == t->from->pid)
		return false;

	if ((from_kuid(current_user_ns(), t->sender_euid) % OBS_UID_PER_USER)
	    < OBS_FIRST_APPLICATION_UID)
		return false;

	return obs_task_in_background(t->from->task);
}

static void obs_stall_queue_release_locked(struct obs_stall *stall,
					   struct list_head *release)
{
	hash_del(&stall->node);
	obs_stall_count--;
	list_add_tail(&stall->release_node, release);
}

static void obs_stall_release_entries(struct list_head *release)
{
	struct obs_stall *stall;
	struct obs_stall *tmp;

	list_for_each_entry_safe(stall, tmp, release, release_node) {
		list_del(&stall->release_node);
		put_task_struct(stall->target_task);
		kfree(stall);
	}
}

static void obs_stall_clear_all_locked(struct list_head *release)
{
	struct obs_stall *stall;
	struct hlist_node *tmp;
	int bkt;

	hash_for_each_safe(obs_stall_table, bkt, tmp, stall, node)
		obs_stall_queue_release_locked(stall, release);
}

static void obs_stall_clear_proc_locked(struct binder_proc *proc,
					struct list_head *release)
{
	struct obs_stall *stall;
	struct hlist_node *tmp;
	int bkt;

	if (!proc)
		return;

	hash_for_each_safe(obs_stall_table, bkt, tmp, stall, node) {
		if (stall->target_proc == proc || stall->from_proc == proc)
			obs_stall_queue_release_locked(stall, release);
	}
}

static void obs_stall_clear_thread_locked(struct binder_proc *proc,
					  struct binder_thread *thread,
					  struct list_head *release)
{
	struct obs_stall *stall;
	struct hlist_node *tmp;
	int bkt;

	hash_for_each_safe(obs_stall_table, bkt, tmp, stall, node) {
		if (stall->target_proc == proc ||
		    stall->from_proc == proc ||
		    (thread && stall->from_thread == thread))
			obs_stall_queue_release_locked(stall, release);
	}
}

static void obs_stall_clear_txn(struct binder_transaction *t)
{
	LIST_HEAD(release);
	struct obs_stall *stall;
	struct hlist_node *tmp;
	unsigned long flags;
	unsigned long key = (unsigned long)t;

	if (!t || !READ_ONCE(obs_stall_count))
		return;

	spin_lock_irqsave(&obs_stall_lock, flags);
	hash_for_each_possible_safe(obs_stall_table, stall, tmp, node, key) {
		if (stall->txn == t)
			obs_stall_queue_release_locked(stall, &release);
	}
	spin_unlock_irqrestore(&obs_stall_lock, flags);

	obs_stall_release_entries(&release);
}

static void obs_stall_clear_proc(struct binder_proc *proc)
{
	LIST_HEAD(release);
	unsigned long flags;

	if (!proc || !READ_ONCE(obs_stall_count))
		return;

	spin_lock_irqsave(&obs_stall_lock, flags);
	obs_stall_clear_proc_locked(proc, &release);
	spin_unlock_irqrestore(&obs_stall_lock, flags);

	obs_stall_release_entries(&release);
}

static void obs_stall_clear_thread(struct binder_proc *proc,
				   struct binder_thread *thread)
{
	LIST_HEAD(release);
	unsigned long flags;

	if (!READ_ONCE(obs_stall_count))
		return;

	spin_lock_irqsave(&obs_stall_lock, flags);
	obs_stall_clear_thread_locked(proc, thread, &release);
	spin_unlock_irqrestore(&obs_stall_lock, flags);

	obs_stall_release_entries(&release);
}

static bool obs_stall_in_cooldown(unsigned long now, unsigned long cooldown)
{
	return cooldown && obs_stall_last_kill &&
		time_before(now, obs_stall_last_kill + cooldown);
}

static void obs_stall_work_fn(struct work_struct *work)
{
	LIST_HEAD(release);
	struct obs_stall *stall;
	struct obs_stall *victim = NULL;
	struct task_struct *victim_task = NULL;
	unsigned long flags;
	unsigned long now = jiffies;
	unsigned long timeout = obs_stall_timeout_jiffies();
	unsigned long cooldown = obs_stall_cooldown_jiffies();
	unsigned int min_threads = max_t(unsigned int,
					 READ_ONCE(obs_stall_min_threads),
					 1);
	unsigned int active = 0;
	unsigned int age_ms = 0;
	pid_t pid = 0;
	uid_t uid = 0;
	bool schedule_again = false;
	int bkt;

	(void)work;

	spin_lock_irqsave(&obs_stall_lock, flags);
	if (!READ_ONCE(obs_stall_kill_enabled)) {
		obs_stall_clear_all_locked(&release);
		schedule_again = false;
		goto unlock;
	}

	active = obs_stall_count;
	if (active >= min_threads && !obs_stall_in_cooldown(now, cooldown)) {
		hash_for_each(obs_stall_table, bkt, stall, node) {
			if (!time_after_eq(now, stall->start_jiffies + timeout))
				continue;
			if (!victim ||
			    time_before(stall->start_jiffies,
					victim->start_jiffies))
				victim = stall;
		}
	}

	if (victim) {
		victim_task = victim->target_task;
		get_task_struct(victim_task);
		pid = victim->target_pid;
		uid = victim->target_uid;
		age_ms = jiffies_to_msecs(now - victim->start_jiffies);
		obs_stall_last_kill = now;
		obs_stall_clear_proc_locked(victim->target_proc, &release);
	}

	schedule_again = obs_stall_count != 0;

unlock:
	spin_unlock_irqrestore(&obs_stall_lock, flags);

	if (victim_task) {
		if (obs_stall_target_allowed(victim_task)) {
			pr_warn("killing pid %d uid %u after %u ms with %u blocked system_server binder calls\n",
				pid, uid, age_ms, active);
			send_sig(SIGKILL, victim_task, 1);
		}
		put_task_struct(victim_task);
	}

	obs_stall_release_entries(&release);

	if (schedule_again)
		mod_delayed_work(system_wq, &obs_stall_work, timeout + 1);
}

static void obs_stall_track(struct binder_proc *proc,
			    struct binder_transaction *t,
			    bool pending_async, bool sync)
{
	struct obs_stall *stall;
	struct obs_stall *found;
	unsigned long flags;
	unsigned long key = (unsigned long)t;
	unsigned int cap;

	if (!READ_ONCE(obs_stall_kill_enabled) || !sync || pending_async ||
	    !proc || !proc->tsk || !t || !t->from || !t->from->proc ||
	    !t->from->task || !obs_target_is(t->from->proc) ||
	    proc == t->from->proc)
		return;

	if (!obs_stall_target_allowed(proc->tsk))
		return;

	stall = kmalloc(sizeof(*stall), GFP_ATOMIC);
	if (!stall)
		return;

	INIT_LIST_HEAD(&stall->release_node);
	get_task_struct(proc->tsk);
	stall->txn = t;
	stall->from_thread = t->from;
	stall->from_proc = t->from->proc;
	stall->target_proc = proc;
	stall->target_task = proc->tsk;
	stall->target_pid = proc->pid;
	stall->target_uid = obs_task_uid(proc->tsk);
	stall->start_jiffies = jiffies;

	spin_lock_irqsave(&obs_stall_lock, flags);
	hash_for_each_possible(obs_stall_table, found, node, key) {
		if (found->txn == t) {
			spin_unlock_irqrestore(&obs_stall_lock, flags);
			put_task_struct(stall->target_task);
			kfree(stall);
			return;
		}
	}

	cap = READ_ONCE(obs_stall_max_pending);
	if (cap && obs_stall_count >= cap) {
		spin_unlock_irqrestore(&obs_stall_lock, flags);
		put_task_struct(stall->target_task);
		kfree(stall);
		return;
	}

	hash_add(obs_stall_table, &stall->node, key);
	obs_stall_count++;
	spin_unlock_irqrestore(&obs_stall_lock, flags);

	mod_delayed_work(system_wq, &obs_stall_work,
			 obs_stall_timeout_jiffies() + 1);
}

static void obs_flush_works_ilocked(struct binder_proc *proc)
{
	struct binder_work *w;
	struct binder_work *tmp;
	struct binder_thread *thread;

	list_for_each_entry_safe(w, tmp, &obs_works, entry) {
		list_del_init(&w->entry);
		list_add_tail(&w->entry, &proc->todo);
	}
	ob.era_ns = 0;

	thread = list_first_entry_or_null(&proc->waiting_threads,
					  struct binder_thread,
					  waiting_thread_node);
	if (thread) {
		list_del_init(&thread->waiting_thread_node);
		wake_up_interruptible(&thread->wait);
	}
}

static void obs_note_stale_flush(void)
{
	if (++obs_stale_streak < OBS_AUTO_DISABLE_STREAK)
		return;

	WRITE_ONCE(obs_enabled, 0);
	pr_warn("bg thread not draining, auto-disabled\n");
}

static void obs_check_timeout_ilocked(struct binder_proc *proc)
{
	u64 now = sched_clock();

	if (list_empty(&obs_works))
		return;

	if (now - ob.check_ns < OBS_CHECK_CYCLE_NS)
		return;

	ob.check_ns = now;
	if (now - ob.era_ns < obs_timeout_ns())
		return;

	obs_flush_works_ilocked(proc);
	obs_note_stale_flush();
}

static void obs_drain_fn(struct work_struct *work)
{
	struct binder_proc *proc = READ_ONCE(ob.proc);
	u64 now;
	u64 age;

	(void)work;

	if (!proc)
		return;

	spin_lock(&proc->inner_lock);
	if (list_empty(&obs_works)) {
		obs_stale_streak = 0;
		spin_unlock(&proc->inner_lock);
		return;
	}

	now = sched_clock();
	age = now - ob.era_ns;
	if (age >= obs_timeout_ns()) {
		obs_flush_works_ilocked(proc);
		obs_note_stale_flush();
	} else {
		schedule_delayed_work(&obs_drain_work,
				      nsecs_to_jiffies(obs_timeout_ns() - age)
				      + 1);
	}
	spin_unlock(&proc->inner_lock);
}

static bool obs_pending_full_ilocked(void)
{
	struct list_head *pos;
	unsigned int count = 0;
	unsigned int cap = READ_ONCE(obs_max_pending);

	if (!cap)
		return false;

	list_for_each(pos, &obs_works) {
		if (++count >= cap)
			return true;
	}

	return false;
}

static void obs_wake_bg_thread_ilocked(struct binder_proc *proc)
{
	struct binder_thread *thread;

	list_for_each_entry(thread, &proc->waiting_threads,
			    waiting_thread_node) {
		if (thread->looper & OBS_LOOPER_STATE_BACKGROUND) {
			list_del_init(&thread->waiting_thread_node);
			wake_up_interruptible(&thread->wait);
			return;
		}
	}
}

static struct binder_thread *obs_select_thread_ilocked(struct binder_proc *proc,
						       struct binder_transaction *t)
{
	struct binder_thread *thread;

	if (READ_ONCE(ob.bg_pid) && obs_work_restricted(t)) {
		list_for_each_entry(thread, &proc->waiting_threads,
				    waiting_thread_node) {
			if (thread->looper & OBS_LOOPER_STATE_BACKGROUND) {
				list_del_init(&thread->waiting_thread_node);
				return thread;
			}
		}
		return NULL;
	}

	thread = list_first_entry_or_null(&proc->waiting_threads,
					  struct binder_thread,
					  waiting_thread_node);
	if (thread)
		list_del_init(&thread->waiting_thread_node);

	return thread;
}

static void obs_txn_entry(void *data, struct binder_proc *proc,
			  struct binder_transaction *t,
			  struct binder_thread **thread, int node_debug_id,
			  bool pending_async, bool sync, bool *skip)
{
	(void)data;
	(void)node_debug_id;
	(void)sync;

	if (!READ_ONCE(obs_enabled) || !obs_target_is(proc) || !t || !thread
	    || !skip)
		return;

	obs_check_timeout_ilocked(proc);

	if (!pending_async && !*thread) {
		*thread = obs_select_thread_ilocked(proc, t);
		*skip = true;
	}
}

static void obs_txn_finish(void *data, struct binder_proc *proc,
			   struct binder_transaction *t,
			   struct task_struct *binder_th_task,
			   bool pending_async, bool sync)
{
	(void)data;
	(void)sync;

	obs_stall_track(proc, t, pending_async, sync);

	if (!READ_ONCE(obs_enabled) || !obs_target_is(proc) || !t)
		return;

	if (binder_th_task || pending_async)
		return;

	if (!READ_ONCE(ob.bg_pid) || !obs_work_restricted(t))
		return;

	if (obs_pending_full_ilocked())
		return;

	list_del_init(&t->work.entry);
	if (list_empty(&obs_works)) {
		ob.era_ns = sched_clock();
		schedule_delayed_work(&obs_drain_work,
				      msecs_to_jiffies(READ_ONCE(obs_timeout_ms))
				      + 1);
	}
	list_add_tail(&t->work.entry, &obs_works);
}

static void obs_select_worklist(void *data, struct list_head **list,
				struct binder_thread *thread,
				struct binder_proc *proc,
				int wait_for_proc_work)
{
	(void)data;

	if (!READ_ONCE(obs_enabled) || !obs_target_is(proc) || !thread || !list
	    || *list)
		return;

	if (!wait_for_proc_work
	    || !(thread->looper & OBS_LOOPER_STATE_BACKGROUND))
		return;

	if (!list_empty(&thread->todo))
		return;

	if (!list_empty(&obs_works))
		*list = &obs_works;
}

static void obs_has_work(void *data, struct binder_thread *thread,
			 bool do_proc_work, int *ret)
{
	(void)data;

	if (!READ_ONCE(obs_enabled) || !do_proc_work || !thread || !ret)
		return;

	if (!obs_target_is(thread->proc))
		return;

	if (!(thread->looper & OBS_LOOPER_STATE_BACKGROUND))
		return;

	if (!list_empty(&obs_works))
		*ret = 1;
}

static void obs_read_done(void *data, struct binder_proc *proc,
			  struct binder_thread *thread)
{
	(void)data;
	(void)thread;

	if (!obs_target_is(proc))
		return;

	spin_lock(&proc->inner_lock);
	if (!READ_ONCE(obs_enabled)) {
		if (!list_empty(&obs_works))
			obs_flush_works_ilocked(proc);
	} else if (!list_empty(&obs_works)) {
		obs_wake_bg_thread_ilocked(proc);
	}
	spin_unlock(&proc->inner_lock);
}

static void obs_restore_priority(void *data, struct binder_transaction *t,
				 struct task_struct *task)
{
	(void)data;
	(void)task;

	obs_stall_clear_txn(t);

	if (!READ_ONCE(obs_enabled) || !t || !t->to_proc
	    || !obs_target_is(t->to_proc))
		return;

	spin_lock(&t->to_proc->inner_lock);
	obs_check_timeout_ilocked(t->to_proc);
	spin_unlock(&t->to_proc->inner_lock);
}

static void obs_looper_registered(void *data, struct binder_thread *thread,
				  struct binder_proc *proc)
{
	(void)data;

	if (!obs_target_is(proc) || !thread || !thread->task)
		return;

	if (READ_ONCE(ob.bg_pid))
		return;

	if (proc->requested_threads_started == OBS_BG_THREAD_INDEX) {
		thread->looper |= OBS_LOOPER_STATE_BACKGROUND;
		WRITE_ONCE(ob.bg_pid, task_pid_nr(thread->task));
	}
}

static void obs_thread_release(void *data, struct binder_proc *proc,
			       struct binder_thread *thread)
{
	(void)data;

	obs_stall_clear_thread(proc, thread);

	if (!obs_target_is(proc) || !thread)
		return;

	if (!(thread->looper & OBS_LOOPER_STATE_BACKGROUND))
		return;

	spin_lock(&proc->inner_lock);
	if (!list_empty(&obs_works))
		obs_flush_works_ilocked(proc);
	spin_unlock(&proc->inner_lock);
	WRITE_ONCE(ob.bg_pid, 0);
}

static void obs_free_proc(void *data, struct binder_proc *proc)
{
	(void)data;

	obs_stall_clear_proc(proc);

	if (!obs_target_is(proc))
		return;

	cancel_delayed_work_sync(&obs_drain_work);
	WARN_ON(!list_empty(&obs_works));
	WRITE_ONCE(ob.bg_pid, 0);
	ob.era_ns = 0;
	ob.check_ns = 0;
	ob.pid = 0;
	WRITE_ONCE(ob.proc, NULL);
}

static void obs_try_target(struct binder_proc *proc)
{
	if (!proc->tsk || !proc->context || !proc->context->name)
		return;

	if (strncmp(proc->tsk->comm, "system_server", TASK_COMM_LEN))
		return;

	if (strcmp(proc->context->name, "binder"))
		return;

	ob.pid = proc->pid;
	ob.era_ns = 0;
	ob.check_ns = 0;
	WRITE_ONCE(ob.bg_pid, 0);
	WRITE_ONCE(ob.proc, proc);
}

static void obs_find_target_fn(struct work_struct *work)
{
	struct binder_proc *proc;

	(void)work;

	if (!obs_procs || !obs_procs_lock)
		return;

	mutex_lock(obs_procs_lock);
	hlist_for_each_entry(proc, obs_procs, proc_node) {
		if (READ_ONCE(ob.proc))
			break;
		obs_try_target(proc);
	}
	mutex_unlock(obs_procs_lock);
}

static void obs_preset(void *data, struct hlist_head *hhead, struct mutex *lock)
{
	(void)data;

	if (!obs_procs)
		WRITE_ONCE(obs_procs, hhead);
	if (!obs_procs_lock)
		WRITE_ONCE(obs_procs_lock, lock);

	if (READ_ONCE(ob.proc))
		return;

	schedule_work(&obs_find_target_work);
}

static int __init ax_binder_obs_init(void)
{
	int ret;

	INIT_WORK(&obs_find_target_work, obs_find_target_fn);
	INIT_DELAYED_WORK(&obs_drain_work, obs_drain_fn);
	INIT_DELAYED_WORK(&obs_stall_work, obs_stall_work_fn);

	ret = register_trace_android_vh_binder_preset(obs_preset, NULL);
	if (ret)
		return ret;

	ret = register_trace_android_vh_binder_looper_state_registered(
			obs_looper_registered, NULL);
	if (ret)
		goto unregister_preset;

	ret = register_trace_android_vh_binder_proc_transaction_entry(
			obs_txn_entry, NULL);
	if (ret)
		goto unregister_looper_registered;

	ret = register_trace_android_vh_binder_proc_transaction_finish(
			obs_txn_finish, NULL);
	if (ret)
		goto unregister_txn_entry;

	ret = register_trace_android_vh_binder_select_worklist_ilocked(
			obs_select_worklist, NULL);
	if (ret)
		goto unregister_txn_finish;

	ret = register_trace_android_vh_binder_has_work_ilocked(obs_has_work,
								NULL);
	if (ret)
		goto unregister_select_worklist;

	ret = register_trace_android_vh_binder_read_done(obs_read_done, NULL);
	if (ret)
		goto unregister_has_work;

	ret = register_trace_android_vh_binder_restore_priority(
			obs_restore_priority, NULL);
	if (ret)
		goto unregister_read_done;

	ret = register_trace_android_vh_binder_thread_release(
			obs_thread_release, NULL);
	if (ret)
		goto unregister_restore_priority;

	ret = register_trace_android_vh_binder_free_proc(obs_free_proc, NULL);
	if (ret)
		goto unregister_thread_release;

	return 0;

unregister_thread_release:
	unregister_trace_android_vh_binder_thread_release(obs_thread_release,
							  NULL);
unregister_restore_priority:
	unregister_trace_android_vh_binder_restore_priority(
			obs_restore_priority, NULL);
unregister_read_done:
	unregister_trace_android_vh_binder_read_done(obs_read_done, NULL);
unregister_has_work:
	unregister_trace_android_vh_binder_has_work_ilocked(obs_has_work, NULL);
unregister_select_worklist:
	unregister_trace_android_vh_binder_select_worklist_ilocked(
			obs_select_worklist, NULL);
unregister_txn_finish:
	unregister_trace_android_vh_binder_proc_transaction_finish(
			obs_txn_finish, NULL);
unregister_txn_entry:
	unregister_trace_android_vh_binder_proc_transaction_entry(obs_txn_entry,
								  NULL);
unregister_looper_registered:
	unregister_trace_android_vh_binder_looper_state_registered(
			obs_looper_registered, NULL);
unregister_preset:
	unregister_trace_android_vh_binder_preset(obs_preset, NULL);
	return ret;
}

static void __exit ax_binder_obs_exit(void)
{
	struct binder_proc *proc;

	unregister_trace_android_vh_binder_free_proc(obs_free_proc, NULL);
	unregister_trace_android_vh_binder_thread_release(obs_thread_release,
							  NULL);
	unregister_trace_android_vh_binder_restore_priority(
			obs_restore_priority, NULL);
	unregister_trace_android_vh_binder_read_done(obs_read_done, NULL);
	unregister_trace_android_vh_binder_has_work_ilocked(obs_has_work, NULL);
	unregister_trace_android_vh_binder_select_worklist_ilocked(
			obs_select_worklist, NULL);
	unregister_trace_android_vh_binder_proc_transaction_finish(
			obs_txn_finish, NULL);
	unregister_trace_android_vh_binder_proc_transaction_entry(obs_txn_entry,
								  NULL);
	unregister_trace_android_vh_binder_looper_state_registered(
			obs_looper_registered, NULL);
	unregister_trace_android_vh_binder_preset(obs_preset, NULL);
	tracepoint_synchronize_unregister();
	cancel_work_sync(&obs_find_target_work);
	cancel_delayed_work_sync(&obs_drain_work);
	cancel_delayed_work_sync(&obs_stall_work);

	{
		LIST_HEAD(release);
		unsigned long flags;

		spin_lock_irqsave(&obs_stall_lock, flags);
		obs_stall_clear_all_locked(&release);
		spin_unlock_irqrestore(&obs_stall_lock, flags);
		obs_stall_release_entries(&release);
	}

	proc = READ_ONCE(ob.proc);
	if (proc) {
		spin_lock(&proc->inner_lock);
		if (!list_empty(&obs_works))
			obs_flush_works_ilocked(proc);
		spin_unlock(&proc->inner_lock);
	}
}

module_init(ax_binder_obs_init);
module_exit(ax_binder_obs_exit);

MODULE_LICENSE("GPL");
