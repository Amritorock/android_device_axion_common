include $(src)/../axion_modules.mk

ifeq ($(axion_frame_boost),y)
obj-m += ax_frame_boost.o
ccflags-y += -I$(abspath $(kbuild-dir)/../include)
ifeq ($(axion_uclamp_eff),y)
ccflags-y += -DAX_FB_HAS_UCLAMP_EFF_GET
endif
ifeq ($(axion_map_util_freq),y)
ccflags-y += -DAX_FB_HAS_MAP_UTIL_FREQ
endif
ifeq ($(axion_boost),y)
ccflags-y += -DAX_SCHED_HAS_BOOST
KBUILD_EXTRA_SYMBOLS += $(wildcard $(abspath $(src)/../ax_boost/Module.symvers))
endif
endif
