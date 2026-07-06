// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025-2026 AxionOS
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_qos.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <ax_sched_common.h>

#define ATCM_CMD_SIZE 192
#define ATCM_PROC_DIR "ax_atcm"
#define ATCM_PROC_STATE "state"
#define ATCM_PROC_STATS "stats"
#define ATCM_CLUSTERS 3
#define ATCM_FIELDS 7
#define ATCM_LEVEL_MODERATE 13
#define ATCM_LEVEL_CRITICAL 14

struct cpu_limit {
	unsigned int cpu;
	struct freq_qos_request min_req;
	bool min_added;
};

static DEFINE_MUTEX(lock);
static void apply_state_locked(void);
static bool initialized;

static int set_uint_param(const char *val, const struct kernel_param *kp)
{
	int ret;

	mutex_lock(&lock);
	ret = param_set_uint(val, kp);
	if (!ret && READ_ONCE(initialized))
		apply_state_locked();
	mutex_unlock(&lock);

	return ret;
}

static const struct kernel_param_ops uint_param_ops = {
	.set = set_uint_param,
	.get = param_get_uint,
};

static unsigned int enabled = 1;
module_param_cb(enabled, &uint_param_ops, &enabled, 0644);

static unsigned int cpu_enabled = 1;
module_param_cb(cpu_enabled, &uint_param_ops, &cpu_enabled, 0644);

static unsigned int resource_enabled = 1;
module_param_cb(resource_enabled, &uint_param_ops, &resource_enabled, 0644);

static unsigned int resource_clamp_level = ATCM_LEVEL_MODERATE;
module_param_cb(resource_clamp_level, &uint_param_ops, &resource_clamp_level, 0644);

static unsigned int resource_block_level = ATCM_LEVEL_CRITICAL;
module_param_cb(resource_block_level, &uint_param_ops, &resource_block_level, 0644);

static struct proc_dir_entry *proc_dir;
static struct cpu_limit cpu_limits[ATCM_CLUSTERS];
static unsigned int cpu_limit_count;
static int thermal_level;
static int cpu_cap = -1;
static int gpu_cap = -1;
static int boost_cap = -1;
static int cpu_min[ATCM_CLUSTERS];
static unsigned int pmqos_ceiling = AX_BOOST_LEVEL_HEAVY;
static unsigned int perf_ceiling = AX_BOOST_LEVEL_HEAVY;
static atomic64_t state_writes;
static atomic64_t cpu_updates;
static atomic64_t cpu_errors;
static atomic64_t resource_updates;

static bool policy_seen(struct cpufreq_policy *policy)
{
	unsigned int i;

	for (i = 0; i < cpu_limit_count; i++) {
		if (cpumask_test_cpu(cpu_limits[i].cpu, policy->related_cpus))
			return true;
	}
	return false;
}

static int add_cpu_limit(struct cpufreq_policy *policy)
{
	struct cpu_limit *limit;
	int ret;

	if (cpu_limit_count >= ATCM_CLUSTERS)
		return 0;

	limit = &cpu_limits[cpu_limit_count];
	limit->cpu = policy->cpu;
	ret = freq_qos_add_request(&policy->constraints, &limit->min_req,
				   FREQ_QOS_MIN, FREQ_QOS_MIN_DEFAULT_VALUE);
	if (ret)
		return ret;
	limit->min_added = true;

	cpu_limit_count++;
	return 0;
}

static void init_cpu_limits_locked(void)
{
	unsigned int cpu;

	if (cpu_limit_count)
		return;

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *policy;
		int ret;

		if (cpu_limit_count >= ATCM_CLUSTERS)
			break;

		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		if (policy_seen(policy)) {
			cpufreq_cpu_put(policy);
			continue;
		}
		ret = add_cpu_limit(policy);
		cpufreq_cpu_put(policy);
		if (ret < 0)
			atomic64_inc(&cpu_errors);
	}
}

static int update_req(struct freq_qos_request *req, bool added, s32 value)
{
	if (!added)
		return 0;
	return freq_qos_update_request(req, value);
}

static void apply_cpu_limits_locked(void)
{
	bool active = READ_ONCE(enabled) && READ_ONCE(cpu_enabled) &&
		thermal_level > 0 && cpu_cap >= 0;
	unsigned int i;

	init_cpu_limits_locked();
	for (i = 0; i < cpu_limit_count; i++) {
		struct cpu_limit *limit = &cpu_limits[i];
		s32 min_freq = FREQ_QOS_MIN_DEFAULT_VALUE;
		int min_ret;

		if (active && cpu_min[i] > 0)
			min_freq = cpu_min[i];

		min_ret = update_req(&limit->min_req, limit->min_added, min_freq);
		if (min_ret < 0)
			atomic64_inc(&cpu_errors);
		else
			atomic64_inc(&cpu_updates);
	}
}

static unsigned int boost_ceiling_locked(void)
{
	unsigned int ceiling = AX_BOOST_LEVEL_HEAVY;
	unsigned int clamp_level = READ_ONCE(resource_clamp_level);
	unsigned int block_level = READ_ONCE(resource_block_level);

	if (!READ_ONCE(enabled) || !READ_ONCE(resource_enabled) || thermal_level <= 0)
		return ceiling;

	if (block_level && thermal_level >= block_level)
		ceiling = 0;
	else if (clamp_level && thermal_level >= clamp_level)
		ceiling = AX_BOOST_LEVEL_LIGHT;

	if (boost_cap >= 0) {
		ceiling = min_t(unsigned int, ceiling, boost_cap);
		ceiling = min_t(unsigned int, ceiling, AX_BOOST_LEVEL_HEAVY);
	}
	return ceiling;
}

static void apply_resource_ceiling_locked(void)
{
	unsigned int ceiling = boost_ceiling_locked();

	pmqos_ceiling = ceiling ? AX_BOOST_LEVEL_HEAVY : 0;
	perf_ceiling = ceiling;
#if defined(ATCM_HAS_BOOST)
	ax_sched_boost_ceiling(AX_BOOST_PMQOS, pmqos_ceiling);
	ax_sched_boost_ceiling(AX_BOOST_GPU |
				       AX_BOOST_MEMLAT |
				       AX_BOOST_DDR |
				       AX_BOOST_L3,
				       perf_ceiling);
	atomic64_inc(&resource_updates);
#endif
}

static void apply_state_locked(void)
{
	apply_cpu_limits_locked();
	apply_resource_ceiling_locked();
}

static int parse_int(char *text, int *value)
{
	return kstrtoint(strstrip(text), 0, value);
}

static ssize_t state_write(struct file *file, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	char buffer[ATCM_CMD_SIZE];
	char *argv[ATCM_FIELDS];
	char *cursor;
	char *token;
	int values[ATCM_FIELDS];
	int argc = 0;
	int i;

	if (!count)
		return 0;
	if (count >= sizeof(buffer))
		count = sizeof(buffer) - 1;
	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	buffer[count] = '\0';
	cursor = buffer;
	while ((token = strsep(&cursor, " \t\n")) != NULL &&
	       argc < ARRAY_SIZE(argv)) {
		if (*token)
			argv[argc++] = token;
	}
	if (argc < ATCM_FIELDS)
		return -EINVAL;

	for (i = 0; i < ATCM_FIELDS; i++) {
		if (parse_int(argv[i], &values[i]))
			return -EINVAL;
	}

	mutex_lock(&lock);
	thermal_level = max(values[0], 0);
	cpu_cap = values[1];
	gpu_cap = values[2];
	boost_cap = values[3];
	for (i = 0; i < ATCM_CLUSTERS; i++)
		cpu_min[i] = max(values[4 + i], 0);
	apply_state_locked();
	mutex_unlock(&lock);

	atomic64_inc(&state_writes);
	return count;
}

static int state_show(struct seq_file *m, void *v)
{
	int i;

	mutex_lock(&lock);
	seq_printf(m, "level %d\n", thermal_level);
	seq_printf(m, "cpu_cap %d\n", cpu_cap);
	seq_printf(m, "gpu_cap %d\n", gpu_cap);
	seq_printf(m, "boost_cap %d\n", boost_cap);
	for (i = 0; i < ATCM_CLUSTERS; i++)
		seq_printf(m, "cluster%d_floor %d\n", i, cpu_min[i]);
	seq_printf(m, "pmqos_ceiling %u\n", pmqos_ceiling);
	seq_printf(m, "perf_ceiling %u\n", perf_ceiling);
	mutex_unlock(&lock);
	return 0;
}

static int state_open(struct inode *inode, struct file *file)
{
	return single_open(file, state_show, NULL);
}

static int stats_show(struct seq_file *m, void *v)
{
	seq_printf(m, "enabled %u\n", READ_ONCE(enabled));
	seq_printf(m, "cpu_enabled %u\n", READ_ONCE(cpu_enabled));
	seq_printf(m, "resource_enabled %u\n", READ_ONCE(resource_enabled));
	seq_printf(m, "resource_clamp_level %u\n",
		   READ_ONCE(resource_clamp_level));
	seq_printf(m, "resource_block_level %u\n",
		   READ_ONCE(resource_block_level));
	seq_printf(m, "cpu_limits %u\n", READ_ONCE(cpu_limit_count));
	seq_printf(m, "state_writes %lld\n",
		   (long long)atomic64_read(&state_writes));
	seq_printf(m, "cpu_updates %lld\n",
		   (long long)atomic64_read(&cpu_updates));
	seq_printf(m, "cpu_errors %lld\n",
		   (long long)atomic64_read(&cpu_errors));
	seq_printf(m, "resource_updates %lld\n",
		   (long long)atomic64_read(&resource_updates));
	return 0;
}

static int stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, stats_show, NULL);
}

#if defined(ATCM_HAS_PROC_OPS)
static const struct proc_ops state_fops = {
	.proc_open = state_open,
	.proc_read = seq_read,
	.proc_write = state_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops stats_fops = {
	.proc_open = stats_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#else
static const struct file_operations state_fops = {
	.owner = THIS_MODULE,
	.open = state_open,
	.read = seq_read,
	.write = state_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations stats_fops = {
	.owner = THIS_MODULE,
	.open = stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

static int create_proc(void)
{
	struct proc_dir_entry *entry;

	proc_dir = proc_mkdir(ATCM_PROC_DIR, NULL);
	if (!proc_dir)
		return -ENOMEM;

	entry = proc_create(ATCM_PROC_STATE, 0660, proc_dir, &state_fops);
	if (!entry) {
		remove_proc_entry(ATCM_PROC_DIR, NULL);
		proc_dir = NULL;
		return -ENOMEM;
	}
	proc_set_user(entry, KUIDT_INIT(1000), KGIDT_INIT(1000));

	entry = proc_create(ATCM_PROC_STATS, 0440, proc_dir, &stats_fops);
	if (!entry) {
		remove_proc_entry(ATCM_PROC_STATE, proc_dir);
		remove_proc_entry(ATCM_PROC_DIR, NULL);
		proc_dir = NULL;
		return -ENOMEM;
	}
	proc_set_user(entry, KUIDT_INIT(1000), KGIDT_INIT(1000));
	return 0;
}

static void remove_proc(void)
{
	if (!proc_dir)
		return;
	remove_proc_entry(ATCM_PROC_STATS, proc_dir);
	remove_proc_entry(ATCM_PROC_STATE, proc_dir);
	remove_proc_entry(ATCM_PROC_DIR, NULL);
	proc_dir = NULL;
}

static int __init atcm_init(void)
{
	int ret;

	mutex_lock(&lock);
	init_cpu_limits_locked();
	apply_resource_ceiling_locked();
	WRITE_ONCE(initialized, true);
	mutex_unlock(&lock);

	ret = create_proc();
	if (ret)
		pr_warn("proc init failed: %d\n", ret);
	return 0;
}

module_init(atcm_init);

static void __exit atcm_exit(void)
{
	unsigned int i;

	mutex_lock(&lock);
	WRITE_ONCE(initialized, false);
	thermal_level = 0;
	cpu_cap = -1;
	gpu_cap = -1;
	boost_cap = -1;
	apply_state_locked();
	for (i = 0; i < cpu_limit_count; i++) {
		struct cpu_limit *limit = &cpu_limits[i];

		if (limit->min_added) {
			freq_qos_update_request(&limit->min_req,
						FREQ_QOS_MIN_DEFAULT_VALUE);
			freq_qos_remove_request(&limit->min_req);
			limit->min_added = false;
		}
	}
	cpu_limit_count = 0;
	mutex_unlock(&lock);
	remove_proc();
}

module_exit(atcm_exit);

MODULE_LICENSE("GPL");
