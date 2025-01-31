#ifndef WHALE_ANDROID_ART_NATIVE_ON_LOAD_H_
#define WHALE_ANDROID_ART_NATIVE_ON_LOAD_H_

#include <jni.h>

constexpr const char *kMethodReserved0 = "reserved0";
constexpr const char *kMethodReserved1 = "reserved1";

/**
 * DO NOT rename the following function
 */
extern "C" {

void WhaleRuntime_reserved0(JNIEnv *env, jclass cl);

void WhaleRuntime_reserved1(JNIEnv *env, jclass cl);

}

jint Whale_Init(JavaVM *vm, void *reserved);

#endif  // WHALE_ANDROID_ART_NATIVE_ON_LOAD_H_
