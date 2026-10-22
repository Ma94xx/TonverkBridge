LOCAL_PATH := $(call my-dir)
MY_DIR := $(LOCAL_PATH)

# libusb (nutzt die mitgelieferte Android-Build-Datei)
include $(LOCAL_PATH)/libusb-1.0.27/android/jni/libusb.mk

# unser UAC2-Treiber
include $(CLEAR_VARS)
LOCAL_PATH := $(MY_DIR)
LOCAL_MODULE := usbaudio
LOCAL_SRC_FILES := usb_audio.c
LOCAL_C_INCLUDES := $(MY_DIR)/libusb-1.0.27/libusb
LOCAL_SHARED_LIBRARIES := libusb1.0
LOCAL_LDLIBS := -llog
include $(BUILD_SHARED_LIBRARY)
