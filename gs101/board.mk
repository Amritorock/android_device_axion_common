PRODUCT_SOONG_NAMESPACES += device/axion/common/gs101

include device/axion/common/config/board/tensor.mk

TARGET_SHIPS_AXION_KERNEL_MODULES := true
TARGET_SUPPORTS_KERNEL_MANAGER := true

ifeq ($(TARGET_SUPPORTS_KERNEL_MANAGER),true)
PRODUCT_COPY_FILES += \
    device/axion/common/prebuilts/ax_kernel_manager_gs101.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/ax_kernel_manager.xml
endif

TARGET_PRODUCT_PROP += \
    device/axion/common/gs101/props/ax_gs101.prop

PRODUCT_COPY_FILES += \
    device/axion/common/init/ax_init_gs101.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/ax_init_gs101.rc \
    device/axion/common/init/axion_init_gs101.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.axion.rc \
    device/axion/common/prebuilts/ax_perf_thermal_gs101.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/ax_perf_thermal.xml
