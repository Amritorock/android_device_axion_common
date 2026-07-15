/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2025-2026 AxionOS
 */

#ifndef AX_SCHED_COMMON_H
#define AX_SCHED_COMMON_H

#include <linux/bits.h>
#include <linux/cpumask.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/topology.h>
#include <linux/string.h>
#include <linux/tracepoint.h>

#define ax_sched_unregister_hook(name, probe, data) \
	tracepoint_probe_unregister(&__tracepoint_##name, (void *)(probe), data)

#define AX_SCHED_RENDER_THREAD "RenderThread"
#define AX_SCHED_GPU_THREAD "GPU"
#define AX_SCHED_HWC_THREAD "HWC"
#define AX_SCHED_HWUI_TASK "hwuiTask"
#define AX_SCHED_GL_THREAD "GLThread"
#define AX_SCHED_ANDROID_ANIM "android.anim"
#define AX_SCHED_ANDROID_ANIM_LF "android.anim.lf"
#define AX_SCHED_ANDROID_DISPLAY "android.display"
#define AX_SCHED_ANDROID_UI "android.ui"
#define AX_SCHED_POWER_MANAGER "PowerManagerSer"
#define AX_SCHED_PHOTONIC_MODULATOR "PhotonicModulat"
#define AX_SCHED_SHELL_SPLASHSCREEN "ll.splashscreen"
#define AX_SCHED_LAUNCHER_UI_HELPER "UiThreadHelper"
#define AX_SCHED_HWC_RELEASE "HWC release"
#define AX_SCHED_GPU_COMPLETION "GPU completion"
#define AX_SCHED_ALLOCATE_BUFFERS "allocateBuffers"
#define AX_SCHED_KGSL_WORKER "kgsl_worker_thr"
#define AX_SCHED_HWC_ASYNC_WORKER "HwcAsyncWorker"
#define AX_SCHED_RE_COMPLETION "RE Completion"
#define AX_SCHED_SF_BACKGROUND_EXEC "BckgrndExec"
#define AX_SCHED_COMPOSER_SERVICE "composer-servic"
#define AX_SCHED_CHROME_IO_THREAD "Chrome_IOThread"
#define AX_SCHED_CHROME_CHILD_IO "Chrome_ChildIOT"
#define AX_SCHED_CHROME_INPROC "Chrome_InProc"
#define AX_SCHED_CHROME_RENDERER "CrRendererMain"
#define AX_SCHED_CHROME_GPU "CrGpuMain"
#define AX_SCHED_CHROME_THREAD_POOL "ThreadPoolForeg"
#define AX_SCHED_COMPOSITOR "Compositor"
#define AX_SCHED_VIZ_COMPOSITOR "VizCompositorTh"
#define AX_SCHED_JNI_SURFACE "JNISurfaceTextu"
#define AX_SCHED_CPU_NONE (-1)

#define AX_BOOST_PMQOS BIT(0)
#define AX_BOOST_GPU BIT(1)
#define AX_BOOST_MEMLAT BIT(2)
#define AX_BOOST_DDR BIT(3)
#define AX_BOOST_L3 BIT(4)
#define AX_BOOST_ALL \
	(AX_BOOST_PMQOS | AX_BOOST_GPU | \
	 AX_BOOST_MEMLAT | AX_BOOST_DDR | AX_BOOST_L3)

#define AX_BOOST_SOURCE_FRAME 1
#define AX_BOOST_SOURCE_LAUNCH 2
#define AX_BOOST_SOURCE_ANIMATION 3
#define AX_BOOST_SOURCE_REMOTE 4
#define AX_BOOST_SOURCE_BINDER 5
#define AX_BOOST_SOURCE_MANUAL 6
#define AX_BOOST_SOURCE_GAME 7
#define AX_BOOST_SOURCE_BURST_BASE 16
#define AX_BOOST_SOURCE_BURST_MAX \
	(AX_BOOST_SOURCE_BURST_BASE + 15)
#define AX_BOOST_SOURCE_MAX AX_BOOST_SOURCE_BURST_MAX

#define AX_GAME_BOOST_PROFILE_SUSTAINED 1
#define AX_GAME_BOOST_PROFILE_RENDER 2
#define AX_GAME_BOOST_PROFILE_LOADING 3
#define AX_GAME_BOOST_PROFILE_CPU 4

#define AX_BOOST_LEVEL_LIGHT 1
#define AX_BOOST_LEVEL_HEAVY 2

struct cpufreq_policy;

static inline bool ax_sched_comm_has_prefix(const char *comm, const char *prefix,
					    size_t len)
{
	return !strncmp(comm, prefix, len);
}

static inline bool ax_sched_comm_matches_render_helper(const char *comm)
{
	return !strcmp(comm, AX_SCHED_RENDER_THREAD) ||
		!strcmp(comm, AX_SCHED_GPU_THREAD) ||
		!strcmp(comm, AX_SCHED_HWC_THREAD) ||
		ax_sched_comm_has_prefix(comm, AX_SCHED_HWUI_TASK,
					 sizeof(AX_SCHED_HWUI_TASK) - 1) ||
		ax_sched_comm_has_prefix(comm, AX_SCHED_GL_THREAD,
					 sizeof(AX_SCHED_GL_THREAD) - 1) ||
		!strcmp(comm, AX_SCHED_HWC_RELEASE) ||
		!strcmp(comm, AX_SCHED_GPU_COMPLETION) ||
		!strcmp(comm, AX_SCHED_ALLOCATE_BUFFERS) ||
		!strcmp(comm, AX_SCHED_LAUNCHER_UI_HELPER) ||
		!strcmp(comm, AX_SCHED_SHELL_SPLASHSCREEN) ||
		ax_sched_comm_has_prefix(comm, AX_SCHED_CHROME_IO_THREAD,
					 sizeof(AX_SCHED_CHROME_IO_THREAD) - 1) ||
		ax_sched_comm_has_prefix(comm, AX_SCHED_CHROME_CHILD_IO,
					 sizeof(AX_SCHED_CHROME_CHILD_IO) - 1) ||
		ax_sched_comm_has_prefix(comm, AX_SCHED_CHROME_INPROC,
					 sizeof(AX_SCHED_CHROME_INPROC) - 1) ||
		ax_sched_comm_has_prefix(comm, AX_SCHED_CHROME_RENDERER,
					 sizeof(AX_SCHED_CHROME_RENDERER) - 1) ||
		ax_sched_comm_has_prefix(comm, AX_SCHED_CHROME_GPU,
					 sizeof(AX_SCHED_CHROME_GPU) - 1) ||
		ax_sched_comm_has_prefix(comm, AX_SCHED_COMPOSITOR,
					 sizeof(AX_SCHED_COMPOSITOR) - 1) ||
		ax_sched_comm_has_prefix(comm, AX_SCHED_VIZ_COMPOSITOR,
					 sizeof(AX_SCHED_VIZ_COMPOSITOR) - 1) ||
		ax_sched_comm_has_prefix(comm, AX_SCHED_JNI_SURFACE,
					 sizeof(AX_SCHED_JNI_SURFACE) - 1);
}

static inline bool ax_sched_task_is_render_helper(struct task_struct *task)
{
	return task && ax_sched_comm_matches_render_helper(task->comm);
}

static inline unsigned int ax_sched_clamp_util(unsigned int util)
{
	return min_t(unsigned int, util, SCHED_CAPACITY_SCALE);
}

enum ax_sched_pick_prio {
	AX_SCHED_PRIO_NONE = 0,
	AX_SCHED_PRIO_BURST = 10,
	AX_SCHED_PRIO_GAME = 15,
	AX_SCHED_PRIO_SVP = 20,
	AX_SCHED_PRIO_FRAME = 30,
};

struct ax_sched_cpu_pick {
	int cpu;
	unsigned int prio;
};

static inline void ax_sched_init_cpu_pick(struct ax_sched_cpu_pick *pick)
{
	pick->cpu = AX_SCHED_CPU_NONE;
	pick->prio = AX_SCHED_PRIO_NONE;
}

static inline bool ax_sched_has_cpu_pick(struct ax_sched_cpu_pick *pick)
{
	return pick && pick->cpu >= 0;
}

static inline bool ax_sched_set_cpu_pick(struct ax_sched_cpu_pick *pick,
					 int cpu, unsigned int prio)
{
	if (!pick || cpu < 0 || prio < pick->prio)
		return false;

	pick->cpu = cpu;
	pick->prio = prio;
	return true;
}

unsigned int ax_burst_sched_task_util(struct task_struct *task);
unsigned int ax_burst_sched_binder_util(struct task_struct *task);
void ax_burst_sched_note_binder_assist(struct task_struct *task,
				       unsigned int util_min);
unsigned int ax_game_boost_active_util(void);
unsigned int ax_game_boost_task_util(struct task_struct *task);
bool ax_game_boost_pick_task_rq(struct task_struct *task,
				       struct ax_sched_cpu_pick *pick);
bool ax_game_boost_pick_fallback_rq(int prev_cpu, struct task_struct *task,
					   struct ax_sched_cpu_pick *pick);
bool ax_game_boost_pick_lowest_rq(struct task_struct *task,
					 struct cpumask *local_cpu_mask,
					 struct ax_sched_cpu_pick *pick);
void ax_game_boost_update(pid_t pid, unsigned int profile,
			  unsigned int level, unsigned int duration_ms, bool sticky,
			  int tid_count, const pid_t *tids);
void ax_game_boost_clear(pid_t pid);
unsigned int ax_frame_boost_active_util(void);
unsigned int ax_frame_boost_task_util(struct task_struct *task);
void ax_frame_boost_note_cpu(struct task_struct *task, int cpu);
void ax_frame_boost_enqueue_task(struct task_struct *task);
void ax_frame_boost_dequeue_task(struct task_struct *task);
void ax_frame_boost_map_util_freq(unsigned long util, unsigned long freq,
				  unsigned long cap,
				  unsigned long *next_freq,
				  struct cpufreq_policy *policy);
#if defined(CONFIG_UCLAMP_TASK)
void ax_frame_boost_uclamp_eff_get(struct task_struct *task,
				   enum uclamp_id clamp_id,
				   struct uclamp_se *uclamp_max,
				   struct uclamp_se *uclamp_eff, int *ret);
#endif
void ax_boost_kick(unsigned int source, unsigned int level, unsigned int resources,
		   unsigned int duration_ms);
void ax_boost_clear(unsigned int source);
void ax_boost_set_ceiling(unsigned int resources, unsigned int level);

#if defined(AX_SCHED_HAS_BOOST)
static inline void ax_sched_boost(unsigned int source, unsigned int level,
				  unsigned int resources,
				  unsigned int duration_ms)
{
	if (!level || !resources || !duration_ms)
		return;

	ax_boost_kick(source, level, resources, duration_ms);
}

static inline void ax_sched_boost_clear(unsigned int source)
{
	ax_boost_clear(source);
}

static inline void ax_sched_boost_ceiling(unsigned int resources, unsigned int level)
{
	ax_boost_set_ceiling(resources, level);
}
#else
static inline void ax_sched_boost(unsigned int source, unsigned int level,
				  unsigned int resources,
				  unsigned int duration_ms)
{
	(void)source;
	(void)level;
	(void)resources;
	(void)duration_ms;
}

static inline void ax_sched_boost_clear(unsigned int source)
{
	(void)source;
}

static inline void ax_sched_boost_ceiling(unsigned int resources, unsigned int level)
{
	(void)resources;
	(void)level;
}
#endif

void ax_thread_snooper_track(pid_t pid, bool enabled);

#if defined(AX_SCHED_HAS_THREAD_SNOOPER)
static inline void ax_sched_thread_snooper_track(pid_t pid, bool enabled)
{
	ax_thread_snooper_track(pid, enabled);
}
#else
static inline void ax_sched_thread_snooper_track(pid_t pid, bool enabled)
{
	(void)pid;
	(void)enabled;
}
#endif

#if defined(AX_BOOST_HAS_GPU_PROVIDER)
int ax_boost_apply_gpu(unsigned int level, unsigned int duration_ms);
#endif
#if defined(AX_BOOST_HAS_MEMLAT_PROVIDER)
int ax_boost_apply_memlat(unsigned int level, unsigned int duration_ms);
#endif
#if defined(AX_BOOST_HAS_DDR_PROVIDER)
int ax_boost_apply_ddr(unsigned int level, unsigned int duration_ms);
#endif
#if defined(AX_BOOST_HAS_L3_PROVIDER)
int ax_boost_apply_l3(unsigned int level, unsigned int duration_ms);
#endif

#if defined(CONFIG_UCLAMP_TASK)
static inline unsigned int ax_sched_uclamp_bucket_id(unsigned int value)
{
	unsigned int delta = DIV_ROUND_CLOSEST(SCHED_CAPACITY_SCALE,
					       UCLAMP_BUCKETS);

	return min_t(unsigned int, value / delta, UCLAMP_BUCKETS - 1);
}

static inline void ax_sched_set_uclamp(struct uclamp_se *uclamp,
				       unsigned int value)
{
	uclamp->value = value;
	uclamp->bucket_id = ax_sched_uclamp_bucket_id(value);
	uclamp->active = false;
	uclamp->user_defined = false;
}

static inline unsigned int ax_sched_merge_uclamp(struct uclamp_se *uclamp,
						 unsigned int value)
{
	return uclamp ? max_t(unsigned int, value, uclamp->value) : value;
}
#endif

#endif
