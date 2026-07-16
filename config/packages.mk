$(call inherit-product-if-exists, axion_sdk/ax_tflite/common.mk)

PRODUCT_PACKAGES += \
    AxThemePicker \
    AxQuickLook \
    AxionWidgets \
    AxionParts \
    AxThemeStore \
    AxPcMode \
    AxSandbox \
    EdgeLauncher \
    GameSpace \
    OmniJaws \
    ColumbusService \
    AxDiagnostics \
    ax_ram_plus_setup

TARGET_INCLUDE_AXFX ?= false
ifeq ($(TARGET_INCLUDE_AXFX),true)
$(call inherit-product-if-exists, packages/apps/AxionFx/config.mk)
endif

$(call inherit-product-if-exists, packages/apps/FaceUnlock/common.mk)
