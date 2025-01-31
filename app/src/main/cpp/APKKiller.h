#include <stdio.h>
#include <iostream>
#include <vector>
#include <jni.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dlfcn.h>

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>

#include <whale.h>
#include "ElfImg.h"

#include "Utils.h"
#include "BinaryReader.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "APKKiller", __VA_ARGS__)

#define apk_asset_path "original.apk" // assets/original.apk
#define apk_fake_name "original.apk" // /data/data/<package_name/cache/original.apk
std::vector<std::vector<uint8_t>> apk_signatures{{}}; // if you fill this, it will ignore the m_APKSign from APKKiller.java

namespace APKKiller {
    JNIEnv *g_env;
    jstring g_apkPath;
    jobject g_packageManager;
    jobject g_proxy;
    std::string g_apkPkg;

    class Reference {
    public:
        jobject reference;
    public:
        Reference(jobject reference) {
            this->reference = g_env->NewGlobalRef(reference);
        }

        jobject get() {
            auto referenceClass = g_env->FindClass("java/lang/ref/Reference");
            auto get = g_env->GetMethodID(referenceClass, "get", "()Ljava/lang/Object;");
            auto result = g_env->CallObjectMethod(reference, get);
            g_env->DeleteLocalRef(referenceClass);
            return result;
        }
    };

    class WeakReference : public Reference {
    public:
        WeakReference(jobject weakReference) : Reference(weakReference) {
        }

        static jobject Create(jobject obj) {
            auto weakReferenceClass = g_env->FindClass("java/lang/ref/WeakReference");
            auto weakReferenceClassConstructor = g_env->GetMethodID(weakReferenceClass, "<init>", "(Ljava/lang/Object;)V");
            auto result = g_env->NewObject(weakReferenceClass, weakReferenceClassConstructor, obj);
            g_env->DeleteLocalRef(weakReferenceClass);
            return result;
        }
    };

    class ArrayList {
    private:
        jobject arrayList;
    public:
        ArrayList(jobject arrayList) {
            this->arrayList = g_env->NewGlobalRef(arrayList);
        }

        jobject getObj() {
            return arrayList;
        }

        jobject get(int index) {
            auto arrayListClass = g_env->FindClass("java/util/ArrayList");
            auto getMethod = g_env->GetMethodID(arrayListClass, "get", "(I)Ljava/lang/Object;");
            auto result = g_env->NewGlobalRef(g_env->CallObjectMethod(arrayList, getMethod, index));
            g_env->DeleteLocalRef(arrayListClass);
            return result;
        }

        void set(int index, jobject value) {
            auto arrayListClass = g_env->FindClass("java/util/ArrayList");
            auto setMethod = g_env->GetMethodID(arrayListClass, "set", "(ILjava/lang/Object;)Ljava/lang/Object;");
            g_env->CallObjectMethod(arrayList, setMethod, index, value);
            g_env->DeleteLocalRef(arrayListClass);
        }

        int size() {
            auto arrayListClass = g_env->FindClass("java/util/ArrayList");
            auto sizeMethod = g_env->GetMethodID(arrayListClass, "size", "()I");
            auto result = g_env->CallIntMethod(arrayList, sizeMethod);
            g_env->DeleteLocalRef(arrayListClass);
            return result;
        }
    };

    class ArrayMap {
    private:
        jobject arrayMap;
    public:
        ArrayMap(jobject arrayMap) {
            this->arrayMap = g_env->NewGlobalRef(arrayMap);
        }

        jobject getObj() {
            return arrayMap;
        }

        jobject valueAt(int index) {
            auto arrayMapClass = g_env->FindClass("android/util/ArrayMap");
            auto valueAtMethod = g_env->GetMethodID(arrayMapClass, "valueAt", "(I)Ljava/lang/Object;");
            auto result = g_env->CallObjectMethod(arrayMap, valueAtMethod, index);
            g_env->DeleteLocalRef(arrayMapClass);
            return result;
        }

        jobject setValueAt(int index, jobject value) {
            auto arrayMapClass = g_env->FindClass("android/util/ArrayMap");
            auto setValueAtMethod = g_env->GetMethodID(arrayMapClass, "setValueAt", "(ILjava/lang/Object;)Ljava/lang/Object;");
            auto result = g_env->CallObjectMethod(arrayMap, setValueAtMethod, index, value);
            g_env->DeleteLocalRef(arrayMapClass);
            return result;
        }

        int size() {
            auto arrayMapClass = g_env->FindClass("android/util/ArrayMap");
            auto sizeMethod = g_env->GetMethodID(arrayMapClass, "size", "()I");
            auto result = g_env->CallIntMethod(arrayMap, sizeMethod);
            return result;
        }
    };

    class Field {
    private:
        jclass clazz;
        jfieldID field;
        bool isStatic;
    public:
        Field(jclass clazz, jfieldID field, bool isStatic) {
            this->clazz = clazz;
            this->field = field;
            this->isStatic = isStatic;
        }
        ~Field() {
            g_env->DeleteLocalRef(clazz);
        }

        jobject get(jobject obj) {
            if (isStatic) {
                return g_env->GetStaticObjectField(clazz, field);
            } else {
                return g_env->GetObjectField(obj, field);
            }
        }

        void set(jobject obj, jobject value) {
            if (isStatic) {
                g_env->SetStaticObjectField(clazz, field, value);
            } else {
                g_env->SetObjectField(obj, field, value);
            }
        }
    };

    // This is for Java Reflection Method, so getMethod and getStaticMethod in Class class is not used here.
    class Method {
    private:
        jobject method;
    public:
        Method(jobject method) {
            this->method = method;
        }
        ~Method() {
            g_env->DeleteLocalRef(method);
        }

        void Cleanup() {
            this->~Method();
        }

        const char *getName() {
            auto methodClass = g_env->FindClass("java/lang/reflect/Method");
            auto getNameMethod = g_env->GetMethodID(methodClass, "getName", "()Ljava/lang/String;");
            auto methodName = g_env->CallObjectMethod(method, getNameMethod);
            auto result = g_env->GetStringUTFChars((jstring) methodName, 0);
            g_env->DeleteLocalRef(methodName);
            g_env->DeleteLocalRef(methodClass);
            return result;
        }

        jobject invoke(jobject object, jobjectArray args = 0) {
            auto methodClass = g_env->FindClass("java/lang/reflect/Method");
            auto invokeMethod = g_env->GetMethodID(methodClass, "invoke", "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
            auto result = g_env->NewGlobalRef(g_env->CallObjectMethod(method, invokeMethod, object, args));
            g_env->DeleteLocalRef(methodClass);
            return result;
        }
    };

    class Class {
    private:
        jclass clazz;
    public:
        Class(const char *className) {
            clazz = g_env->FindClass(className);
        }

        ~Class() {
            g_env->DeleteLocalRef(clazz);
        }

        void Cleanup() {
            this->~Class();
        }

        jclass getClass() {
            return clazz;
        }

        Field *getField(const char *fieldName, const char *fieldSig) {
            auto field = g_env->GetFieldID(clazz, fieldName, fieldSig);
            if (g_env->ExceptionCheck()) {
                g_env->ExceptionDescribe();
                g_env->ExceptionClear();
            }
            LOGI("Field %s(%s): %p", fieldName, fieldSig, field);
            return new Field(clazz, field, false);
        }

        Field *getStaticField(const char *fieldName, const char *fieldSig) {
            auto field = g_env->GetStaticFieldID(clazz, fieldName, fieldSig);
            if (g_env->ExceptionCheck()) {
                g_env->ExceptionDescribe();
                g_env->ExceptionClear();
            }
            LOGI("Field %s(%s): %p", fieldName, fieldSig, field);
            return new Field(clazz, field, true);
        }

        jmethodID getMethod(const char *methodName, const char *methodSig) {
            auto method = g_env->GetMethodID(clazz, methodName, methodSig);
            if (g_env->ExceptionCheck()) {
                g_env->ExceptionDescribe();
                g_env->ExceptionClear();
            }
            LOGI("Method %s(%s): %p", methodName, methodSig, method);
            return method;
        }

        jmethodID getStaticMethod(const char *methodName, const char *methodSig) {
            auto method = g_env->GetStaticMethodID(clazz, methodName, methodSig);
            if (g_env->ExceptionCheck()) {
                g_env->ExceptionDescribe();
                g_env->ExceptionClear();
            }
            LOGI("Method %s(%s): %p", methodName, methodSig, method);
            return method;
        }
    };
}

using namespace APKKiller;

jobject getApplicationContext(jobject obj) {
    auto contextWrapperClass = g_env->FindClass("android/content/ContextWrapper");
    auto getApplicationContextMethod = g_env->GetMethodID(contextWrapperClass, "getApplicationContext", "()Landroid/content/Context;");
    return g_env->CallObjectMethod(obj, getApplicationContextMethod);
}

jobject getPackageManager(jobject obj) {
    auto contextClass = g_env->FindClass("android/content/Context");
    auto getPackageManagerMethod = g_env->GetMethodID(contextClass, "getPackageManager", "()Landroid/content/pm/PackageManager;");
    return g_env->CallObjectMethod(obj, getPackageManagerMethod);
}

std::string getPackageName(jobject obj) {
    auto contextClass = g_env->FindClass("android/content/Context");
    auto getPackageNameMethod = g_env->GetMethodID(contextClass, "getPackageName", "()Ljava/lang/String;");
    auto packageName = (jstring) g_env->CallObjectMethod(obj, getPackageNameMethod);
    return g_env->GetStringUTFChars(packageName, 0);
}


void patch_ApplicationInfo(jobject obj) {
    if (!obj) return;
    LOGI("-------- Patching ApplicationInfo - %p", obj);
    auto applicationInfoClass = new Class("android/content/pm/ApplicationInfo");

    auto sourceDirField = applicationInfoClass->getField("sourceDir", "Ljava/lang/String;");
    auto publicSourceDirField = applicationInfoClass->getField("publicSourceDir", "Ljava/lang/String;");

    sourceDirField->set(obj, g_apkPath);
    publicSourceDirField->set(obj, g_apkPath);
}

void patch_LoadedApk(jobject obj) {
    if (!obj) return;
    LOGI("-------- Patching LoadedApk - %p", obj);
    auto loadedApkClass = new Class("android/app/LoadedApk");

    auto mApplicationInfoField = loadedApkClass->getField("mApplicationInfo", "Landroid/content/pm/ApplicationInfo;");
    patch_ApplicationInfo(mApplicationInfoField->get(obj));

    auto mAppDirField = loadedApkClass->getField("mAppDir", "Ljava/lang/String;");
    auto mResDirField = loadedApkClass->getField("mResDir", "Ljava/lang/String;");

    mAppDirField->set(obj, g_apkPath);
    mResDirField->set(obj, g_apkPath);
}

void patch_AppBindData(jobject obj) {
    if (!obj) return;
    LOGI("-------- Patching AppBindData - %p", obj);
    auto appBindDataClass = new Class("android/app/ActivityThread$AppBindData");

    auto infoField = appBindDataClass->getField("info", "Landroid/app/LoadedApk;");
    patch_LoadedApk(infoField->get(obj));

    auto appInfoField = appBindDataClass->getField("appInfo", "Landroid/content/pm/ApplicationInfo;");
    patch_ApplicationInfo(appInfoField->get(obj));
}

void patch_PackageInfo(jobject obj) {
    if (!obj) return;
    LOGI("-------- Patching PackageInfo - %p", obj);
    auto packageInfoClass = new Class("android/content/pm/PackageInfo");

    auto applicationInfoField = packageInfoClass->getField("applicationInfo", "Landroid/content/pm/ApplicationInfo;");
    patch_ApplicationInfo(applicationInfoField->get(obj));
}

void patch_ContextImpl(jobject obj) {
    if (!obj) return;
    auto contextImplClass = new Class("android/app/ContextImpl");
    if (g_env->IsInstanceOf(obj, contextImplClass->getClass())) {
        LOGI("-------- Patching ContextImpl - %p", obj);
        auto mPackageInfoField = contextImplClass->getField("mPackageInfo", "Landroid/app/LoadedApk;");
        patch_PackageInfo(mPackageInfoField->get(obj));
        auto mPackageManagerField = contextImplClass->getField("mPackageManager", "Landroid/content/pm/PackageManager;");
        mPackageManagerField->set(obj, g_proxy);
    }
}

void patch_Application(jobject obj) {
    if (!obj) return;
    auto applicationClass = new Class("android/app/Application");
    if (g_env->IsInstanceOf(obj, applicationClass->getClass())) {
        LOGI("-------- Patching Application - %p", obj);
        auto mLoadedApkField = applicationClass->getField("mLoadedApk", "Landroid/app/LoadedApk;");
        patch_LoadedApk(mLoadedApkField->get(obj));
    }

    patch_ContextImpl(getApplicationContext(obj));
}

AAssetManager *g_assetManager;

void extractAsset(std::string assetName, std::string extractPath) {
    LOGI("-------- Extracting %s to %s", assetName.c_str(), extractPath.c_str());
    AAssetManager *assetManager = g_assetManager;
    AAsset *asset = AAssetManager_open(assetManager, assetName.c_str(), AASSET_MODE_UNKNOWN);
    if (!asset) {
        return;
    }

    int fd = open(extractPath.c_str(), O_CREAT | O_WRONLY, 0644);
    if (fd < 0) {
        AAsset_close(asset);
        return;
    }

    const int BUFFER_SIZE = 512;
    char buffer[BUFFER_SIZE];
    int bytesRead;
    while ((bytesRead = AAsset_read(asset, buffer, BUFFER_SIZE)) > 0) {
        int bytesWritten = write(fd, buffer, bytesRead);
        if (bytesWritten != bytesRead) {
            AAsset_close(asset);
            close(fd);
            return;
        }
    }

    AAsset_close(asset);
    close(fd);
}

void patch_PackageManager(jobject obj) {
    if (!obj) return;

    auto activityThreadClass = new Class("android/app/ActivityThread");
    auto sCurrentActivityThreadField = activityThreadClass->getStaticField("sCurrentActivityThread", "Landroid/app/ActivityThread;");
    auto sCurrentActivityThread = sCurrentActivityThreadField->get(NULL);

    auto sPackageManagerField = activityThreadClass->getStaticField("sPackageManager", "Landroid/content/pm/IPackageManager;");
    g_packageManager = g_env->NewGlobalRef(sPackageManagerField->get(NULL));

    auto iPackageManagerClass = new Class("android/content/pm/IPackageManager");

    auto classClass = g_env->FindClass("java/lang/Class");
    auto getClassLoaderMethod = g_env->GetMethodID(classClass, "getClassLoader", "()Ljava/lang/ClassLoader;");

    auto classLoader = g_env->CallObjectMethod(iPackageManagerClass->getClass(), getClassLoaderMethod);
    auto classArray = g_env->NewObjectArray(1, classClass, NULL);
    g_env->SetObjectArrayElement(classArray, 0, iPackageManagerClass->getClass());

    auto apkKillerClass = g_env->FindClass("com/kuro/APKKiller");
    auto myInvocationHandlerField = g_env->GetStaticFieldID(apkKillerClass, "myInvocationHandler", "Ljava/lang/reflect/InvocationHandler;");
    auto myInvocationHandler = g_env->GetStaticObjectField(apkKillerClass, myInvocationHandlerField);

    auto proxyClass = g_env->FindClass("java/lang/reflect/Proxy");
    auto newProxyInstanceMethod = g_env->GetStaticMethodID(proxyClass, "newProxyInstance", "(Ljava/lang/ClassLoader;[Ljava/lang/Class;Ljava/lang/reflect/InvocationHandler;)Ljava/lang/Object;");
    g_proxy = g_env->NewGlobalRef(g_env->CallStaticObjectMethod(proxyClass, newProxyInstanceMethod, classLoader, classArray, myInvocationHandler));

    sPackageManagerField->set(sCurrentActivityThread, g_proxy);

    auto pm = getPackageManager(obj);
    auto mPMField = (new Class("android/app/ApplicationPackageManager"))->getField("mPM", "Landroid/content/pm/IPackageManager;");
    mPMField->set(pm, g_proxy);
}

void doBypass(JNIEnv *env) {
    ElfImg art("libart.so");
    auto setHiddenApiExemptionsMethod = (void (*)(JNIEnv *, jobject, jobjectArray)) art.getSymbolAddress("_ZN3artL32VMRuntime_setHiddenApiExemptionsEP7_JNIEnvP7_jclassP13_jobjectArray");
    if (setHiddenApiExemptionsMethod != nullptr) {
        auto objectClass = env->FindClass("java/lang/Object");
        auto objectArray = env->NewObjectArray(1, objectClass, NULL);
        env->SetObjectArrayElement(objectArray, 0, env->NewStringUTF("L"));

        auto VMRuntimeClass = env->FindClass("dalvik/system/VMRuntime");
        auto getRuntimeMethod = env->GetStaticMethodID(VMRuntimeClass, "getRuntime", "()Ldalvik/system/VMRuntime;");
        auto runtime = env->CallStaticObjectMethod(VMRuntimeClass, getRuntimeMethod);
        // setHiddenApiExemptionsMethod(env, runtime, objectArray);
    }
}

void APKKill(JNIEnv *env, jclass clazz, jobject context) {
    LOGI("-------- Killing APK");

    APKKiller::g_env = env;
    g_assetManager = AAssetManager_fromJava(env, env->CallObjectMethod(context, env->GetMethodID(env->FindClass("android/content/Context"), "getAssets", "()Landroid/content/res/AssetManager;")));

    std::string apkPkg = getPackageName(context);
    APKKiller::g_apkPkg = apkPkg;

    LOGI("-------- Killing %s", apkPkg.c_str());

    char apkDir[512];
    sprintf(apkDir, "/data/data/%s/cache", apkPkg.c_str());
    mkdir(apkDir, 0777);

    std::string apkPath = "/data/data/";
    apkPath += apkPkg;
    apkPath += "/cache/";
    apkPath += apk_fake_name;

    if (access(apkPath.c_str(), F_OK) == -1) {
        extractAsset(apk_asset_path, apkPath);
    }

    APKKiller::g_apkPath = (jstring) env->NewGlobalRef(g_env->NewStringUTF(apkPath.c_str()));

    doBypass(env);

    auto apkKillerClass = g_env->FindClass("com/kuro/APKKiller");
    auto javaKillerMethod = g_env->GetStaticMethodID(apkKillerClass, "Kill", "()V");
    g_env->CallStaticVoidMethod(apkKillerClass, javaKillerMethod);

    if (apk_signatures.empty()) {
        auto m_APKSignField = g_env->GetStaticFieldID(apkKillerClass, "m_APKSign", "Ljava/lang/String;");
        auto m_APKSign = g_env->GetStringUTFChars((jstring) g_env->GetStaticObjectField(apkKillerClass, m_APKSignField), NULL);
        {
            auto signs = base64_decode(m_APKSign);
            BinaryReader reader(signs.data(), signs.size());
            apk_signatures.resize((int) reader.readInt8());
            for (int i = 0; i < apk_signatures.size(); i++) {
                size_t size = reader.readInt32(), n;
                apk_signatures[i].resize(size);

                uint8_t *sign = new uint8_t[size];
                if ((n = reader.read(sign, size)) > 0) {
                    memcpy(apk_signatures[i].data(), sign, n);
                }
            }
        }
    }

    auto activityThreadClass = new Class("android/app/ActivityThread");
    auto sCurrentActivityThreadField = activityThreadClass->getStaticField("sCurrentActivityThread", "Landroid/app/ActivityThread;");
    auto sCurrentActivityThread = sCurrentActivityThreadField->get(NULL);

    auto mBoundApplicationField = activityThreadClass->getField("mBoundApplication", "Landroid/app/ActivityThread$AppBindData;");
    patch_AppBindData(mBoundApplicationField->get(sCurrentActivityThread));

    auto mInitialApplicationField = activityThreadClass->getField("mInitialApplication", "Landroid/app/Application;");
    patch_Application(mInitialApplicationField->get(sCurrentActivityThread));

    auto mAllApplicationsField = activityThreadClass->getField("mAllApplications", "Ljava/util/ArrayList;");
    auto mAllApplications = mAllApplicationsField->get(sCurrentActivityThread);
    ArrayList *list = new ArrayList(mAllApplications);
    for (int i = 0; i < list->size(); i++) {
        auto application = list->get(i);
        patch_Application(application);
        list->set(i, application);
    }
    mAllApplicationsField->set(sCurrentActivityThread, list->getObj());

    auto mPackagesField = activityThreadClass->getField("mPackages", "Landroid/util/ArrayMap;");
    auto mPackages = mPackagesField->get(sCurrentActivityThread);
    ArrayMap *map = new ArrayMap(mPackages);
    for (int i = 0; i < map->size(); i++) {
        auto loadedApk = new WeakReference(map->valueAt(i));
        patch_LoadedApk(loadedApk->get());
        map->setValueAt(i, WeakReference::Create(loadedApk->get()));
    }
    mPackagesField->set(sCurrentActivityThread, map->getObj());

    auto mResourcePackagesField = activityThreadClass->getField("mResourcePackages", "Landroid/util/ArrayMap;");
    auto mResourcePackages = mResourcePackagesField->get(sCurrentActivityThread);
    map = new ArrayMap(mResourcePackages);
    for (int i = 0; i < map->size(); i++) {
        auto loadedApk = new WeakReference(map->valueAt(i));
        patch_LoadedApk(loadedApk->get());
        map->setValueAt(i, WeakReference::Create(loadedApk->get()));
    }
    mResourcePackagesField->set(sCurrentActivityThread, map->getObj());

    patch_ContextImpl(context);
    patch_PackageManager(context);
}

jobject processInvoke(JNIEnv *env, jclass clazz, jobject method, jobjectArray args) {
    g_env = env;

    auto Integer_intValue = [env](jobject integer) {
        auto integerClass = env->FindClass("java/lang/Integer");
        auto intValueMethod = env->GetMethodID(integerClass, "intValue", "()I");
        auto result = env->CallIntMethod(integer, intValueMethod);
        env->DeleteLocalRef(integerClass);
        return result;
    };

    auto String_fromParam = [env, args](int idx) -> const char * {
        auto param = env->GetObjectArrayElement(args, idx);
        if (!param) {
            return 0;
        }
        auto result = env->GetStringUTFChars((jstring) param, NULL);
        env->DeleteLocalRef(param);
        return result;
    };

    auto Integer_fromParam = [env, args, Integer_intValue](int idx) -> int {
        auto param = env->GetObjectArrayElement(args, idx);
        if (!param) {
            return 0;
        }
        auto result = Integer_intValue(param);
        env->DeleteLocalRef(param);
        return result;
    };

    auto mMethod = new Method(method);
    const char *mName = mMethod->getName();
    auto mResult = mMethod->invoke(g_packageManager, args);
    mMethod->Cleanup();

    if (!strcmp(mName, "getPackageInfo")) {
        const char *packageName = String_fromParam(0);
        int flags = Integer_fromParam(1);
        if (!strcmp(packageName, g_apkPkg.c_str())) {
            if ((flags & 0x40) != 0) {
                auto packageInfo = mResult;
                if (packageInfo) {
                    auto packageInfoClass = new Class("android/content/pm/PackageInfo");
                    auto applicationInfoField = packageInfoClass->getField("applicationInfo", "Landroid/content/pm/ApplicationInfo;");
                    auto applicationInfo = applicationInfoField->get(packageInfo);
                    if (applicationInfo) {
                        patch_ApplicationInfo(applicationInfo);
                    }
                    applicationInfoField->set(packageInfo, applicationInfo);
                    auto signaturesField = packageInfoClass->getField("signatures", "[Landroid/content/pm/Signature;");

                    auto signatureClass = env->FindClass("android/content/pm/Signature");
                    auto signatureConstructor = env->GetMethodID(signatureClass, "<init>", "([B)V");
                    auto signatureArray = env->NewObjectArray(apk_signatures.size(), signatureClass, NULL);
                    for (int i = 0; i < apk_signatures.size(); i++) {
                        auto signature = env->NewByteArray(apk_signatures[i].size());
                        env->SetByteArrayRegion(signature, 0, apk_signatures[i].size(), (jbyte *) apk_signatures[i].data());
                        env->SetObjectArrayElement(signatureArray, i, env->NewObject(signatureClass, signatureConstructor, signature));
                    }
                    signaturesField->set(packageInfo, signatureArray);

                    packageInfoClass->Cleanup();
                }
                return packageInfo;
            } else if ((flags & 0x8000000) != 0) {
                auto packageInfo = mResult;
                if (packageInfo) {
                    auto packageInfoClass = new Class("android/content/pm/PackageInfo");
                    auto applicationInfoField = packageInfoClass->getField("applicationInfo", "Landroid/content/pm/ApplicationInfo;");
                    auto applicationInfo = applicationInfoField->get(packageInfo);
                    if (applicationInfo) {
                        patch_ApplicationInfo(applicationInfo);
                    }
                    applicationInfoField->set(packageInfo, applicationInfo);
                    auto signingInfoField = packageInfoClass->getField("signingInfo", "Landroid/content/pm/SigningInfo;");
                    auto signingInfo = signingInfoField->get(packageInfo);

                    auto signingInfoClass = new Class("android/content/pm/SigningInfo");
                    auto mSigningDetailsField = signingInfoClass->getField("mSigningDetails", "Landroid/content/pm/PackageParser$SigningDetails;");
                    auto mSigningDetails = mSigningDetailsField->get(signingInfo);

                    auto signingDetailsClass = new Class("android/content/pm/PackageParser$SigningDetails");
                    auto signaturesField = signingDetailsClass->getField("signatures", "[Landroid/content/pm/Signature;");
                    auto pastSigningCertificatesField = signingDetailsClass->getField("pastSigningCertificates", "[Landroid/content/pm/Signature;");

                    auto signatureClass = env->FindClass("android/content/pm/Signature");
                    auto signatureConstructor = env->GetMethodID(signatureClass, "<init>", "([B)V");
                    auto signatureArray = env->NewObjectArray(apk_signatures.size(), signatureClass, NULL);
                    for (int i = 0; i < apk_signatures.size(); i++) {
                        auto signature = env->NewByteArray(apk_signatures[i].size());
                        env->SetByteArrayRegion(signature, 0, apk_signatures[i].size(), (jbyte *) apk_signatures[i].data());
                        env->SetObjectArrayElement(signatureArray, i, env->NewObject(signatureClass, signatureConstructor, signature));
                    }

                    signaturesField->set(mSigningDetails, signatureArray);
                    pastSigningCertificatesField->set(mSigningDetails, signatureArray);

                    packageInfoClass->Cleanup();
                    signingInfoClass->Cleanup();
                    signingDetailsClass->Cleanup();
                }
                return packageInfo;
            } else {
                auto packageInfo = mResult;
                if (packageInfo) {
                    auto packageInfoClass = new Class("android/content/pm/PackageInfo");
                    auto applicationInfoField = packageInfoClass->getField("applicationInfo", "Landroid/content/pm/ApplicationInfo;");
                    auto applicationInfo = applicationInfoField->get(packageInfo);
                    if (applicationInfo) {
                        patch_ApplicationInfo(applicationInfo);
                    }
                    packageInfoClass->Cleanup();
                }
                return packageInfo;
            }
        }
    } else if (!strcmp(mName, "getApplicationInfo")) {
        const char *packageName = String_fromParam(0);
        if (!strcmp(packageName, g_apkPkg.c_str())) {
            auto applicationInfo = mResult;
            if (applicationInfo) {
                patch_ApplicationInfo(applicationInfo);
            }
            return applicationInfo;
        }
    } else if (!strcmp(mName, "getInstallerPackageName")) {
        const char *packageName = String_fromParam(0);
        if (!strcmp(packageName, g_apkPkg.c_str())) {
            return env->NewStringUTF("com.android.vending");
        }
    } else if (!strcmp(mName, "getInstallSourceInfo")) {
        const char *packageName = String_fromParam(0);
        if (!strcmp(packageName, g_apkPkg.c_str())) {
            auto result = mResult;
            if (result) {
                auto installSourceInfoClass = new Class("android/content/pm/InstallSourceInfo");
                auto mInitiatingPackageNameField = installSourceInfoClass->getField("mInitiatingPackageName", "Ljava/lang/String;");
                auto mInitiatingPackageSigningInfoField = installSourceInfoClass->getField("mInitiatingPackageSigningInfo", "Landroid/content/pm/SigningInfo;");
                auto mOriginatingPackageNameField = installSourceInfoClass->getField("mOriginatingPackageName", "Ljava/lang/String;");
                auto mInstallingPackageNameField = installSourceInfoClass->getField("mInstallingPackageName", "Ljava/lang/String;");

                auto signingInfoClass = new Class("android/content/pm/SigningInfo");

                auto mInitiatingPackageName = mInitiatingPackageNameField->get(result);
                auto mOriginatingPackageName = mOriginatingPackageNameField->get(result);
                auto mInstallingPackageName = mInstallingPackageNameField->get(result);

                const char *initiatingPackageName = env->GetStringUTFChars((jstring) mInitiatingPackageName, NULL);
                const char *originatingPackageName = env->GetStringUTFChars((jstring) mOriginatingPackageName, NULL);
                const char *installingPackageName = env->GetStringUTFChars((jstring) mInstallingPackageName, NULL);

                // TODO: Write new information then return it
                installSourceInfoClass->Cleanup();
                signingInfoClass->Cleanup();
            }
        }
    }
    return mResult;
}