include $(src)/../axion_modules.mk

ifeq ($(axion_atcm),y)
obj-m += ax_atcm.o
ccflags-y += -I$(abspath $(kbuild-dir)/../include)
atcm_has_proc_ops := $(shell grep -q "struct proc_ops" $(srctree)/include/linux/proc_fs.h 2>/dev/null && echo y)
ifeq ($(atcm_has_proc_ops),y)
ccflags-y += -DATCM_HAS_PROC_OPS
endif
ifeq ($(axion_boost),y)
ccflags-y += -DATCM_HAS_BOOST -DAX_SCHED_HAS_BOOST
KBUILD_EXTRA_SYMBOLS += $(wildcard $(abspath $(src)/../ax_boost/Module.symvers))
endif
endif
