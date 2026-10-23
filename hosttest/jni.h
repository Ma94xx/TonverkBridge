// Minimaler JNI-Stub für Host-Tests von usb_audio.c
#ifndef HOSTTEST_JNI_H
#define HOSTTEST_JNI_H
#include <stdint.h>

#define JNIEXPORT
#define JNICALL
#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006
#define JNI_ABORT 2

typedef int32_t jint;
typedef uint8_t jboolean;
typedef signed char jbyte;
typedef float jfloat;
typedef void *jobject;
typedef void *jclass;
typedef void *jstring;
typedef void *jbyteArray;
typedef void *jfloatArray;
typedef void *jmethodID;

struct JNINativeInterface;
struct JNIInvokeInterface;
typedef const struct JNINativeInterface *JNIEnv;
typedef const struct JNIInvokeInterface *JavaVM;

struct JNINativeInterface {
    jclass (*FindClass)(JNIEnv *, const char *);
    jobject (*NewGlobalRef)(JNIEnv *, jobject);
    jmethodID (*GetStaticMethodID)(JNIEnv *, jclass, const char *, const char *);
    jstring (*NewStringUTF)(JNIEnv *, const char *);
    void (*CallStaticVoidMethod)(JNIEnv *, jclass, jmethodID, ...);
    void (*DeleteLocalRef)(JNIEnv *, jobject);
    jbyte *(*GetByteArrayElements)(JNIEnv *, jbyteArray, jboolean *);
    void (*ReleaseByteArrayElements)(JNIEnv *, jbyteArray, jbyte *, jint);
    void (*SetByteArrayRegion)(JNIEnv *, jbyteArray, jint, jint, const jbyte *);
    jfloat *(*GetFloatArrayElements)(JNIEnv *, jfloatArray, jboolean *);
    void (*ReleaseFloatArrayElements)(JNIEnv *, jfloatArray, jfloat *, jint);
};

struct JNIInvokeInterface {
    jint (*GetEnv)(JavaVM *, void **, jint);
    jint (*AttachCurrentThread)(JavaVM *, JNIEnv **, void *);
    jint (*DetachCurrentThread)(JavaVM *);
};

#endif
