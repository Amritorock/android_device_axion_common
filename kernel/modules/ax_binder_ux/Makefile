include $(src)/../axion_modules.mk

ifeq ($(axion_binder_ux),y)
obj-m += ax_binder_ux.o
ccflags-y += -I$(srctree)/drivers/android
ccflags-y += -I$(abspath $(kbuild-dir)/../include)
ifeq ($(axion_burst_sched),y)
ccflags-y += -DAX_BINDER_HAS_BURST_SCENE
KBUILD_EXTRA_SYMBOLS += $(wildcard $(abspath $(src)/../ax_burst_sched/Module.symvers))
endif
ifeq ($(axion_frame_boost),y)
ccflags-y += -DAX_BINDER_HAS_FRAME_BOOST
KBUILD_EXTRA_SYMBOLS += $(wildcard $(abspath $(src)/../ax_frame_boost/Module.symvers))
endif
endif
