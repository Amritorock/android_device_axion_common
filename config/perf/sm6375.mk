TARGET_DISABLES_LIBPERF := true
TARGET_SHIPS_AXION_KERNEL_MODULES := true
TARGET_SUPPORTS_KERNEL_MANAGER := true
TARGET_NEEDS_VULKAN_MEDIA_FIX := true

ifeq ($(TARGET_SUPPORTS_KERNEL_MANAGER),true)
PRODUCT_COPY_FILES += \
    device/axion/common/config/perf/ax_kernel_manager_sm6375.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/ax_kernel_manager.xml
endif
