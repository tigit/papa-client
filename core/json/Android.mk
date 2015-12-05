LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE    := json
LOCAL_SRC_FILES := fpconv.c lua_cjson.c strbuf.c
LOCAL_C_INCLUDES := $(JNI_PATH)/lua/

include $(BUILD_STATIC_LIBRARY)
