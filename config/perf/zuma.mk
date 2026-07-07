TARGET_DISABLES_LIBPERF := true
TARGET_SHIPS_AXION_KERNEL_MODULES := true
TARGET_SUPPORTS_KERNEL_MANAGER := true

ifeq ($(TARGET_SUPPORTS_KERNEL_MANAGER),true)
PRODUCT_COPY_FILES += \
    device/axion/common/config/perf/ax_kernel_manager_zuma.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/ax_kernel_manager.xml \
    device/axion/common/config/perf/ax_init_zuma.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/ax_init_zuma.rc
endif
