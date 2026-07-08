axion_vendor_hooks := $(filter y,$(CONFIG_ANDROID_VENDOR_HOOKS))
axion_sched_rq := $(and $(axion_vendor_hooks),\
	$(filter y,$(AXION_KERNEL_MODULES_SCHED_RQ_HOOKS)))
axion_uclamp_eff := $(filter y,$(AXION_KERNEL_MODULES_UCLAMP_EFF_HOOK))
axion_binder_ux := $(and $(axion_vendor_hooks),\
	$(filter y,$(AXION_KERNEL_MODULES_BINDER_UX_HOOKS)))
axion_binder_obs := $(and $(axion_vendor_hooks),\
	$(filter y,$(AXION_KERNEL_MODULES_BINDER_OBS_HOOKS)))
axion_task_tracepoints := $(filter y,$(AXION_KERNEL_MODULES_TASK_TRACEPOINTS))
axion_affinity_guard_flag := $(filter y,$(AXION_KERNEL_MODULES_AFFINITY_GUARD_HOOK))
axion_affinity_guard := $(and $(axion_vendor_hooks),$(axion_affinity_guard_flag))
axion_map_util_freq := $(and $(axion_vendor_hooks),\
	$(filter y,$(AXION_KERNEL_MODULES_MAP_UTIL_FREQ_HOOK)))
axion_boost := $(filter y,$(AXION_KERNEL_MODULES_AX_BOOST))
axion_atcm := $(filter y,$(AXION_KERNEL_MODULES_ATCM))
axion_burst_sched := $(axion_sched_rq)
axion_frame_boost := $(axion_sched_rq)
axion_game_boost := $(and $(axion_sched_rq),$(filter y,$(AXION_KERNEL_MODULES_GAME_BOOST)))
axion_burst_svp := $(and $(axion_burst_sched),\
	$(filter y,$(AXION_KERNEL_MODULES_SCHED_NOCHECK)))
axion_thread_snooper := $(axion_task_tracepoints)
