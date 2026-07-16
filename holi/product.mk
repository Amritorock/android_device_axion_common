ifeq ($(TARGET_SUPPORTS_KERNEL_MANAGER),true)
PRODUCT_COPY_FILES += \
    device/axion/common/prebuilts/ax_kernel_manager_holi.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/ax_kernel_manager.xml
PRODUCT_PACKAGES += \
    ax_init_holi
endif

PRODUCT_SOONG_NAMESPACES += device/axion/common/holi

TARGET_PRODUCT_PROP += device/axion/common/holi/props/ax_holi.prop

PRODUCT_COPY_FILES += \
    device/axion/common/prebuilts/ax_perf_thermal_holi.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/ax_perf_thermal.xml
