TARGET_SHIPS_AXION_KERNEL_MODULES := true
TARGET_SUPPORTS_KERNEL_MANAGER := true
TARGET_DISABLES_LIBPERF := true

ifeq ($(TARGET_SUPPORTS_KERNEL_MANAGER),true)
PRODUCT_COPY_FILES += \
    device/axion/common/prebuilts/ax_kernel_manager_$(AXION_SOC).xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/ax_kernel_manager.xml
endif

AXION_GEN_PROP := $(OUT_DIR)/axion/axion_build.props.prop
$(shell python3 device/axion/common/build/gen_axion_props.py $(AXION_GEN_PROP) \
  persist.sys.ax_chg_bypass=$(BYPASS_CHARGE_SUPPORTED) \
  persist.sys.ax_hbm_supp=$(HBM_SUPPORTED) \
  persist.sys.ax_hbm_file=$(HBM_NODE) \
  persist.sys.ax_ims_ovrrde=$(TARGET_ENABLES_IMS_OVERRIDES) \
  persist.sys.ax_touch_boost=$(TARGET_TOUCH_BOOST_SUPPORTED) \
  persist.sys.ax_doze_tap=$(TARGET_DOZE_TAP_PULSE_SUPPORTED) \
  persist.sys.ax_doze_dt2p=$(TARGET_DOZE_DOUBLE_TAP_PULSE_SUPPORTED) \
  persist.sys.ax_doze_pickup=$(TARGET_DOZE_PICKUP_PULSE_SUPPORTED) \
  persist.sys.ax_doze_fps=$(TARGET_DOZE_SIDE_FPS_PULSE_SUPPORTED) \
  persist.sys.vk_use_ogl_for_media=$(TARGET_NEEDS_VULKAN_MEDIA_FIX) \
  persist.sys.ax_disable_pwrhal=$(TARGET_DISABLES_LIBPERF))

TARGET_PRODUCT_PROP += \
    device/axion/common/platform/$(AXION_SOC)/props/ax_$(AXION_SOC).prop \
    $(AXION_GEN_PROP)

PRODUCT_COPY_FILES += \
    device/axion/common/init/init.axion.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.axion-common.rc \
    device/axion/common/init/init.axion.modules.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.axion.modules.rc \
    device/axion/common/init/ax_init_$(AXION_SOC).rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/ax_init_$(AXION_SOC).rc \
    device/axion/common/prebuilts/ax_perf_thermal_$(AXION_SOC).xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/ax_perf_thermal.xml
