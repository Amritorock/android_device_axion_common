PRODUCT_SOONG_NAMESPACES += device/axion/common/gs101

include device/axion/common/config/board/tensor.mk
include device/axion/common/config/board/common.mk

PRODUCT_COPY_FILES += \
    device/axion/common/init/axion_init_gs101.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.axion.rc
