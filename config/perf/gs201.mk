ifeq ($(TARGET_SUPPORTS_KERNEL_MANAGER),true)
PRODUCT_COPY_FILES += \
    device/axion/common/config/perf/ax_kernel_manager_gs201.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/ax_kernel_manager.xml
endif
