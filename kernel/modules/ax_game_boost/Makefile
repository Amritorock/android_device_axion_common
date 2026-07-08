include $(src)/../axion_modules.mk

ifeq ($(axion_game_boost),y)
obj-m += ax_game_boost.o
ccflags-y += -I$(abspath $(kbuild-dir)/../include)
ifeq ($(axion_boost),y)
ccflags-y += -DAX_SCHED_HAS_BOOST
KBUILD_EXTRA_SYMBOLS += $(wildcard $(abspath $(src)/../ax_boost/Module.symvers))
endif
endif
