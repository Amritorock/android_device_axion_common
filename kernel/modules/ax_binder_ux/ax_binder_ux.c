// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025-2026 AxionOS
 */

#include <linux/bug.h>
#include <linux/hashtable.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/tracepoint.h>
#include <linux/workqueue.h>
#include <ax_sched_common.h>
#include <trace/hooks/binder.h>
#include <uapi/linux/android/binder.h>
#include <uapi/linux/sched/types.h>

#include "binder_internal.h"

#define TF_AX_UX_BOOST 0x80
#define AX_BOOST_TABLE_BITS 6
#define AX_BOOST_TABLE_CAP 512
#define AX_BINDER_DEFAULT_SYNC_BOOST_TIMEOUT_MS 128

static unsigned int ax_binder_ux_enabled = 1;
module_param_named(ux_enabled, ax_binder_ux_enabled, uint, 0644);

static unsigned int ax_binder_sync_boost_util = 512;
module_param_named(sync_boost_util, ax_binder_sync_boost_util, uint, 0644);

static unsigned int ax_binder_sync_boost_timeout_ms =
	AX_BINDER_DEFAULT_SYNC_BOOST_TIMEOUT_MS;
module_param_named(sync_boost_timeout_ms, ax_binder_sync_boost_timeout_ms,
		   uint, 0644);

#ifdef AX_BINDER_HAS_BURST_SCENE
static unsigned int ax_binder_scene_boost = 1;
module_param_named(scene_boost, ax_binder_scene_boost, uint, 0644);
#endif

#ifdef AX_BINDER_HAS_FRAME_BOOST
static unsigned int ax_binder_frame_boost = 1;
module_param_named(frame_boost, ax_binder_frame_boost, uint, 0644);
#endif

struct ax_boost_entry {
	struct hlist_node node;
	pid_t tid;
	struct task_struct *task;
	unsigned long expire_jiffies;
	unsigned int util_min;
};

struct ax_boost_req {
	struct list_head node;
	struct task_struct *task;
	unsigned int util_min;
};

static DEFINE_HASHTABLE(ax_boost_table, AX_BOOST_TABLE_BITS);
static DEFINE_SPINLOCK(ax_boost_lock);
static LIST_HEAD(ax_boost_reqs);
static int ax_boost_count;
static struct work_struct ax_boost_work;
static struct delayed_work ax_boost_expire_work;

static unsigned int ax_binder_boost_util(struct binder_transaction *t)
{
	unsigned int util;

	if (!t || !READ_ONCE(ax_binder_ux_enabled))
		return 0;

	util = (t->flags & TF_AX_UX_BOOST) ?
		READ_ONCE(ax_binder_sync_boost_util) : 0;
#ifdef AX_BINDER_HAS_BURST_SCENE
	if (READ_ONCE(ax_binder_scene_boost) && t->from && t->from->task) {
		util = max_t(unsigned int, util,
			     ax_burst_sched_binder_util(t->from->task));
	}
#endif
#ifdef AX_BINDER_HAS_FRAME_BOOST
	if (READ_ONCE(ax_binder_frame_boost) && t->from && t->from->task) {
		util = max_t(unsigned int, util,
			     ax_frame_boost_task_util(t->from->task));
	}
#endif

	return ax_sched_clamp_util(util);
}

static bool ax_binder_work_has_ux(struct binder_work *work)
{
	struct binder_transaction *t;

	if (work->type != BINDER_WORK_TRANSACTION)
		return false;

	t = container_of(work, struct binder_transaction, work);
	return ax_binder_boost_util(t) != 0;
}

static void ax_binder_enqueue_work(struct binder_work *work,
				   struct list_head *target_list)
{
	struct list_head *pos;

	BUG_ON(target_list == NULL);
	BUG_ON(work->entry.next && !list_empty(&work->entry));

	pos = target_list->next;
	while (pos != target_list &&
	       ax_binder_work_has_ux(list_entry(pos, struct binder_work, entry)))
		pos = pos->next;

	list_add_tail(&work->entry, pos);
}

static void ax_binder_special_task(void *data, struct binder_transaction *t,
				   struct binder_proc *proc,
				   struct binder_thread *thread,
				   struct binder_work *work,
				   struct list_head *head, bool sync,
				   bool *special_task)
{
	(void)data;
	(void)thread;

	if (!special_task || !*special_task || !t || !proc || !work || !head)
		return;

	if (sync || head == &proc->todo || work != &t->work)
		return;

	if (!ax_binder_boost_util(t))
		return;

	ax_binder_enqueue_work(work, head);
	*special_task = false;
}

static void ax_boost_apply(struct task_struct *task, unsigned int util_min)
{
	struct sched_attr attr = {
		.sched_flags = SCHED_FLAG_KEEP_ALL | SCHED_FLAG_UTIL_CLAMP_MIN,
		.sched_util_min = util_min,
	};

	sched_setattr_nocheck(task, &attr);
}

static unsigned long ax_boost_timeout_jiffies(void)
{
	return max_t(unsigned long,
		     msecs_to_jiffies(max_t(unsigned int,
					    READ_ONCE(ax_binder_sync_boost_timeout_ms),
					    1)),
		     1);
}

static unsigned long ax_boost_expire_jiffies(void)
{
	return jiffies + ax_boost_timeout_jiffies();
}

static unsigned long ax_boost_min_delay(unsigned long delay, unsigned long next)
{
	if (!next)
		return delay;
	if (!delay || next < delay)
		return next;
	return delay;
}

static void ax_boost_work_fn(struct work_struct *work)
{
	struct ax_boost_req *req;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&ax_boost_lock, flags);
		req = list_first_entry_or_null(&ax_boost_reqs,
					       struct ax_boost_req, node);
		if (req)
			list_del(&req->node);
		spin_unlock_irqrestore(&ax_boost_lock, flags);
		if (!req)
			break;
		ax_boost_apply(req->task, req->util_min);
		put_task_struct(req->task);
		kfree(req);
	}
}

static void ax_boost_schedule_expire(unsigned long delay)
{
	mod_delayed_work(system_wq, &ax_boost_expire_work,
			 max_t(unsigned long, delay, 1));
}

static void ax_boost_request_locked(struct task_struct *task,
				    unsigned int util_min)
{
	struct ax_boost_req *req;

	req = kmalloc(sizeof(*req), GFP_ATOMIC);
	if (!req) {
		put_task_struct(task);
		return;
	}

	req->task = task;
	req->util_min = util_min;
	list_add_tail(&req->node, &ax_boost_reqs);
}

static unsigned long ax_boost_expire_locked(bool *expired)
{
	struct ax_boost_entry *entry;
	struct hlist_node *tmp;
	unsigned long delay = 0;
	unsigned long now = jiffies;
	int bkt;

	hash_for_each_safe(ax_boost_table, bkt, tmp, entry, node) {
		unsigned long expire = READ_ONCE(entry->expire_jiffies);

		if (!expire || time_after_eq(now, expire)) {
			hash_del(&entry->node);
			ax_boost_count--;
			ax_boost_request_locked(entry->task, 0);
			kfree(entry);
			*expired = true;
			continue;
		}

		delay = ax_boost_min_delay(delay, expire - now);
	}

	return delay;
}

static void ax_boost_expire_work_fn(struct work_struct *work)
{
	unsigned long flags;
	unsigned long delay;
	bool expired = false;

	spin_lock_irqsave(&ax_boost_lock, flags);
	delay = ax_boost_expire_locked(&expired);
	spin_unlock_irqrestore(&ax_boost_lock, flags);

	if (expired)
		schedule_work(&ax_boost_work);
	if (delay)
		ax_boost_schedule_expire(delay);
}

static void ax_boost_table_clear_locked(void)
{
	struct ax_boost_entry *entry;
	struct hlist_node *tmp;
	int bkt;

	hash_for_each_safe(ax_boost_table, bkt, tmp, entry, node) {
		hash_del(&entry->node);
		ax_boost_request_locked(entry->task, 0);
		kfree(entry);
	}
	ax_boost_count = 0;
}

static bool ax_boost_track(struct task_struct *task, unsigned int util_min)
{
	struct ax_boost_entry *entry;
	struct ax_boost_entry *found;
	unsigned long flags;
	unsigned long expire = ax_boost_expire_jiffies();
	pid_t tid = task_pid_nr(task);
	bool tracked = false;

	entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return false;

	spin_lock_irqsave(&ax_boost_lock, flags);
	hash_for_each_possible(ax_boost_table, found, node, tid) {
		if (found->tid == tid && found->task == task) {
			WRITE_ONCE(found->expire_jiffies, expire);
			if (READ_ONCE(found->util_min) != util_min) {
				WRITE_ONCE(found->util_min, util_min);
				get_task_struct(task);
				ax_boost_request_locked(task, util_min);
				tracked = true;
			}
			spin_unlock_irqrestore(&ax_boost_lock, flags);
			kfree(entry);
			ax_boost_schedule_expire(ax_boost_timeout_jiffies());
			return tracked;
		}
	}
	if (ax_boost_count >= AX_BOOST_TABLE_CAP)
		ax_boost_table_clear_locked();
	get_task_struct(task);
	entry->tid = tid;
	entry->task = task;
	entry->expire_jiffies = expire;
	entry->util_min = util_min;
	hash_add(ax_boost_table, &entry->node, tid);
	ax_boost_count++;
	get_task_struct(task);
	ax_boost_request_locked(task, util_min);
	tracked = true;
	spin_unlock_irqrestore(&ax_boost_lock, flags);

	ax_boost_schedule_expire(ax_boost_timeout_jiffies());
	return tracked;
}

static bool ax_boost_untrack(struct task_struct *task)
{
	struct ax_boost_entry *entry;
	struct hlist_node *tmp;
	unsigned long flags;
	pid_t tid = task_pid_nr(task);
	bool removed = false;

	spin_lock_irqsave(&ax_boost_lock, flags);
	hash_for_each_possible_safe(ax_boost_table, entry, tmp, node, tid) {
		if (entry->tid == tid && entry->task == task) {
			hash_del(&entry->node);
			ax_boost_count--;
			ax_boost_request_locked(entry->task, 0);
			kfree(entry);
			removed = true;
			break;
		}
	}
	spin_unlock_irqrestore(&ax_boost_lock, flags);
	return removed;
}

static void ax_binder_txn_finish(void *data, struct binder_proc *proc,
				 struct binder_transaction *t,
				 struct task_struct *binder_th_task,
				 bool pending_async, bool sync)
{
	unsigned int util;

	(void)data;
	(void)proc;
	(void)pending_async;

	if (!sync || !t || !binder_th_task)
		return;

	util = ax_binder_boost_util(t);
	if (!util)
		return;

#ifdef AX_BINDER_HAS_BURST_SCENE
	if (t->from && t->from->task)
		ax_burst_sched_note_binder_assist(t->from->task, util);
#endif

	if (ax_boost_track(binder_th_task, util))
		schedule_work(&ax_boost_work);
}

static void ax_binder_restore_priority(void *data, struct binder_transaction *t,
				       struct task_struct *task)
{
	(void)data;
	(void)t;

	if (!task || !READ_ONCE(ax_boost_count))
		return;

	if (ax_boost_untrack(task))
		schedule_work(&ax_boost_work);
}

static void ax_binder_thread_idle(void *data, bool do_proc_work,
				  struct binder_thread *thread,
				  struct binder_proc *proc)
{
	(void)data;
	(void)proc;

	if (!do_proc_work || !thread || !thread->task
	    || !READ_ONCE(ax_boost_count))
		return;

	if (ax_boost_untrack(thread->task))
		schedule_work(&ax_boost_work);
}

static void ax_binder_thread_release(void *data, struct binder_proc *proc,
				     struct binder_thread *thread)
{
	(void)data;
	(void)proc;

	if (!thread || !thread->task || !READ_ONCE(ax_boost_count))
		return;

	if (ax_boost_untrack(thread->task))
		schedule_work(&ax_boost_work);
}

static int __init ax_binder_ux_init(void)
{
	int ret;

	INIT_WORK(&ax_boost_work, ax_boost_work_fn);
	INIT_DELAYED_WORK(&ax_boost_expire_work, ax_boost_expire_work_fn);

	ret = register_trace_android_vh_binder_special_task(ax_binder_special_task,
							    NULL);
	if (ret)
		return ret;

	ret = register_trace_android_vh_binder_proc_transaction_finish(
			ax_binder_txn_finish, NULL);
	if (ret)
		goto unregister_special_task;

	ret = register_trace_android_vh_binder_restore_priority(
			ax_binder_restore_priority, NULL);
	if (ret)
		goto unregister_txn_finish;

	ret = register_trace_android_vh_binder_wait_for_work(
			ax_binder_thread_idle, NULL);
	if (ret)
		goto unregister_restore_priority;

	ret = register_trace_android_vh_binder_thread_release(
			ax_binder_thread_release, NULL);
	if (ret)
		goto unregister_wait_for_work;

	return 0;

unregister_wait_for_work:
	unregister_trace_android_vh_binder_wait_for_work(ax_binder_thread_idle,
							 NULL);
unregister_restore_priority:
	unregister_trace_android_vh_binder_restore_priority(
			ax_binder_restore_priority, NULL);
unregister_txn_finish:
	unregister_trace_android_vh_binder_proc_transaction_finish(
			ax_binder_txn_finish, NULL);
unregister_special_task:
	unregister_trace_android_vh_binder_special_task(ax_binder_special_task,
							NULL);
	return ret;
}

static void __exit ax_binder_ux_exit(void)
{
	unsigned long flags;

	unregister_trace_android_vh_binder_thread_release(
			ax_binder_thread_release, NULL);
	unregister_trace_android_vh_binder_wait_for_work(ax_binder_thread_idle,
							 NULL);
	unregister_trace_android_vh_binder_restore_priority(
			ax_binder_restore_priority, NULL);
	unregister_trace_android_vh_binder_proc_transaction_finish(
			ax_binder_txn_finish, NULL);
	unregister_trace_android_vh_binder_special_task(ax_binder_special_task,
							NULL);
	tracepoint_synchronize_unregister();
	cancel_delayed_work_sync(&ax_boost_expire_work);

	spin_lock_irqsave(&ax_boost_lock, flags);
	ax_boost_table_clear_locked();
	spin_unlock_irqrestore(&ax_boost_lock, flags);
	schedule_work(&ax_boost_work);
	flush_work(&ax_boost_work);
}

module_init(ax_binder_ux_init);
module_exit(ax_binder_ux_exit);

MODULE_LICENSE("GPL");
