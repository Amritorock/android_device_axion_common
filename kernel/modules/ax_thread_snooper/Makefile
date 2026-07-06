include $(src)/../axion_modules.mk

ifeq ($(axion_thread_snooper),y)
obj-m += ax_thread_snooper.o
ccflags-y += -I$(abspath $(kbuild-dir)/../include)
endif
