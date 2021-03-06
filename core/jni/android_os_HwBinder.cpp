/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "android_os_HwBinder"
#include <android-base/logging.h>

#include "android_os_HwBinder.h"

#include "android_os_HwParcel.h"
#include "android_os_HwRemoteBinder.h"

#include <JNIHelp.h>
#include <android/hidl/manager/1.0/IServiceManager.h>
#include <android/hidl/base/1.0/IBase.h>
#include <android/hidl/base/1.0/BpBase.h>
#include <android_runtime/AndroidRuntime.h>
#include <hidl/ServiceManagement.h>
#include <hidl/Status.h>
#include <hidl/HidlTransportSupport.h>
#include <hwbinder/ProcessState.h>
#include <nativehelper/ScopedLocalRef.h>

#include "core_jni_helpers.h"

using android::AndroidRuntime;
using android::hardware::hidl_vec;
using android::hardware::hidl_string;

#define PACKAGE_PATH    "android/os"
#define CLASS_NAME      "HwBinder"
#define CLASS_PATH      PACKAGE_PATH "/" CLASS_NAME

namespace android {

static jclass gArrayListClass;
static struct {
    jmethodID size;
    jmethodID get;
} gArrayListMethods;

static struct fields_t {
    jfieldID contextID;
    jmethodID onTransactID;
} gFields;

// static
void JHwBinder::InitClass(JNIEnv *env) {
    ScopedLocalRef<jclass> clazz(
            env, FindClassOrDie(env, CLASS_PATH));

    gFields.contextID =
        GetFieldIDOrDie(env, clazz.get(), "mNativeContext", "J");

    gFields.onTransactID =
        GetMethodIDOrDie(
                env,
                clazz.get(),
                "onTransact",
                "(IL" PACKAGE_PATH "/HwParcel;L" PACKAGE_PATH "/HwParcel;I)V");
}

// static
sp<JHwBinder> JHwBinder::SetNativeContext(
        JNIEnv *env, jobject thiz, const sp<JHwBinder> &context) {
    sp<JHwBinder> old =
        (JHwBinder *)env->GetLongField(thiz, gFields.contextID);

    if (context != NULL) {
        context->incStrong(NULL /* id */);
    }

    if (old != NULL) {
        old->decStrong(NULL /* id */);
    }

    env->SetLongField(thiz, gFields.contextID, (long)context.get());

    return old;
}

// static
sp<JHwBinder> JHwBinder::GetNativeContext(
        JNIEnv *env, jobject thiz) {
    return (JHwBinder *)env->GetLongField(thiz, gFields.contextID);
}

JHwBinder::JHwBinder(JNIEnv *env, jobject thiz) {
    jclass clazz = env->GetObjectClass(thiz);
    CHECK(clazz != NULL);

    mClass = (jclass)env->NewGlobalRef(clazz);
    mObject = env->NewWeakGlobalRef(thiz);
}

JHwBinder::~JHwBinder() {
    JNIEnv *env = AndroidRuntime::getJNIEnv();

    env->DeleteWeakGlobalRef(mObject);
    mObject = NULL;

    env->DeleteGlobalRef(mClass);
    mClass = NULL;
}

status_t JHwBinder::onTransact(
        uint32_t code,
        const hardware::Parcel &data,
        hardware::Parcel *reply,
        uint32_t flags,
        TransactCallback callback) {
    JNIEnv *env = AndroidRuntime::getJNIEnv();

    ScopedLocalRef<jobject> requestObj(env, JHwParcel::NewObject(env));
    JHwParcel::GetNativeContext(env, requestObj.get())->setParcel(
            const_cast<hardware::Parcel *>(&data), false /* assumeOwnership */);

    ScopedLocalRef<jobject> replyObj(env, JHwParcel::NewObject(env));

    sp<JHwParcel> replyContext =
        JHwParcel::GetNativeContext(env, replyObj.get());

    replyContext->setParcel(reply, false /* assumeOwnership */);
    replyContext->setTransactCallback(callback);

    env->CallVoidMethod(
            mObject,
            gFields.onTransactID,
            code,
            requestObj.get(),
            replyObj.get(),
            flags);

    status_t err = OK;

    if (!replyContext->wasSent()) {
        // The implementation never finished the transaction.
        err = UNKNOWN_ERROR;  // XXX special error code instead?

        reply->setDataPosition(0 /* pos */);
    }

    // Release all temporary storage now that scatter-gather data
    // has been consolidated, either by calling the TransactCallback,
    // if wasSent() == true or clearing the reply parcel (setDataOffset above).
    replyContext->getStorage()->release(env);

    // We cannot permanently pass ownership of "data" and "reply" over to their
    // Java object wrappers (we don't own them ourselves).

    JHwParcel::GetNativeContext(env, requestObj.get())->setParcel(
            NULL /* parcel */, false /* assumeOwnership */);

    replyContext->setParcel(
            NULL /* parcel */, false /* assumeOwnership */);

    return err;
}

}  // namespace android

////////////////////////////////////////////////////////////////////////////////

using namespace android;

static void releaseNativeContext(void *nativeContext) {
    sp<JHwBinder> binder = (JHwBinder *)nativeContext;

    if (binder != NULL) {
        binder->decStrong(NULL /* id */);
    }
}

static jlong JHwBinder_native_init(JNIEnv *env) {
    JHwBinder::InitClass(env);

    return reinterpret_cast<jlong>(&releaseNativeContext);
}

static void JHwBinder_native_setup(JNIEnv *env, jobject thiz) {
    sp<JHwBinder> context = new JHwBinder(env, thiz);

    JHwBinder::SetNativeContext(env, thiz, context);
}

static void JHwBinder_native_transact(
        JNIEnv * /* env */,
        jobject /* thiz */,
        jint /* code */,
        jobject /* requestObj */,
        jobject /* replyObj */,
        jint /* flags */) {
    CHECK(!"Should not be here");
}

static void JHwBinder_native_registerService(
        JNIEnv *env,
        jobject thiz,
        jobject interfaceChainArrayList,
        jstring serviceNameObj) {
    if (serviceNameObj == NULL) {
        jniThrowException(env, "java/lang/NullPointerException", NULL);
        return;
    }

    const char *serviceName = env->GetStringUTFChars(serviceNameObj, NULL);
    if (serviceName == NULL) {
        return;  // XXX exception already pending?
    }

    jint numInterfaces = env->CallIntMethod(interfaceChainArrayList,
                                            gArrayListMethods.size);
    hidl_string *strings = new hidl_string[numInterfaces];

    for (jint i = 0; i < numInterfaces; i++) {
        jstring strObj = static_cast<jstring>(
            env->CallObjectMethod(interfaceChainArrayList,
                                  gArrayListMethods.get,
                                  i)
        );
        const char * str = env->GetStringUTFChars(strObj, nullptr);
        strings[i] = hidl_string(str);
        env->ReleaseStringUTFChars(strObj, str);
    }

    hidl_vec<hidl_string> interfaceChain;
    interfaceChain.setToExternal(strings, numInterfaces, true /* shouldOwn */);

    using android::hidl::manager::V1_0::IServiceManager;

    sp<hardware::IBinder> binder = JHwBinder::GetNativeContext(env, thiz);

    /* TODO(b/33440494) this is not right */
    sp<hidl::base::V1_0::IBase> base = new hidl::base::V1_0::BpBase(binder);

    auto manager = hardware::defaultServiceManager();

    if (manager == nullptr) {
        LOG(ERROR) << "Could not get hwservicemanager.";
        signalExceptionForError(env, UNKNOWN_ERROR);
        return;
    }

    bool ok = manager->add(
                interfaceChain,
                serviceName,
                base);

    env->ReleaseStringUTFChars(serviceNameObj, serviceName);
    serviceName = NULL;

    if (ok) {
        LOG(INFO) << "Starting thread pool.";
        ::android::hardware::ProcessState::self()->startThreadPool();
    }

    signalExceptionForError(env, (ok ? OK : UNKNOWN_ERROR));
}

static jobject JHwBinder_native_getService(
        JNIEnv *env,
        jclass /* clazzObj */,
        jstring ifaceNameObj,
        jstring serviceNameObj) {

    if (ifaceNameObj == NULL) {
        jniThrowException(env, "java/lang/NullPointerException", NULL);
        return NULL;
    }
    if (serviceNameObj == NULL) {
        jniThrowException(env, "java/lang/NullPointerException", NULL);
        return NULL;
    }

    const char *ifaceName = env->GetStringUTFChars(ifaceNameObj, NULL);
    if (ifaceName == NULL) {
        return NULL; // XXX exception already pending?
    }
    const char *serviceName = env->GetStringUTFChars(serviceNameObj, NULL);
    if (serviceName == NULL) {
        env->ReleaseStringUTFChars(ifaceNameObj, ifaceName);
        return NULL; // XXX exception already pending?
    }

    LOG(INFO) << "looking for service '"
              << serviceName
              << "'";

    auto manager = hardware::defaultServiceManager();

    if (manager == nullptr) {
        LOG(ERROR) << "Could not get hwservicemanager.";
        signalExceptionForError(env, UNKNOWN_ERROR);
        return NULL;
    }

    sp<hardware::IBinder> service;
    manager->get(
            ifaceName,
            serviceName,
            [&service](sp<hidl::base::V1_0::IBase> out) {
                service = hardware::toBinder<
                        hidl::base::V1_0::IBase, hidl::base::V1_0::BpBase
                    >(out);
            });

    env->ReleaseStringUTFChars(ifaceNameObj, ifaceName);
    ifaceName = NULL;
    env->ReleaseStringUTFChars(serviceNameObj, serviceName);
    serviceName = NULL;

    if (service == NULL) {
        signalExceptionForError(env, NAME_NOT_FOUND);
        return NULL;
    }

    LOG(INFO) << "Starting thread pool.";
    ::android::hardware::ProcessState::self()->startThreadPool();

    return JHwRemoteBinder::NewObject(env, service);
}

static JNINativeMethod gMethods[] = {
    { "native_init", "()J", (void *)JHwBinder_native_init },
    { "native_setup", "()V", (void *)JHwBinder_native_setup },

    { "transact",
        "(IL" PACKAGE_PATH "/HwParcel;L" PACKAGE_PATH "/HwParcel;I)V",
        (void *)JHwBinder_native_transact },

    { "registerService", "(Ljava/util/ArrayList;Ljava/lang/String;)V",
        (void *)JHwBinder_native_registerService },

    { "getService", "(Ljava/lang/String;Ljava/lang/String;)L" PACKAGE_PATH "/IHwBinder;",
        (void *)JHwBinder_native_getService },
};

namespace android {

int register_android_os_HwBinder(JNIEnv *env) {
    jclass arrayListClass = FindClassOrDie(env, "java/util/ArrayList");
    gArrayListClass = MakeGlobalRefOrDie(env, arrayListClass);
    gArrayListMethods.size = GetMethodIDOrDie(env, arrayListClass, "size", "()I");
    gArrayListMethods.get = GetMethodIDOrDie(env, arrayListClass, "get", "(I)Ljava/lang/Object;");

    return RegisterMethodsOrDie(env, CLASS_PATH, gMethods, NELEM(gMethods));
}

}  // namespace android
