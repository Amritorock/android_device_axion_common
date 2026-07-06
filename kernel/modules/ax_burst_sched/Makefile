include $(src)/../axion_modules.mk

ifeq ($(axion_burst_sched),y)
obj-m += ax_burst_sched.o
ccflags-y += -I$(abspath $(kbuild-dir)/../include)
ifeq ($(axion_uclamp_eff),y)
ccflags-y += -DAX_BS_HAS_UCLAMP_EFF_GET
endif
ifeq ($(axion_map_util_freq),y)
ccflags-y += -DAX_BS_HAS_MAP_UTIL_FREQ
endif
ifeq ($(axion_boost),y)
ccflags-y += -DAX_SCHED_HAS_BOOST
KBUILD_EXTRA_SYMBOLS += $(wildcard $(abspath $(src)/../ax_boost/Module.symvers))
endif
ifeq ($(axion_thread_snooper),y)
ccflags-y += -DAX_SCHED_HAS_THREAD_SNOOPER
KBUILD_EXTRA_SYMBOLS += $(wildcard $(abspath $(src)/../ax_thread_snooper/Module.symvers))
endif
ifeq ($(axion_frame_boost),y)
ccflags-y += -DAX_BS_HAS_FRAME_BOOST
KBUILD_EXTRA_SYMBOLS += $(wildcard $(abspath $(src)/../ax_frame_boost/Module.symvers))
endif
ifeq ($(axion_burst_svp),y)
ccflags-y += -DAX_BS_HAS_SVP_POLICY
endif
ifneq ($(wildcard $(srctree)/include/trace/hooks/rwsem.h),)
ccflags-y += -DAX_BS_HAS_RWSEM_HOOK
endif
endif
