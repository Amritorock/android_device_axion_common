TARGET_SHIPS_AXION_KERNEL_MODULES ?= false
ifeq ($(TARGET_SHIPS_AXION_KERNEL_MODULES),true)
TARGET_DISABLES_LIBPERF := true
endif

PRODUCT_COPY_FILES += \
    device/axion/common/init/init.axion.modules.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.axion.modules.rc
