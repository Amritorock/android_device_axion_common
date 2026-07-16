TARGET_PRODUCT_PROP += \
    device/axion/common/config/defaults_common.prop


include device/axion/common/config/packages.mk

$(call soong_config_set_bool,lmkd,use_hooks,true)

# eliminate interpreter overhead
PRODUCT_DEXPREOPT_SPEED_APPS += \
    Settings \
    AxThemePicker \
    AxionParts \
    EdgeLauncher \
    GameSpace \
    AppLocker

TARGET_DISABLES_LIBPERF ?= false
TARGET_SHIPS_AXION_KERNEL_MODULES ?= false
TARGET_SUPPORTS_KERNEL_MANAGER ?= false

#vulkan-first
TARGET_USES_VULKAN := true

PRODUCT_COPY_FILES += \
	frameworks/native/data/etc/android.hardware.opengles.aep.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.opengles.aep.xml \
	frameworks/native/data/etc/android.hardware.vulkan.version-1_4.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.version.xml \
	frameworks/native/data/etc/android.hardware.vulkan.level-1.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.level.xml \
	frameworks/native/data/etc/android.hardware.vulkan.compute-0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.compute.xml \
	frameworks/native/data/etc/android.software.vulkan.deqp.level-2025-03-01.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.vulkan.deqp.level.xml \
	frameworks/native/data/etc/android.software.opengles.deqp.level-2025-03-01.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.opengles.deqp.level.xml
	
TARGET_NEEDS_VULKAN_MEDIA_FIX ?= false

PRODUCT_PRODUCT_PROPERTIES += \
    persist.sys.vk_use_ogl_for_media=$(TARGET_NEEDS_VULKAN_MEDIA_FIX)
