// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025-2026 AxionOS
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/cpufreq.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/topology.h>
#include <linux/seq_file.h>
#include <linux/seqlock.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/version.h>
#include <ax_sched_common.h>
#include <trace/hooks/sched.h>

#define AX_FB_MAX_TIDS 16
#define AX_FB_CMD_SIZE 256
#define AX_FB_PROC_DIR "ax_frame_boost"
#define AX_FB_PROC_BOOST "boost"
#define AX_FB_PROC_STATS "stats"
#define AX_FB_LEVEL_LIGHT 1
#define AX_FB_LEVEL_HEAVY 2
#define AX_FB_DEFAULT_LIGHT_UTIL 640
#define AX_FB_DEFAULT_HEAVY_UTIL 896
#define AX_FB_DEFAULT_UTIL_CAP 960
#define AX_FB_DEFAULT_MAX_DURATION_MS 1600
#define AX_FB_SCORE_MAX 255
#define AX_FB_LIGHT_SCORE 72
#define AX_FB_HEAVY_SCORE 160
#define AX_FB_SOURCE_EXPENSIVE_FRAME 2
#define AX_FB_SOURCE_GPU_LOAD_UP 3
#define AX_FB_SOURCE_ADPF_GPU_LOAD_UP 6
#define AX_FB_SOURCE_ANIMATION_RENDER 7
#define AX_FB_DEFAULT_FRAME_SCORE_GAIN 96
#define AX_FB_DEFAULT_GPU_SCORE_GAIN 64
#define AX_FB_FRAME_TIME_MIN_NS 1000000ULL

static unsigned int ax_fb_enabled = 1;
module_param_named(enabled, ax_fb_enabled, uint, 0644);

static unsigned int ax_fb_light_util = AX_FB_DEFAULT_LIGHT_UTIL;
module_param_named(light_util, ax_fb_light_util, uint, 0644);

static unsigned int ax_fb_heavy_util = AX_FB_DEFAULT_HEAVY_UTIL;
module_param_named(heavy_util, ax_fb_heavy_util, uint, 0644);

static unsigned int ax_fb_util_cap = AX_FB_DEFAULT_UTIL_CAP;
module_param_named(util_cap, ax_fb_util_cap, uint, 0644);

static unsigned int ax_fb_max_duration_ms = AX_FB_DEFAULT_MAX_DURATION_MS;
module_param_named(max_duration_ms, ax_fb_max_duration_ms, uint, 0644);

static unsigned int ax_fb_frame_score_gain = AX_FB_DEFAULT_FRAME_SCORE_GAIN;
module_param_named(frame_score_gain, ax_fb_frame_score_gain, uint, 0644);

static unsigned int ax_fb_gpu_score_gain = AX_FB_DEFAULT_GPU_SCORE_GAIN;
module_param_named(gpu_score_gain, ax_fb_gpu_score_gain, uint, 0644);

#if defined(AX_FB_HAS_MAP_UTIL_FREQ)
static unsigned int ax_fb_freq_assist = 1;
module_param_named(freq_assist, ax_fb_freq_assist, uint, 0644);
#endif

static DEFINE_RAW_SPINLOCK(ax_fb_lock);
static seqcount_t ax_fb_state_seq = SEQCNT_ZERO(ax_fb_state_seq);
static struct proc_dir_entry *ax_fb_proc_dir;
static pid_t ax_fb_pid = -1;
static int ax_fb_cpu = AX_SCHED_CPU_NONE;
static int ax_fb_uid = -1;
static int ax_fb_level;
static int ax_fb_source;
static u64 ax_fb_actual_ns;
static u64 ax_fb_target_ns;
static unsigned int ax_fb_score;
static unsigned long ax_fb_expire_jiffies;
static int ax_fb_tid_count;
static pid_t ax_fb_tids[AX_FB_MAX_TIDS];
static atomic64_t ax_fb_updates;
static atomic64_t ax_fb_clears;
static atomic64_t ax_fb_uclamp_assists;
static atomic64_t ax_fb_enqueue_events;
static atomic64_t ax_fb_dequeue_events;
#if defined(AX_FB_HAS_MAP_UTIL_FREQ)
static atomic64_t ax_fb_freq_assists;
#endif

static unsigned int ax_fb_source_score(int source)
{
	switch (source) {
	case AX_FB_SOURCE_GPU_LOAD_UP:
	case AX_FB_SOURCE_ANIMATION_RENDER:
	case AX_FB_SOURCE_ADPF_GPU_LOAD_UP:
		return READ_ONCE(ax_fb_gpu_score_gain);
	case AX_FB_SOURCE_EXPENSIVE_FRAME:
		return READ_ONCE(ax_fb_frame_score_gain) / 2;
	default:
		return 0;
	}
}

static unsigned int ax_fb_duration_score(u64 actual_ns, u64 target_ns)
{
	unsigned int gain = READ_ONCE(ax_fb_frame_score_gain);
	u64 overrun_ns;

	if (!gain || !target_ns || actual_ns <= target_ns ||
	    target_ns < AX_FB_FRAME_TIME_MIN_NS)
		return 0;

	overrun_ns = min(actual_ns - target_ns, target_ns);
	return min_t(unsigned int,
		     div64_u64(overrun_ns * gain, target_ns), gain);
}

static unsigned int ax_fb_score_for(int level, int source,
				    u64 actual_ns, u64 target_ns)
{
	unsigned int score;

	if (level >= AX_FB_LEVEL_HEAVY)
		score = AX_FB_HEAVY_SCORE;
	else
		score = AX_FB_LIGHT_SCORE;

	score += ax_fb_source_score(source);
	score += ax_fb_duration_score(actual_ns, target_ns);
	return min_t(unsigned int, score, AX_FB_SCORE_MAX);
}

static unsigned int ax_fb_resource_mask(int source)
{
	switch (source) {
	case AX_FB_SOURCE_GPU_LOAD_UP:
	case AX_FB_SOURCE_ANIMATION_RENDER:
	case AX_FB_SOURCE_ADPF_GPU_LOAD_UP:
		return AX_BOOST_PMQOS | AX_BOOST_GPU |
			AX_BOOST_MEMLAT | AX_BOOST_DDR |
			AX_BOOST_L3;
	default:
		return AX_BOOST_PMQOS;
	}
}

static unsigned int ax_fb_scaled_util(unsigned int base)
{
	unsigned int cap = READ_ONCE(ax_fb_util_cap);
	unsigned int score = min_t(unsigned int, READ_ONCE(ax_fb_score),
				   AX_FB_SCORE_MAX);

	if (cap < base)
		return cap;

	return min_t(unsigned int,
		     base + score * (cap - base) / AX_FB_SCORE_MAX, cap);
}

static unsigned int ax_fb_active_util(void)
{
	unsigned int level = READ_ONCE(ax_fb_level);

	if (!READ_ONCE(ax_fb_enabled) || level == 0)
		return 0;

	if (time_after_eq(jiffies, READ_ONCE(ax_fb_expire_jiffies)))
		return 0;

	if (level >= AX_FB_LEVEL_HEAVY)
		return ax_sched_clamp_util(
			ax_fb_scaled_util(READ_ONCE(ax_fb_heavy_util)));

	return ax_sched_clamp_util(
		ax_fb_scaled_util(READ_ONCE(ax_fb_light_util)));
}

unsigned int ax_frame_boost_active_util(void)
{
	return ax_fb_active_util();
}
EXPORT_SYMBOL_GPL(ax_frame_boost_active_util);

static bool ax_fb_task_matches(struct task_struct *task)
{
	pid_t pid;
	pid_t tid;
	int count;
	int i;

	if (!task)
		return false;

	pid = READ_ONCE(ax_fb_pid);
	if (pid <= 0)
		return false;

	tid = task_pid_nr(task);
	if (tid == pid)
		return true;

	count = min_t(int, READ_ONCE(ax_fb_tid_count), AX_FB_MAX_TIDS);
	for (i = 0; i < count; i++) {
		if (tid == READ_ONCE(ax_fb_tids[i]))
			return true;
	}

	return task_tgid_nr(task) == pid && ax_sched_task_is_render_helper(task);
}

static unsigned int ax_fb_task_util(struct task_struct *task)
{
	unsigned int util = ax_fb_active_util();

	if (!util || !ax_fb_task_matches(task))
		return 0;

	return util;
}

unsigned int ax_frame_boost_task_util(struct task_struct *task)
{
	return ax_fb_task_util(task);
}
EXPORT_SYMBOL_GPL(ax_frame_boost_task_util);

void ax_frame_boost_note_cpu(struct task_struct *task, int cpu)
{
	unsigned long flags;

	if (!task || cpu < 0 || cpu >= nr_cpu_ids || !ax_fb_task_util(task))
		return;

	raw_spin_lock_irqsave(&ax_fb_lock, flags);
	if (ax_fb_task_util(task)) {
		write_seqcount_begin(&ax_fb_state_seq);
		WRITE_ONCE(ax_fb_cpu, cpu);
		write_seqcount_end(&ax_fb_state_seq);
	}
	raw_spin_unlock_irqrestore(&ax_fb_lock, flags);
}
EXPORT_SYMBOL_GPL(ax_frame_boost_note_cpu);

#if defined(AX_FB_HAS_UCLAMP_EFF_GET) && defined(CONFIG_UCLAMP_TASK)
void ax_frame_boost_uclamp_eff_get(struct task_struct *task,
				   enum uclamp_id clamp_id,
				   struct uclamp_se *uclamp_max,
				   struct uclamp_se *uclamp_eff, int *ret)
{
	unsigned int util;

	if (!ret || !uclamp_eff || !task || clamp_id != UCLAMP_MIN)
		return;

	util = ax_fb_task_util(task);
	if (!util)
		return;

	util = ax_sched_merge_uclamp(uclamp_eff, util);
	util = max_t(unsigned int, util, task->uclamp_req[UCLAMP_MIN].value);
	util = max_t(unsigned int, util, task->uclamp[UCLAMP_MIN].value);
	if (uclamp_max)
		util = min_t(unsigned int, util, uclamp_max->value);

	ax_sched_set_uclamp(uclamp_eff, ax_sched_clamp_util(util));
	atomic64_inc(&ax_fb_uclamp_assists);
	*ret = 1;
}
EXPORT_SYMBOL_GPL(ax_frame_boost_uclamp_eff_get);
#endif

void ax_frame_boost_enqueue_task(struct task_struct *task)
{
	if (!task || !ax_fb_task_util(task))
		return;

#if defined(AX_FB_HAS_MAP_UTIL_FREQ)
	ax_frame_boost_note_cpu(task, task_cpu(task));
#endif
	atomic64_inc(&ax_fb_enqueue_events);
}
EXPORT_SYMBOL_GPL(ax_frame_boost_enqueue_task);

void ax_frame_boost_dequeue_task(struct task_struct *task)
{
	if (!task || !ax_fb_task_matches(task))
		return;

	atomic64_inc(&ax_fb_dequeue_events);
}
EXPORT_SYMBOL_GPL(ax_frame_boost_dequeue_task);

#if defined(AX_FB_HAS_MAP_UTIL_FREQ)
static unsigned long ax_fb_freq_util(struct cpufreq_policy *policy)
{
	unsigned long util;
	unsigned int seq;
	int cpu;

	if (!READ_ONCE(ax_fb_enabled) || !READ_ONCE(ax_fb_freq_assist))
		return 0;

	do {
		seq = read_seqcount_begin(&ax_fb_state_seq);
		util = ax_fb_active_util();
		if (util && policy) {
			cpu = READ_ONCE(ax_fb_cpu);
			if (cpu < 0 || cpu >= nr_cpu_ids ||
			    !cpumask_test_cpu(cpu, policy->related_cpus))
				util = 0;
		}
	} while (read_seqcount_retry(&ax_fb_state_seq, seq));

	return util;
}

void ax_frame_boost_map_util_freq(unsigned long util, unsigned long freq,
				  unsigned long cap,
				  unsigned long *next_freq,
				  struct cpufreq_policy *policy)
{
	unsigned long active_util;
	unsigned long requested;

	if (!next_freq || !freq || !cap)
		return;

	active_util = ax_fb_freq_util(policy);
	if (!active_util)
		return;

	active_util = min_t(unsigned long, max(util, active_util), cap);
	requested = freq * active_util / cap;
	if (requested > *next_freq) {
		*next_freq = requested;
		atomic64_inc(&ax_fb_freq_assists);
	}
}
EXPORT_SYMBOL_GPL(ax_frame_boost_map_util_freq);
#endif

static bool ax_fb_clear_locked(pid_t pid)
{
	if (pid > 0 && ax_fb_pid > 0 && pid != ax_fb_pid)
		return false;

	if (ax_fb_pid > 0)
		atomic64_inc(&ax_fb_clears);
	write_seqcount_begin(&ax_fb_state_seq);
	ax_fb_pid = -1;
	ax_fb_cpu = AX_SCHED_CPU_NONE;
	ax_fb_uid = -1;
	ax_fb_level = 0;
	ax_fb_source = 0;
	ax_fb_actual_ns = 0;
	ax_fb_target_ns = 0;
	ax_fb_score = 0;
	ax_fb_expire_jiffies = 0;
	ax_fb_tid_count = 0;
	memset(ax_fb_tids, 0, sizeof(ax_fb_tids));
	write_seqcount_end(&ax_fb_state_seq);
	return true;
}

static int ax_fb_parse_int(char *text, int *value)
{
	return kstrtoint(strstrip(text), 10, value);
}

static int ax_fb_parse_u64(char *text, u64 *value)
{
	return kstrtoull(strstrip(text), 10, value);
}

static ssize_t ax_fb_boost_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	char buffer[AX_FB_CMD_SIZE];
	char *argv[8 + AX_FB_MAX_TIDS];
	char *cursor;
	char *token;
	unsigned long flags;
	unsigned int duration_ms;
	unsigned long delay;
	u64 actual_ns = 0;
	u64 target_ns = 0;
	int argc = 0;
	int pid;
	int uid;
	int level;
	int parsed;
	int requested_tids = 0;
	int source = 0;
	int metadata_arg;
	int i;

	if (count == 0)
		return 0;

	if (count >= sizeof(buffer))
		count = sizeof(buffer) - 1;

	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	buffer[count] = '\0';
	cursor = buffer;
	while ((token = strsep(&cursor, " \t\n")) != NULL && argc < ARRAY_SIZE(argv)) {
		if (*token)
			argv[argc++] = token;
	}

	if (argc < 4)
		return -EINVAL;

	if (ax_fb_parse_int(argv[0], &pid) || ax_fb_parse_int(argv[1], &uid) ||
	    ax_fb_parse_int(argv[2], &level) || ax_fb_parse_int(argv[3], &parsed))
		return -EINVAL;

	duration_ms = parsed > 0 ? parsed : 0;

	if (argc > 4) {
		if (ax_fb_parse_int(argv[4], &requested_tids))
			return -EINVAL;
		requested_tids = clamp(requested_tids, 0, AX_FB_MAX_TIDS);
	}

	if (level <= 0 || duration_ms == 0 || !READ_ONCE(ax_fb_enabled)) {
		raw_spin_lock_irqsave(&ax_fb_lock, flags);
		ax_fb_clear_locked(pid);
		raw_spin_unlock_irqrestore(&ax_fb_lock, flags);
		ax_sched_boost_clear(AX_BOOST_SOURCE_FRAME);
		return count;
	}

	if (pid <= 0 || uid < 0)
		return -EINVAL;

	if (level > AX_FB_LEVEL_HEAVY)
		level = AX_FB_LEVEL_HEAVY;

	duration_ms = min(duration_ms, READ_ONCE(ax_fb_max_duration_ms));
	delay = msecs_to_jiffies(duration_ms);
	if (!delay)
		delay = 1;

	raw_spin_lock_irqsave(&ax_fb_lock, flags);
	write_seqcount_begin(&ax_fb_state_seq);
	if (ax_fb_pid != pid || !ax_fb_active_util())
		ax_fb_cpu = AX_SCHED_CPU_NONE;
	ax_fb_pid = pid;
	ax_fb_uid = uid;
	ax_fb_level = level;
	ax_fb_expire_jiffies = jiffies + delay;
	ax_fb_tid_count = 0;
	memset(ax_fb_tids, 0, sizeof(ax_fb_tids));
	for (i = 0; i < requested_tids && i + 5 < argc; i++) {
		if (ax_fb_parse_int(argv[i + 5], &parsed) || parsed <= 0)
			continue;
		ax_fb_tids[ax_fb_tid_count++] = parsed;
	}
	metadata_arg = 5 + requested_tids;
	if (argc > metadata_arg)
		ax_fb_parse_int(argv[metadata_arg], &source);
	if (argc > metadata_arg + 1)
		ax_fb_parse_u64(argv[metadata_arg + 1], &actual_ns);
	if (argc > metadata_arg + 2)
		ax_fb_parse_u64(argv[metadata_arg + 2], &target_ns);
	ax_fb_source = source;
	ax_fb_actual_ns = actual_ns;
	ax_fb_target_ns = target_ns;
	ax_fb_score = ax_fb_score_for(level, source, actual_ns, target_ns);
	write_seqcount_end(&ax_fb_state_seq);
	atomic64_inc(&ax_fb_updates);
	raw_spin_unlock_irqrestore(&ax_fb_lock, flags);

	ax_sched_boost(AX_BOOST_SOURCE_FRAME, level,
				ax_fb_resource_mask(source), duration_ms);

	return count;
}

static int ax_fb_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	unsigned long remaining = 0;
	pid_t pid;
	int uid;
	int level;
	int count;
	int i;

	raw_spin_lock_irqsave(&ax_fb_lock, flags);
	if (ax_fb_level && time_before(jiffies, ax_fb_expire_jiffies))
		remaining = jiffies_to_msecs(ax_fb_expire_jiffies - jiffies);
	pid = ax_fb_pid;
	uid = ax_fb_uid;
	level = remaining ? ax_fb_level : 0;
	count = ax_fb_tid_count;
	seq_printf(m, "%d %d %d %lu", pid, uid, level, remaining);
	for (i = 0; i < count; i++)
		seq_printf(m, " %d", ax_fb_tids[i]);
	seq_putc(m, '\n');
	raw_spin_unlock_irqrestore(&ax_fb_lock, flags);
	return 0;
}

static int ax_fb_open(struct inode *inode, struct file *file)
{
	return single_open(file, ax_fb_show, NULL);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops ax_fb_boost_fops = {
	.proc_open = ax_fb_open,
	.proc_read = seq_read,
	.proc_write = ax_fb_boost_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#else
static const struct file_operations ax_fb_boost_fops = {
	.owner = THIS_MODULE,
	.open = ax_fb_open,
	.read = seq_read,
	.write = ax_fb_boost_write,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

static int ax_fb_stats_show(struct seq_file *m, void *v)
{
	unsigned int active_util = ax_fb_active_util();

	seq_printf(m,
		   "active=%u pid=%d uid=%d level=%d source=%d score=%u actual_ns=%llu target_ns=%llu updates=%lld clears=%lld uclamp_assists=%lld enqueue_events=%lld dequeue_events=%lld",
		   active_util ? 1 : 0,
		   READ_ONCE(ax_fb_pid),
		   READ_ONCE(ax_fb_uid),
		   READ_ONCE(ax_fb_level),
		   READ_ONCE(ax_fb_source),
		   READ_ONCE(ax_fb_score),
		   (unsigned long long)READ_ONCE(ax_fb_actual_ns),
		   (unsigned long long)READ_ONCE(ax_fb_target_ns),
		   atomic64_read(&ax_fb_updates),
		   atomic64_read(&ax_fb_clears),
		   atomic64_read(&ax_fb_uclamp_assists),
		   atomic64_read(&ax_fb_enqueue_events),
		   atomic64_read(&ax_fb_dequeue_events));
#if defined(AX_FB_HAS_MAP_UTIL_FREQ)
	seq_printf(m, " freq_assists=%lld", atomic64_read(&ax_fb_freq_assists));
#endif
	seq_putc(m, '\n');
	return 0;
}

static int ax_fb_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, ax_fb_stats_show, NULL);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops ax_fb_stats_fops = {
	.proc_open = ax_fb_stats_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#else
static const struct file_operations ax_fb_stats_fops = {
	.owner = THIS_MODULE,
	.open = ax_fb_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

static int ax_fb_create_proc(void)
{
	struct proc_dir_entry *entry;
	struct proc_dir_entry *stats;

	ax_fb_proc_dir = proc_mkdir(AX_FB_PROC_DIR, NULL);
	if (!ax_fb_proc_dir)
		return -ENOMEM;

	entry = proc_create(AX_FB_PROC_BOOST, 0660, ax_fb_proc_dir,
			    &ax_fb_boost_fops);
	if (!entry) {
		remove_proc_entry(AX_FB_PROC_DIR, NULL);
		ax_fb_proc_dir = NULL;
		return -ENOMEM;
	}

	proc_set_user(entry, KUIDT_INIT(1000), KGIDT_INIT(1000));
	stats = proc_create(AX_FB_PROC_STATS, 0440, ax_fb_proc_dir,
			    &ax_fb_stats_fops);
	if (!stats) {
		remove_proc_entry(AX_FB_PROC_BOOST, ax_fb_proc_dir);
		remove_proc_entry(AX_FB_PROC_DIR, NULL);
		ax_fb_proc_dir = NULL;
		return -ENOMEM;
	}

	proc_set_user(stats, KUIDT_INIT(1000), KGIDT_INIT(1000));
	return 0;
}

static void ax_fb_remove_proc(void)
{
	if (!ax_fb_proc_dir)
		return;

	remove_proc_entry(AX_FB_PROC_STATS, ax_fb_proc_dir);
	remove_proc_entry(AX_FB_PROC_BOOST, ax_fb_proc_dir);
	remove_proc_entry(AX_FB_PROC_DIR, NULL);
	ax_fb_proc_dir = NULL;
}

static int __init ax_frame_boost_init(void)
{
	return ax_fb_create_proc();
}

module_init(ax_frame_boost_init);

static void __exit ax_frame_boost_exit(void)
{
	ax_fb_remove_proc();
}

module_exit(ax_frame_boost_exit);

MODULE_LICENSE("GPL");
