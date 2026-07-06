include $(src)/../axion_modules.mk

ag_has_setaffinity_early := $(shell \
	grep -q "android_vh_sched_setaffinity_early" \
	$(srctree)/include/trace/hooks/sched.h 2>/dev/null && echo y)

ifeq ($(axion_affinity_guard),y)
ifeq ($(ag_has_setaffinity_early),y)
obj-m += ax_affinity_guard.o
ccflags-y += -I$(abspath $(kbuild-dir)/../include)
endif
endif
