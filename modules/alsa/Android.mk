# hardware/ti/omap3/modules/alsa/Android.mk
#
# Copyright 2009-2010 Texas Instruments
#

# This is the OMAP3 ALSA module for OMAP3

ifeq ($(strip $(BOARD_USES_ALSA_AUDIO)),true)

  LOCAL_PATH := $(call my-dir)

  include $(CLEAR_VARS)

  LOCAL_PRELINK_MODULE := false

  LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw

  LOCAL_CFLAGS := -D_POSIX_SOURCE -Wno-multichar
  ifeq ($(strip $(BOARD_USES_TI_OMAP3_MODEM_AUDIO)),true)
    LOCAL_CFLAGS += -DAUDIO_MODEM_TI
    # HACK flag below ignore error reported by the pcm start
    LOCAL_CFLAGS += -DAUDIO_MODEM_HACK_IGNORE_PCM_START_ERROR
  endif
  ifeq ($(BOARD_HAVE_BLUETOOTH),true)
      LOCAL_CFLAGS += -DAUDIO_BLUETOOTH
  endif

  LOCAL_C_INCLUDES += hardware/alsa_sound external/alsa-lib/include
  ifeq ($(strip $(BOARD_USES_TI_OMAP3_MODEM_AUDIO)),true)
    LOCAL_C_INCLUDES += hardware/ti/omap3/modules/alsa
    ifeq ($(BOARD_HAVE_BLUETOOTH),true)
        LOCAL_C_INCLUDES += $(call include-path-for, bluez-libs) \
                            external/bluetooth/bluez/include
    endif
  endif

  ifeq ($(strip $(TARGET_BOARD_PLATFORM)), omap3)
    LOCAL_SRC_FILES:= alsa_omap3.cpp
  endif
  ifeq ($(strip $(TARGET_BOARD_PLATFORM)), omap4)
    LOCAL_SRC_FILES:= alsa_omap4.cpp
  endif

  ifeq ($(strip $(BOARD_USES_TI_OMAP3_MODEM_AUDIO)),true)
    LOCAL_SRC_FILES += alsa_omap3_modem.cpp
  endif

  LOCAL_SHARED_LIBRARIES := \
    libaudio \
    libasound \
    liblog \
    libcutils \
    libutils \
    libdl

  ifeq ($(strip $(BOARD_USES_TI_OMAP3_MODEM_AUDIO)),true)
    ifeq ($(strip $(BOARD_HAVE_BLUETOOTH)),true)
        LOCAL_SHARED_LIBRARIES += \
            libbluetooth

        #LOCAL_STATIC_LIBRARIES := \
         #   libbluez-common-static
    endif
  endif

  LOCAL_MODULE:= alsa.$(TARGET_BOARD_PLATFORM)

  include $(BUILD_SHARED_LIBRARY)

endif
