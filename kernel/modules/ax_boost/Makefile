include $(src)/../axion_modules.mk

ifeq ($(axion_boost),y)
obj-m += ax_boost.o
ccflags-y += -I$(abspath $(kbuild-dir)/../include)
ifneq ($(wildcard $(srctree)/include/linux/pm_qos.h),)
ccflags-y += -DBOOST_HAS_PMQOS
endif
boost_has_proc_ops := $(shell grep -q "struct proc_ops" $(srctree)/include/linux/proc_fs.h 2>/dev/null && echo y)
boost_has_devfreq_provider := $(shell grep -q "devfreq_get_by_name" $(srctree)/include/linux/devfreq.h 2>/dev/null && echo y)
boost_has_devfreq_min_helper := $(shell grep -q "devfreq_get_min_freq" $(srctree)/include/linux/devfreq.h 2>/dev/null && echo y)
boost_has_qcom_holi_icc_provider := $(if $(wildcard $(srctree)/include/linux/interconnect.h),$(if $(wildcard $(srctree)/include/dt-bindings/interconnect/qcom,holi.h),y))
provider_enabled :=
ifeq ($(boost_has_proc_ops),y)
ccflags-y += -DBOOST_HAS_PROC_OPS
endif
ifeq ($(AXION_KERNEL_MODULES_AX_BOOST_GPU_PROVIDER),y)
ifeq ($(boost_has_devfreq_provider),y)
ccflags-y += -DAX_BOOST_HAS_GPU_PROVIDER
provider_enabled := y
endif
endif
ifeq ($(AXION_KERNEL_MODULES_AX_BOOST_MEMLAT_PROVIDER),y)
ifeq ($(boost_has_devfreq_provider),y)
ccflags-y += -DAX_BOOST_HAS_MEMLAT_PROVIDER
provider_enabled := y
endif
endif
ifeq ($(AXION_KERNEL_MODULES_AX_BOOST_DDR_PROVIDER),y)
ifeq ($(boost_has_devfreq_provider),y)
ccflags-y += -DAX_BOOST_HAS_DDR_PROVIDER
provider_enabled := y
endif
endif
ifeq ($(AXION_KERNEL_MODULES_AX_BOOST_L3_PROVIDER),y)
ifeq ($(boost_has_devfreq_provider),y)
ccflags-y += -DAX_BOOST_HAS_L3_PROVIDER
provider_enabled := y
endif
endif
ifeq ($(provider_enabled),y)
ccflags-y += -DAX_BOOST_HAS_DEVFREQ_PROVIDER
ifeq ($(boost_has_devfreq_min_helper),y)
ccflags-y += -DAX_BOOST_HAS_DEVFREQ_MIN_HELPER
endif
ifeq ($(boost_has_qcom_holi_icc_provider),y)
ccflags-y += -DAX_BOOST_HAS_QCOM_HOLI_ICC_PROVIDER
endif
endif
endif
