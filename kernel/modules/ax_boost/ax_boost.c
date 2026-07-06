// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025-2026 AxionOS
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/err.h>
#if defined(AX_BOOST_HAS_DEVFREQ_PROVIDER)
#include <linux/devfreq.h>
#endif
#if defined(AX_BOOST_HAS_QCOM_HOLI_ICC_PROVIDER)
#include <linux/interconnect.h>
#include <dt-bindings/interconnect/qcom,holi.h>
#endif
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#if defined(BOOST_HAS_PMQOS)
#include <linux/pm_qos.h>
#endif
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/workqueue.h>
#include <ax_sched_common.h>

#define BOOST_CMD_SIZE 128
#define BOOST_PROC_DIR "ax_boost"
#define BOOST_PROC_BOOST "boost"
#define BOOST_PROC_STATS "stats"
#define BOOST_DEFAULT_MAX_DURATION_MS 1600
#define BOOST_DEFAULT_PMQOS_LATENCY_US 44
#define BOOST_DEFAULT_DEVFREQ_LIGHT_PCT 70
#define BOOST_DEFAULT_DEVFREQ_HEAVY_PCT 100
#define BOOST_DEFAULT_GPU_LIGHT_MBPS 2400
#define BOOST_DEFAULT_GPU_HEAVY_MBPS 5200
#define BOOST_DEFAULT_DDR_LIGHT_MBPS 3200
#define BOOST_DEFAULT_DDR_HEAVY_MBPS 6400

enum boost_res {
	BOOST_PMQOS,
	BOOST_GPU,
	BOOST_MEMLAT,
	BOOST_DDR,
	BOOST_L3,
	BOOST_COUNT,
};

static const unsigned int boost_bits[BOOST_COUNT] = {
	AX_BOOST_PMQOS,
	AX_BOOST_GPU,
	AX_BOOST_MEMLAT,
	AX_BOOST_DDR,
	AX_BOOST_L3,
};

static const char * const boost_names[BOOST_COUNT] = {
	"pmqos",
	"gpu",
	"memlat",
	"ddr",
	"l3",
};

static unsigned int boost_enabled = 1;
module_param_named(enabled, boost_enabled, uint, 0644);

static unsigned int boost_pmqos_enabled = 1;
module_param_named(pmqos_enabled, boost_pmqos_enabled, uint, 0644);

static unsigned int boost_gpu_enabled = 1;
module_param_named(gpu_enabled, boost_gpu_enabled, uint, 0644);

static unsigned int boost_memlat_enabled = 1;
module_param_named(memlat_enabled, boost_memlat_enabled, uint, 0644);

static unsigned int boost_ddr_enabled = 1;
module_param_named(ddr_enabled, boost_ddr_enabled, uint, 0644);

static unsigned int boost_l3_enabled = 1;
module_param_named(l3_enabled, boost_l3_enabled, uint, 0644);

static unsigned int boost_max_duration_ms = BOOST_DEFAULT_MAX_DURATION_MS;
module_param_named(max_duration_ms, boost_max_duration_ms, uint, 0644);

static unsigned int boost_pmqos_latency_us = BOOST_DEFAULT_PMQOS_LATENCY_US;
module_param_named(pmqos_latency_us, boost_pmqos_latency_us, uint, 0644);

static unsigned int boost_devfreq_light_pct = BOOST_DEFAULT_DEVFREQ_LIGHT_PCT;
module_param_named(devfreq_light_pct, boost_devfreq_light_pct, uint, 0644);

static unsigned int boost_devfreq_heavy_pct = BOOST_DEFAULT_DEVFREQ_HEAVY_PCT;
module_param_named(devfreq_heavy_pct, boost_devfreq_heavy_pct, uint, 0644);

static unsigned int boost_gpu_light_mbps = BOOST_DEFAULT_GPU_LIGHT_MBPS;
module_param_named(gpu_light_mbps, boost_gpu_light_mbps, uint, 0644);

static unsigned int boost_gpu_heavy_mbps = BOOST_DEFAULT_GPU_HEAVY_MBPS;
module_param_named(gpu_heavy_mbps, boost_gpu_heavy_mbps, uint, 0644);

static unsigned int boost_ddr_light_mbps = BOOST_DEFAULT_DDR_LIGHT_MBPS;
module_param_named(ddr_light_mbps, boost_ddr_light_mbps, uint, 0644);

static unsigned int boost_ddr_heavy_mbps = BOOST_DEFAULT_DDR_HEAVY_MBPS;
module_param_named(ddr_heavy_mbps, boost_ddr_heavy_mbps, uint, 0644);

static DEFINE_SPINLOCK(boost_lock);
static DEFINE_MUTEX(boost_provider_lock);
static struct delayed_work boost_work;
static struct proc_dir_entry *boost_proc_dir;
#if defined(BOOST_HAS_PMQOS)
static struct pm_qos_request boost_pmqos_request;
static bool boost_pmqos_added;
static bool boost_pmqos_active;
#endif
static unsigned int boost_level[BOOST_COUNT];
static unsigned int boost_source[BOOST_COUNT];
static unsigned long boost_expire_jiffies[BOOST_COUNT];
static unsigned int boost_source_level[BOOST_COUNT][AX_BOOST_SOURCE_MAX + 1];
static unsigned long boost_source_expire_jiffies[BOOST_COUNT][AX_BOOST_SOURCE_MAX + 1];
static unsigned int boost_atcm_ceiling[BOOST_COUNT];
static unsigned int boost_applied_level[BOOST_COUNT];
static unsigned int boost_applied_mask;
static u64 boost_generation;
static u64 boost_applied_generation;
static atomic64_t boost_kicks;
static atomic64_t boost_clears;
static atomic64_t boost_expires;
static atomic64_t boost_atcm_clamps;
static atomic64_t boost_pmqos_votes;
static atomic64_t boost_provider_hits;
static atomic64_t boost_provider_misses;

#if defined(AX_BOOST_HAS_DEVFREQ_PROVIDER)
struct boost_devfreq_target {
	const char *name;
	const char *alias;
	struct devfreq *devfreq;
	unsigned long vote;
	unsigned long saved_min;
	bool saved;
};

static struct boost_devfreq_target boost_gpu_devfreq_targets[] = {
	{ .name = "qcom,kgsl-3d0", .alias = "kgsl-3d0" },
	{ .name = "kgsl-busmon" },
	{ .name = "mali" },
};

static struct boost_devfreq_target boost_memlat_devfreq_targets[] = {
	{ .name = "qcom,cpu0-cpu-ddr-lat" },
	{ .name = "qcom,cpu6-cpu-ddr-lat" },
	{ .name = "qcom,cpu0-cpu-ddr-latfloor" },
	{ .name = "qcom,cpu6-cpu-ddr-latfloor" },
	{ .name = "devfreq_mif_cpu0_memlat" },
	{ .name = "devfreq_mif_cpu1_memlat" },
	{ .name = "devfreq_mif_cpu2_memlat" },
	{ .name = "devfreq_mif_cpu3_memlat" },
	{ .name = "devfreq_mif_cpu4_memlat" },
	{ .name = "devfreq_mif_cpu5_memlat" },
	{ .name = "devfreq_mif_cpu6_memlat" },
	{ .name = "devfreq_mif_cpu7_memlat" },
	{ .name = "devfreq_mif_cpu8_memlat" },
};

static struct boost_devfreq_target boost_ddr_devfreq_targets[] = {
	{ .name = "qcom,cpu-cpu-ddr-bw" },
	{ .name = "devfreq_mif" },
	{ .name = "devfreq_bw" },
	{ .name = "devfreq_bo" },
};

static struct boost_devfreq_target boost_l3_devfreq_targets[] = {
	{ .name = "qcom,cpu0-cpu-l3-lat" },
	{ .name = "qcom,cpu1-cpu-l3-lat" },
	{ .name = "qcom,cpu2-cpu-l3-lat" },
	{ .name = "qcom,cpu3-cpu-l3-lat" },
	{ .name = "qcom,cpu4-cpu-l3-lat" },
	{ .name = "qcom,cpu5-cpu-l3-lat" },
	{ .name = "qcom,cpu6-cpu-l3-lat" },
	{ .name = "qcom,cpu7-cpu-l3-lat" },
	{ .name = "devfreq_dsu_lat" },
	{ .name = "devfreq_dsu" },
};
#endif

#if defined(AX_BOOST_HAS_QCOM_HOLI_ICC_PROVIDER)
struct boost_icc_vote {
	int src;
	int dst;
	struct icc_path *path;
	u32 avg_bw;
	u32 peak_bw;
};

#if defined(AX_BOOST_HAS_GPU_PROVIDER)
static struct boost_icc_vote boost_gpu_icc_vote = {
	.src = MASTER_GRAPHICS_3D,
	.dst = SLAVE_EBI,
};
#endif

#if defined(AX_BOOST_HAS_DDR_PROVIDER)
static struct boost_icc_vote boost_ddr_icc_vote = {
	.src = MASTER_AMPSS_M0,
	.dst = SLAVE_EBI,
};
#endif

#if defined(AX_BOOST_HAS_GPU_PROVIDER) || defined(AX_BOOST_HAS_DDR_PROVIDER)
static u32 boost_icc_kbps(unsigned int mbps)
{
	return mbps * 1000;
}
#endif
#endif

#if defined(AX_BOOST_HAS_GPU_PROVIDER) || \
	defined(AX_BOOST_HAS_MEMLAT_PROVIDER) || \
	defined(AX_BOOST_HAS_DDR_PROVIDER) || \
	defined(AX_BOOST_HAS_L3_PROVIDER)
static void boost_merge_result(int *ret, int err)
{
	if (!err)
		*ret = 0;
	else if (*ret == -ENODEV)
		*ret = err;
}
#endif

#if defined(AX_BOOST_HAS_DEVFREQ_PROVIDER)
static struct devfreq *boost_find_devfreq(struct boost_devfreq_target *target)
{
	struct devfreq *devfreq;

	if (target->devfreq)
		return target->devfreq;

	devfreq = devfreq_get_by_name(target->name);
	if (IS_ERR(devfreq) && target->alias)
		devfreq = devfreq_get_by_name(target->alias);
	if (!IS_ERR(devfreq))
		target->devfreq = devfreq;

	return devfreq;
}

static unsigned int boost_devfreq_pct(unsigned int level)
{
	unsigned int pct;

	if (level >= AX_BOOST_LEVEL_HEAVY)
		pct = READ_ONCE(boost_devfreq_heavy_pct);
	else
		pct = READ_ONCE(boost_devfreq_light_pct);

	return clamp_t(unsigned int, pct, 1, 100);
}

static unsigned long boost_devfreq_floor(struct devfreq *devfreq,
					 unsigned int level)
{
	struct devfreq_dev_profile *profile = devfreq->profile;
	unsigned long min_freq = ULONG_MAX;
	unsigned long target_freq;
	unsigned long best_freq = 0;
	unsigned long max_freq = 0;
	unsigned int pct;
	unsigned int i;

	mutex_lock(&devfreq->lock);
	if (profile && profile->freq_table && profile->max_state) {
		for (i = 0; i < profile->max_state; i++) {
			unsigned long freq = profile->freq_table[i];

			if (!freq)
				continue;
			min_freq = min(min_freq, freq);
			max_freq = max(max_freq, freq);
		}
	}

	if (!max_freq || min_freq == ULONG_MAX) {
		min_freq = devfreq->scaling_min_freq;
		max_freq = devfreq->scaling_max_freq;
	}

	if (!max_freq || max_freq == ULONG_MAX || min_freq == ULONG_MAX) {
		mutex_unlock(&devfreq->lock);
		return 0;
	}

	pct = boost_devfreq_pct(level);
	target_freq = min_freq + (max_freq - min_freq) * pct / 100;
	best_freq = max_freq;
	if (profile && profile->freq_table && profile->max_state) {
		for (i = 0; i < profile->max_state; i++) {
			unsigned long freq = profile->freq_table[i];

			if (freq >= target_freq && freq < best_freq)
				best_freq = freq;
		}
	} else {
		best_freq = target_freq;
	}
	mutex_unlock(&devfreq->lock);

	return best_freq;
}

static unsigned long boost_devfreq_min(struct devfreq *devfreq)
{
#if defined(AX_BOOST_HAS_DEVFREQ_MIN_HELPER)
	return devfreq_get_min_freq(devfreq);
#else
	unsigned long min_freq;

	mutex_lock(&devfreq->lock);
	min_freq = devfreq->min_freq;
	mutex_unlock(&devfreq->lock);

	return min_freq;
#endif
}

static int boost_apply_devfreq_target(struct boost_devfreq_target *target,
				      unsigned int level)
{
	struct devfreq *devfreq;
	unsigned long floor = 0;
	int ret;

	devfreq = boost_find_devfreq(target);
	if (IS_ERR(devfreq))
		return PTR_ERR(devfreq);

	if (level) {
		floor = boost_devfreq_floor(devfreq, level);
		if (!floor)
			return -EINVAL;
		if (!target->saved) {
			target->saved_min = boost_devfreq_min(devfreq);
			target->saved = true;
		}
		floor = max(floor, target->saved_min);
	} else if (target->saved) {
		floor = target->saved_min;
	}

	if (target->vote == floor) {
		if (!level) {
			target->vote = 0;
			target->saved_min = 0;
			target->saved = false;
		}
		return 0;
	}

	ret = devfreq_set_min_freq(devfreq, floor);
	if (ret && !level && floor)
		ret = devfreq_set_min_freq(devfreq, 0);
	if (!ret) {
		target->vote = floor;
		if (!level) {
			target->vote = 0;
			target->saved_min = 0;
			target->saved = false;
		}
	}
	return ret;
}

static int boost_apply_devfreq_targets(struct boost_devfreq_target *targets,
				       unsigned int count, unsigned int level)
{
	bool attempted = false;
	int ret = -ENODEV;
	unsigned int i;

	for (i = 0; i < count; i++) {
		int err;

		if (!level && !targets[i].devfreq)
			continue;

		attempted = true;
		err = boost_apply_devfreq_target(&targets[i], level);
		boost_merge_result(&ret, err);
	}

	if (!attempted && !level)
		return 0;
	return ret;
}

static void boost_release_devfreq_targets(struct boost_devfreq_target *targets,
					  unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (!targets[i].devfreq)
			continue;
		if (targets[i].saved &&
		    devfreq_set_min_freq(targets[i].devfreq, targets[i].saved_min))
			devfreq_set_min_freq(targets[i].devfreq, 0);
		devfreq_put(targets[i].devfreq);
		targets[i].devfreq = NULL;
		targets[i].vote = 0;
		targets[i].saved_min = 0;
		targets[i].saved = false;
	}
}
#endif

#if defined(AX_BOOST_HAS_QCOM_HOLI_ICC_PROVIDER) && \
	(defined(AX_BOOST_HAS_GPU_PROVIDER) || \
	 defined(AX_BOOST_HAS_DDR_PROVIDER))
static int boost_apply_icc_vote(struct boost_icc_vote *vote,
				unsigned int level, unsigned int light_mbps,
				unsigned int heavy_mbps)
{
	struct icc_path *path;
	unsigned int mbps;
	u32 avg_bw = 0;
	u32 peak_bw = 0;
	int ret;

	if (!level && !vote->path)
		return -ENOENT;

	if (!vote->path) {
		path = icc_get(NULL, vote->src, vote->dst);
		if (IS_ERR_OR_NULL(path))
			return path ? PTR_ERR(path) : -ENODEV;
		vote->path = path;
	}

	if (level) {
		if (level >= AX_BOOST_LEVEL_HEAVY)
			mbps = heavy_mbps;
		else
			mbps = light_mbps;
		peak_bw = boost_icc_kbps(mbps);
		avg_bw = boost_icc_kbps(mbps / 2);
	}

	if (vote->avg_bw == avg_bw && vote->peak_bw == peak_bw)
		return 0;

	ret = icc_set_bw(vote->path, avg_bw, peak_bw);
	if (!ret) {
		vote->avg_bw = avg_bw;
		vote->peak_bw = peak_bw;
	}
	return ret;
}
#endif

#if defined(AX_BOOST_HAS_QCOM_HOLI_ICC_PROVIDER)
static void boost_release_icc_vote(struct boost_icc_vote *vote)
{
	if (!vote->path)
		return;

	icc_set_bw(vote->path, 0, 0);
	icc_put(vote->path);
	vote->path = NULL;
	vote->avg_bw = 0;
	vote->peak_bw = 0;
}
#endif

#if defined(AX_BOOST_HAS_GPU_PROVIDER)
int ax_boost_apply_gpu(unsigned int level, unsigned int duration_ms)
{
	int ret = -ENODEV;

	(void)duration_ms;
	mutex_lock(&boost_provider_lock);
#if defined(AX_BOOST_HAS_DEVFREQ_PROVIDER)
	boost_merge_result(&ret,
			   boost_apply_devfreq_targets(boost_gpu_devfreq_targets,
						       ARRAY_SIZE(boost_gpu_devfreq_targets),
						       level));
#endif
#if defined(AX_BOOST_HAS_QCOM_HOLI_ICC_PROVIDER)
	boost_merge_result(&ret,
			   boost_apply_icc_vote(&boost_gpu_icc_vote, level,
						READ_ONCE(boost_gpu_light_mbps),
						READ_ONCE(boost_gpu_heavy_mbps)));
#endif
	mutex_unlock(&boost_provider_lock);
	return ret;
}
#endif

#if defined(AX_BOOST_HAS_MEMLAT_PROVIDER)
int ax_boost_apply_memlat(unsigned int level, unsigned int duration_ms)
{
	int ret = -ENODEV;

	(void)duration_ms;
	mutex_lock(&boost_provider_lock);
#if defined(AX_BOOST_HAS_DEVFREQ_PROVIDER)
	boost_merge_result(&ret,
			   boost_apply_devfreq_targets(boost_memlat_devfreq_targets,
						       ARRAY_SIZE(boost_memlat_devfreq_targets),
						       level));
#endif
	mutex_unlock(&boost_provider_lock);
	return ret;
}
#endif

#if defined(AX_BOOST_HAS_DDR_PROVIDER)
int ax_boost_apply_ddr(unsigned int level, unsigned int duration_ms)
{
	int ret = -ENODEV;

	(void)duration_ms;
	mutex_lock(&boost_provider_lock);
#if defined(AX_BOOST_HAS_DEVFREQ_PROVIDER)
	boost_merge_result(&ret,
			   boost_apply_devfreq_targets(boost_ddr_devfreq_targets,
						       ARRAY_SIZE(boost_ddr_devfreq_targets),
						       level));
#endif
#if defined(AX_BOOST_HAS_QCOM_HOLI_ICC_PROVIDER)
	boost_merge_result(&ret,
			   boost_apply_icc_vote(&boost_ddr_icc_vote, level,
						READ_ONCE(boost_ddr_light_mbps),
						READ_ONCE(boost_ddr_heavy_mbps)));
#endif
	mutex_unlock(&boost_provider_lock);
	return ret;
}
#endif

#if defined(AX_BOOST_HAS_L3_PROVIDER)
int ax_boost_apply_l3(unsigned int level, unsigned int duration_ms)
{
	int ret = -ENODEV;

	(void)duration_ms;
	mutex_lock(&boost_provider_lock);
#if defined(AX_BOOST_HAS_DEVFREQ_PROVIDER)
	boost_merge_result(&ret,
			   boost_apply_devfreq_targets(boost_l3_devfreq_targets,
						       ARRAY_SIZE(boost_l3_devfreq_targets),
						       level));
#endif
	mutex_unlock(&boost_provider_lock);
	return ret;
}
#endif

static void boost_release_providers(void)
{
	mutex_lock(&boost_provider_lock);
#if defined(AX_BOOST_HAS_DEVFREQ_PROVIDER)
	boost_release_devfreq_targets(boost_gpu_devfreq_targets,
				      ARRAY_SIZE(boost_gpu_devfreq_targets));
	boost_release_devfreq_targets(boost_memlat_devfreq_targets,
				      ARRAY_SIZE(boost_memlat_devfreq_targets));
	boost_release_devfreq_targets(boost_ddr_devfreq_targets,
				      ARRAY_SIZE(boost_ddr_devfreq_targets));
	boost_release_devfreq_targets(boost_l3_devfreq_targets,
				      ARRAY_SIZE(boost_l3_devfreq_targets));
#endif
#if defined(AX_BOOST_HAS_QCOM_HOLI_ICC_PROVIDER) && \
	defined(AX_BOOST_HAS_GPU_PROVIDER)
	boost_release_icc_vote(&boost_gpu_icc_vote);
#endif
#if defined(AX_BOOST_HAS_QCOM_HOLI_ICC_PROVIDER) && \
	defined(AX_BOOST_HAS_DDR_PROVIDER)
	boost_release_icc_vote(&boost_ddr_icc_vote);
#endif
	mutex_unlock(&boost_provider_lock);
}

static unsigned int boost_filter_resources(unsigned int resources)
{
	resources &= AX_BOOST_ALL;
#if !defined(BOOST_HAS_PMQOS)
	resources &= ~AX_BOOST_PMQOS;
#endif
#if !defined(AX_BOOST_HAS_GPU_PROVIDER)
	resources &= ~AX_BOOST_GPU;
#endif
#if !defined(AX_BOOST_HAS_MEMLAT_PROVIDER)
	resources &= ~AX_BOOST_MEMLAT;
#endif
#if !defined(AX_BOOST_HAS_DDR_PROVIDER)
	resources &= ~AX_BOOST_DDR;
#endif
#if !defined(AX_BOOST_HAS_L3_PROVIDER)
	resources &= ~AX_BOOST_L3;
#endif
	if (!READ_ONCE(boost_pmqos_enabled))
		resources &= ~AX_BOOST_PMQOS;
	if (!READ_ONCE(boost_gpu_enabled))
		resources &= ~AX_BOOST_GPU;
	if (!READ_ONCE(boost_memlat_enabled))
		resources &= ~AX_BOOST_MEMLAT;
	if (!READ_ONCE(boost_ddr_enabled))
		resources &= ~AX_BOOST_DDR;
	if (!READ_ONCE(boost_l3_enabled))
		resources &= ~AX_BOOST_L3;
	return resources;
}

static unsigned int boost_clamp_level(unsigned int level)
{
	return min_t(unsigned int, max_t(unsigned int, level,
					AX_BOOST_LEVEL_LIGHT),
		     AX_BOOST_LEVEL_HEAVY);
}

static unsigned int boost_clamp_ceiling(unsigned int level)
{
	return min_t(unsigned int, level, AX_BOOST_LEVEL_HEAVY);
}

static unsigned int boost_clamp_duration(unsigned int duration_ms)
{
	unsigned int max_duration_ms = READ_ONCE(boost_max_duration_ms);

	if (max_duration_ms)
		duration_ms = min(duration_ms, max_duration_ms);
	return duration_ms;
}

static bool boost_source_valid(unsigned int source)
{
	return source > 0 && source <= AX_BOOST_SOURCE_MAX;
}

static unsigned long boost_min_delay(unsigned long delay, unsigned long next)
{
	if (!next)
		return delay;
	if (!delay || next < delay)
		return next;
	return delay;
}

static bool boost_refresh_resource_locked(enum boost_res index, unsigned long now)
{
	unsigned int best_source = 0;
	unsigned int best_level = 0;
	unsigned long best_expire = 0;
	unsigned int old_source = READ_ONCE(boost_source[index]);
	unsigned int old_level = READ_ONCE(boost_level[index]);
	unsigned long old_expire = READ_ONCE(boost_expire_jiffies[index]);
	unsigned int source;

	for (source = 1; source <= AX_BOOST_SOURCE_MAX; source++) {
		unsigned int level = READ_ONCE(boost_source_level[index][source]);
		unsigned long expire = READ_ONCE(boost_source_expire_jiffies[index][source]);

		if (!level)
			continue;
		if (time_after_eq(now, expire)) {
			WRITE_ONCE(boost_source_level[index][source], 0);
			WRITE_ONCE(boost_source_expire_jiffies[index][source], 0);
			continue;
		}
		if (level > best_level ||
		    (level == best_level && time_after(expire, best_expire))) {
			best_level = level;
			best_source = source;
			best_expire = expire;
		}
	}

	WRITE_ONCE(boost_level[index], best_level);
	WRITE_ONCE(boost_source[index], best_source);
	WRITE_ONCE(boost_expire_jiffies[index], best_expire);
	return old_level != best_level || old_source != best_source ||
		old_expire != best_expire;
}

static unsigned long boost_next_delay_locked(unsigned long now)
{
	unsigned long delay = 0;
	int i;

	for (i = 0; i < BOOST_COUNT; i++) {
		unsigned long expire = READ_ONCE(boost_expire_jiffies[i]);

		if (READ_ONCE(boost_level[i]) &&
		    time_before(now, expire))
			delay = boost_min_delay(delay,
						max_t(unsigned long,
						      expire - now, 1));
	}

	return delay;
}

static void boost_clear_source_locked(unsigned int source)
{
	unsigned long now = jiffies;
	bool changed = false;
	int i;

	for (i = 0; i < BOOST_COUNT; i++) {
		unsigned int slot;

		for (slot = 1; slot <= AX_BOOST_SOURCE_MAX; slot++) {
			if (source && slot != source)
				continue;
			if (READ_ONCE(boost_source_level[i][slot])) {
				WRITE_ONCE(boost_source_level[i][slot], 0);
				WRITE_ONCE(boost_source_expire_jiffies[i][slot], 0);
				changed = true;
			}
		}
		if (boost_refresh_resource_locked(i, now))
			changed = true;
	}

	if (changed) {
		atomic64_inc(&boost_clears);
		boost_generation++;
	}
}

static void boost_clear_locked(void)
{
	boost_clear_source_locked(0);
}

static void boost_prune_locked(unsigned long now)
{
	bool changed = false;
	int i;

	for (i = 0; i < BOOST_COUNT; i++) {
		unsigned int old_level = READ_ONCE(boost_level[i]);

		if (boost_refresh_resource_locked(i, now)) {
			changed = true;
			if (old_level)
				atomic64_inc(&boost_expires);
		}
	}

	if (changed)
		boost_generation++;
}

static void boost_release_pmqos(void)
{
#if defined(BOOST_HAS_PMQOS)
	if (!boost_pmqos_added || !boost_pmqos_active)
		return;

	pm_qos_update_request(&boost_pmqos_request, PM_QOS_DEFAULT_VALUE);
	boost_pmqos_active = false;
#endif
}

static void boost_apply_pmqos(unsigned int level)
{
#if defined(BOOST_HAS_PMQOS)
	s32 latency_us;

	if (!boost_pmqos_added)
		return;

	if (!level) {
		boost_release_pmqos();
		return;
	}

	latency_us = min_t(unsigned int, READ_ONCE(boost_pmqos_latency_us),
			   INT_MAX);
	pm_qos_update_request(&boost_pmqos_request, latency_us);
	boost_pmqos_active = true;
	atomic64_inc(&boost_pmqos_votes);
#else
	(void)level;
#endif
}

static int boost_apply_gpu(unsigned int level, unsigned int duration_ms)
{
#if defined(AX_BOOST_HAS_GPU_PROVIDER)
	return ax_boost_apply_gpu(level, duration_ms);
#else
	(void)level;
	(void)duration_ms;
	return -ENOENT;
#endif
}

static int boost_apply_memlat(unsigned int level, unsigned int duration_ms)
{
#if defined(AX_BOOST_HAS_MEMLAT_PROVIDER)
	return ax_boost_apply_memlat(level, duration_ms);
#else
	(void)level;
	(void)duration_ms;
	return -ENOENT;
#endif
}

static int boost_apply_ddr(unsigned int level, unsigned int duration_ms)
{
#if defined(AX_BOOST_HAS_DDR_PROVIDER)
	return ax_boost_apply_ddr(level, duration_ms);
#else
	(void)level;
	(void)duration_ms;
	return -ENOENT;
#endif
}

static int boost_apply_l3(unsigned int level, unsigned int duration_ms)
{
#if defined(AX_BOOST_HAS_L3_PROVIDER)
	return ax_boost_apply_l3(level, duration_ms);
#else
	(void)level;
	(void)duration_ms;
	return -ENOENT;
#endif
}

static void boost_apply_provider(enum boost_res index, unsigned int level,
				 unsigned int duration_ms)
{
	int ret = -EINVAL;

	switch (index) {
	case BOOST_GPU:
		ret = boost_apply_gpu(level, duration_ms);
		break;
	case BOOST_MEMLAT:
		ret = boost_apply_memlat(level, duration_ms);
		break;
	case BOOST_DDR:
		ret = boost_apply_ddr(level, duration_ms);
		break;
	case BOOST_L3:
		ret = boost_apply_l3(level, duration_ms);
		break;
	default:
		break;
	}

	if (ret)
		atomic64_inc(&boost_provider_misses);
	else
		atomic64_inc(&boost_provider_hits);
}

static void boost_apply_resource(enum boost_res index, unsigned int level,
				 unsigned int duration_ms)
{
	if (index == BOOST_PMQOS)
		boost_apply_pmqos(level);
	else
		boost_apply_provider(index, level, duration_ms);
}

static unsigned int boost_effective_level(enum boost_res index,
					  unsigned int level)
{
	unsigned int ceiling = READ_ONCE(boost_atcm_ceiling[index]);

	if (level && ceiling < level) {
		atomic64_inc(&boost_atcm_clamps);
		return ceiling;
	}
	return level;
}

static void boost_work_fn(struct work_struct *work)
{
	unsigned int level[BOOST_COUNT];
	unsigned int duration_ms[BOOST_COUNT];
	unsigned int active_mask = 0;
	unsigned long flags;
	unsigned long delay;
	unsigned long now;
	u64 generation;
	int i;

	spin_lock_irqsave(&boost_lock, flags);
	now = jiffies;
	boost_prune_locked(now);
	generation = boost_generation;
	delay = boost_next_delay_locked(now);
	for (i = 0; i < BOOST_COUNT; i++) {
		unsigned long expire = READ_ONCE(boost_expire_jiffies[i]);
		unsigned int requested_level = READ_ONCE(boost_level[i]);

		level[i] = boost_effective_level(i, requested_level);
		if (requested_level && level[i] && time_before(now, expire)) {
			duration_ms[i] = jiffies_to_msecs(expire - now);
			active_mask |= boost_bits[i];
		} else {
			duration_ms[i] = 0;
		}
	}
	spin_unlock_irqrestore(&boost_lock, flags);

	for (i = 0; i < BOOST_COUNT; i++) {
		bool active = active_mask & boost_bits[i];
		bool applied = boost_applied_mask & boost_bits[i];

		if (active && (generation != boost_applied_generation ||
			       !applied || boost_applied_level[i] != level[i])) {
			boost_apply_resource(i, level[i], duration_ms[i]);
			boost_applied_mask |= boost_bits[i];
			boost_applied_level[i] = level[i];
		} else if (!active && applied) {
			boost_apply_resource(i, 0, 0);
			boost_applied_mask &= ~boost_bits[i];
			boost_applied_level[i] = 0;
		}
	}

	boost_applied_generation = generation;
	if (delay)
		mod_delayed_work(system_wq, &boost_work, delay);
}

void ax_boost_kick(unsigned int source, unsigned int level, unsigned int resources,
		   unsigned int duration_ms)
{
	unsigned long flags;
	unsigned long expire;
	unsigned int delay_ms;
	unsigned long now;
	bool changed = false;
	int i;

	if (!READ_ONCE(boost_enabled) || !level || !duration_ms)
		return;

	if (!boost_source_valid(source))
		source = AX_BOOST_SOURCE_MANUAL;

	resources = boost_filter_resources(resources);
	if (!resources)
		return;

	delay_ms = boost_clamp_duration(duration_ms);
	if (!delay_ms)
		return;

	level = boost_clamp_level(level);
	now = jiffies;
	expire = now + max_t(unsigned long, msecs_to_jiffies(delay_ms), 1);

	spin_lock_irqsave(&boost_lock, flags);
	for (i = 0; i < BOOST_COUNT; i++) {
		unsigned int current_level;
		unsigned long current_expire;

		if (!(resources & boost_bits[i]))
			continue;
		current_level = READ_ONCE(boost_source_level[i][source]);
		current_expire = READ_ONCE(boost_source_expire_jiffies[i][source]);
		if (time_after_eq(now, current_expire)) {
			current_level = 0;
			current_expire = 0;
			WRITE_ONCE(boost_source_level[i][source], 0);
			WRITE_ONCE(boost_source_expire_jiffies[i][source], 0);
		}

		if (current_level && level < current_level)
			continue;

		if (level > current_level || time_after(expire, current_expire)) {
			WRITE_ONCE(boost_source_level[i][source], level);
			WRITE_ONCE(boost_source_expire_jiffies[i][source], expire);
			boost_refresh_resource_locked(i, now);
			changed = true;
		}
	}
	if (changed) {
		boost_generation++;
		atomic64_inc(&boost_kicks);
	}
	spin_unlock_irqrestore(&boost_lock, flags);

	if (changed)
		mod_delayed_work(system_wq, &boost_work, 0);
}
EXPORT_SYMBOL_GPL(ax_boost_kick);

void ax_boost_clear(unsigned int source)
{
	unsigned long flags;

	spin_lock_irqsave(&boost_lock, flags);
	boost_clear_source_locked(source);
	spin_unlock_irqrestore(&boost_lock, flags);

	mod_delayed_work(system_wq, &boost_work, 0);
}
EXPORT_SYMBOL_GPL(ax_boost_clear);

void ax_boost_set_ceiling(unsigned int resources, unsigned int level)
{
	unsigned long flags;
	unsigned int ceiling;
	bool changed = false;
	int i;

	resources &= AX_BOOST_ALL;
	if (!resources)
		resources = AX_BOOST_ALL;

	ceiling = boost_clamp_ceiling(level);
	spin_lock_irqsave(&boost_lock, flags);
	for (i = 0; i < BOOST_COUNT; i++) {
		if (!(resources & boost_bits[i]))
			continue;
		if (READ_ONCE(boost_atcm_ceiling[i]) == ceiling)
			continue;
		WRITE_ONCE(boost_atcm_ceiling[i], ceiling);
		changed = true;
	}
	if (changed)
		boost_generation++;
	spin_unlock_irqrestore(&boost_lock, flags);

	if (changed)
		mod_delayed_work(system_wq, &boost_work, 0);
}
EXPORT_SYMBOL_GPL(ax_boost_set_ceiling);

static int boost_parse_uint(char *text, unsigned int *value)
{
	return kstrtouint(strstrip(text), 0, value);
}

static ssize_t boost_write(struct file *file, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	char buffer[BOOST_CMD_SIZE];
	char *argv[4];
	char *cursor;
	char *token;
	unsigned long flags;
	unsigned int source;
	unsigned int level;
	unsigned int resources;
	unsigned int duration_ms;
	int argc = 0;

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

	if (boost_parse_uint(argv[0], &source) ||
	    boost_parse_uint(argv[1], &level) ||
	    boost_parse_uint(argv[2], &resources) ||
	    boost_parse_uint(argv[3], &duration_ms))
		return -EINVAL;

	if (!level || !resources || !duration_ms || !READ_ONCE(boost_enabled)) {
		spin_lock_irqsave(&boost_lock, flags);
		boost_clear_locked();
		spin_unlock_irqrestore(&boost_lock, flags);
		mod_delayed_work(system_wq, &boost_work, 0);
		return count;
	}

	ax_boost_kick(source, level, resources, duration_ms);
	return count;
}

static int boost_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&boost_lock, flags);
	for (i = 0; i < BOOST_COUNT; i++) {
		unsigned long remaining = 0;
		unsigned long expire = READ_ONCE(boost_expire_jiffies[i]);

		if (READ_ONCE(boost_level[i]) &&
		    time_before(jiffies, expire))
			remaining = jiffies_to_msecs(expire - jiffies);
		seq_printf(m, "%s %u %u %lu\n", boost_names[i],
			   remaining ? READ_ONCE(boost_level[i]) : 0,
			   remaining ? READ_ONCE(boost_source[i]) : 0,
			   remaining);
	}
	spin_unlock_irqrestore(&boost_lock, flags);
	return 0;
}

static int boost_open(struct inode *inode, struct file *file)
{
	return single_open(file, boost_show, NULL);
}

static int boost_stats_show(struct seq_file *m, void *v)
{
	int i;

	seq_printf(m, "enabled %u\n", READ_ONCE(boost_enabled));
	seq_printf(m, "applied_mask 0x%x\n", READ_ONCE(boost_applied_mask));
	seq_printf(m, "devfreq_light_pct %u\n",
		   READ_ONCE(boost_devfreq_light_pct));
	seq_printf(m, "devfreq_heavy_pct %u\n",
		   READ_ONCE(boost_devfreq_heavy_pct));
	seq_printf(m, "gpu_light_mbps %u\n", READ_ONCE(boost_gpu_light_mbps));
	seq_printf(m, "gpu_heavy_mbps %u\n", READ_ONCE(boost_gpu_heavy_mbps));
	seq_printf(m, "ddr_light_mbps %u\n", READ_ONCE(boost_ddr_light_mbps));
	seq_printf(m, "ddr_heavy_mbps %u\n", READ_ONCE(boost_ddr_heavy_mbps));
	for (i = 0; i < BOOST_COUNT; i++)
		seq_printf(m, "atcm_ceiling_%s %u\n", boost_names[i],
			   READ_ONCE(boost_atcm_ceiling[i]));
	seq_printf(m, "kicks %lld\n",
		   (long long)atomic64_read(&boost_kicks));
	seq_printf(m, "clears %lld\n",
		   (long long)atomic64_read(&boost_clears));
	seq_printf(m, "expires %lld\n",
		   (long long)atomic64_read(&boost_expires));
	seq_printf(m, "atcm_clamps %lld\n",
		   (long long)atomic64_read(&boost_atcm_clamps));
	seq_printf(m, "pmqos_votes %lld\n",
		   (long long)atomic64_read(&boost_pmqos_votes));
	seq_printf(m, "provider_hits %lld\n",
		   (long long)atomic64_read(&boost_provider_hits));
	seq_printf(m, "provider_misses %lld\n",
		   (long long)atomic64_read(&boost_provider_misses));
	return 0;
}

static int boost_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, boost_stats_show, NULL);
}

#if defined(BOOST_HAS_PROC_OPS)
static const struct proc_ops boost_fops = {
	.proc_open = boost_open,
	.proc_read = seq_read,
	.proc_write = boost_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const struct proc_ops boost_stats_fops = {
	.proc_open = boost_stats_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#else
static const struct file_operations boost_fops = {
	.owner = THIS_MODULE,
	.open = boost_open,
	.read = seq_read,
	.write = boost_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations boost_stats_fops = {
	.owner = THIS_MODULE,
	.open = boost_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

static int boost_create_proc(void)
{
	struct proc_dir_entry *entry;

	boost_proc_dir = proc_mkdir(BOOST_PROC_DIR, NULL);
	if (!boost_proc_dir)
		return -ENOMEM;

	entry = proc_create(BOOST_PROC_BOOST, 0660, boost_proc_dir,
			    &boost_fops);
	if (!entry) {
		remove_proc_entry(BOOST_PROC_DIR, NULL);
		boost_proc_dir = NULL;
		return -ENOMEM;
	}

	proc_set_user(entry, KUIDT_INIT(1000), KGIDT_INIT(1000));
	entry = proc_create(BOOST_PROC_STATS, 0440, boost_proc_dir,
			    &boost_stats_fops);
	if (!entry) {
		remove_proc_entry(BOOST_PROC_BOOST, boost_proc_dir);
		remove_proc_entry(BOOST_PROC_DIR, NULL);
		boost_proc_dir = NULL;
		return -ENOMEM;
	}

	proc_set_user(entry, KUIDT_INIT(1000), KGIDT_INIT(1000));
	return 0;
}

static void boost_remove_proc(void)
{
	if (!boost_proc_dir)
		return;

	remove_proc_entry(BOOST_PROC_STATS, boost_proc_dir);
	remove_proc_entry(BOOST_PROC_BOOST, boost_proc_dir);
	remove_proc_entry(BOOST_PROC_DIR, NULL);
	boost_proc_dir = NULL;
}

static int __init boost_init(void)
{
	int ret;
	int i;

	INIT_DELAYED_WORK(&boost_work, boost_work_fn);
	for (i = 0; i < BOOST_COUNT; i++)
		WRITE_ONCE(boost_atcm_ceiling[i],
			   AX_BOOST_LEVEL_HEAVY);
#if defined(BOOST_HAS_PMQOS)
	pm_qos_add_request(&boost_pmqos_request, PM_QOS_CPU_DMA_LATENCY,
			   PM_QOS_DEFAULT_VALUE);
	boost_pmqos_added = true;
#endif

	ret = boost_create_proc();
	if (ret) {
		pr_warn("proc init failed: %d\n", ret);
		boost_remove_proc();
	}

	return 0;
}

module_init(boost_init);

static void __exit boost_exit(void)
{
	unsigned long flags;

	spin_lock_irqsave(&boost_lock, flags);
	boost_clear_locked();
	spin_unlock_irqrestore(&boost_lock, flags);
	cancel_delayed_work_sync(&boost_work);
	boost_work_fn(&boost_work.work);
	boost_release_providers();
#if defined(BOOST_HAS_PMQOS)
	if (boost_pmqos_added)
		pm_qos_remove_request(&boost_pmqos_request);
#endif
	boost_remove_proc();
}

module_exit(boost_exit);

MODULE_LICENSE("GPL");
