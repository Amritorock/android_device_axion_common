ifeq ($(TARGET_SUPPORTS_KERNEL_MANAGER),true)
PRODUCT_COPY_FILES += \
    device/axion/common/prebuilts/ax_kernel_manager_zuma.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/ax_kernel_manager.xml
PRODUCT_PACKAGES += \
    ax_init_zuma
endif

PRODUCT_SOONG_NAMESPACES += device/axion/common/zuma

TARGET_PRODUCT_PROP += device/axion/common/zuma/props/ax_zuma.prop

include device/axion/common/config/product/tensor.mk

PRODUCT_COPY_FILES += \
    device/axion/common/prebuilts/ax_perf_thermal_zuma.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/ax_perf_thermal.xml
PRODUCT_COPY_FILES += \
    device/axion/common/init/axion_init_zuma.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.axion.rc
