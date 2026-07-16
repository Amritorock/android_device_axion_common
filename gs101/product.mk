ifeq ($(TARGET_SUPPORTS_KERNEL_MANAGER),true)
PRODUCT_COPY_FILES += \
    device/axion/common/prebuilts/ax_kernel_manager_gs101.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/ax_kernel_manager.xml
PRODUCT_PACKAGES += \
    ax_init_gs101
endif

PRODUCT_SOONG_NAMESPACES += device/axion/common/gs101

TARGET_PRODUCT_PROP += device/axion/common/gs101/props/ax_gs101.prop

include device/axion/common/config/product/tensor.mk

PRODUCT_COPY_FILES += \
    device/axion/common/prebuilts/ax_perf_thermal_gs101.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/ax_perf_thermal.xml
PRODUCT_COPY_FILES += \
    device/axion/common/init/axion_init_gs101.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.axion.rc
