LOCAL_PATH := $(call my-dir)

# HAL module implementation, not prelinked and stored in
# hw/<OVERLAY_HARDWARE_MODULE_ID>.<ro.product.board>.so
include $(CLEAR_VARS)
LOCAL_PRELINK_MODULE := false
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_SHARED_LIBRARIES := liblog libcutils

LOCAL_SRC_FILES := 	\
	allocator.cpp 	\
	gralloc.cpp 	\
	framebuffer.cpp \
	mapper.cpp

LOCAL_MODULE := gralloc.$(TARGET_BOARD_PLATFORM)

LOCAL_CFLAGS:= -DLOG_TAG=\"gralloc\"
include $(BUILD_SHARED_LIBRARY)
