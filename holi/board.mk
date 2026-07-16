PRODUCT_SOONG_NAMESPACES += device/axion/common/holi

TARGET_SHIPS_AXION_KERNEL_MODULES := true
TARGET_SUPPORTS_KERNEL_MANAGER := true
TARGET_NEEDS_VULKAN_MEDIA_FIX := true

ifeq ($(TARGET_SUPPORTS_KERNEL_MANAGER),true)
PRODUCT_COPY_FILES += \
    device/axion/common/prebuilts/ax_kernel_manager_holi.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/ax_kernel_manager.xml
endif

TARGET_PRODUCT_PROP += \
    device/axion/common/holi/props/ax_holi.prop

PRODUCT_COPY_FILES += \
    device/axion/common/init/ax_init_holi.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/ax_init_holi.rc \
    device/axion/common/prebuilts/ax_perf_thermal_holi.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/ax_perf_thermal.xml
