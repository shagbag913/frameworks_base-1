/**
 * Copyright (C) 2017 The Android Open Source Project
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

#define LOG_TAG "radio.Tuner.jni"
#define LOG_NDEBUG 0

#include "com_android_server_radio_Tuner.h"

#include "com_android_server_radio_convert.h"
#include "com_android_server_radio_TunerCallback.h"

#include <JNIHelp.h>
#include <Utils.h>
#include <android/hardware/broadcastradio/1.1/IBroadcastRadioFactory.h>
#include <binder/IPCThreadState.h>
#include <core_jni_helpers.h>
#include <media/AudioSystem.h>
#include <utils/Log.h>

namespace android {
namespace server {
namespace radio {
namespace Tuner {

using hardware::Return;
using hardware::hidl_death_recipient;
using hardware::hidl_vec;

namespace V1_0 = hardware::broadcastradio::V1_0;
namespace V1_1 = hardware::broadcastradio::V1_1;

using V1_0::Band;
using V1_0::BandConfig;
using V1_0::MetaData;
using V1_0::Result;
using V1_1::ITunerCallback;
using V1_1::ProgramListResult;

static Mutex gContextMutex;

static struct {
    struct {
        jclass clazz;
        jmethodID cstor;
        jmethodID add;
    } ArrayList;
    struct {
        jfieldID nativeContext;
        jfieldID region;
        jfieldID tunerCallback;
    } Tuner;
} gjni;

static const char* const kAudioDeviceName = "Radio tuner source";

class HalDeathRecipient : public hidl_death_recipient {
    wp<V1_1::ITunerCallback> mTunerCallback;

public:
    HalDeathRecipient(wp<V1_1::ITunerCallback> tunerCallback):mTunerCallback(tunerCallback) {}

    virtual void serviceDied(uint64_t cookie, const wp<hidl::base::V1_0::IBase>& who);
};

struct TunerContext {
    TunerContext() {}

    bool mIsClosed = false;
    HalRevision mHalRev;
    bool mWithAudio;
    Band mBand;
    sp<V1_0::ITuner> mHalTuner;
    sp<V1_1::ITuner> mHalTuner11;
    sp<HalDeathRecipient> mHalDeathRecipient;

private:
    DISALLOW_COPY_AND_ASSIGN(TunerContext);
};

static TunerContext& getNativeContext(jlong nativeContextHandle) {
    auto nativeContext = reinterpret_cast<TunerContext*>(nativeContextHandle);
    LOG_ALWAYS_FATAL_IF(nativeContext == nullptr, "Native context not initialized");
    return *nativeContext;
}

/**
 * Always lock gContextMutex when using native context.
 */
static TunerContext& getNativeContext(JNIEnv *env, JavaRef<jobject> const &jTuner) {
    return getNativeContext(env->GetLongField(jTuner.get(), gjni.Tuner.nativeContext));
}

static jlong nativeInit(JNIEnv *env, jobject obj, jint halRev, bool withAudio, jint band) {
    ALOGV("nativeInit()");
    AutoMutex _l(gContextMutex);

    auto ctx = new TunerContext();
    ctx->mHalRev = static_cast<HalRevision>(halRev);
    ctx->mWithAudio = withAudio;
    ctx->mBand = static_cast<Band>(band);

    static_assert(sizeof(jlong) >= sizeof(ctx), "jlong is smaller than a pointer");
    return reinterpret_cast<jlong>(ctx);
}

static void nativeFinalize(JNIEnv *env, jobject obj, jlong nativeContext) {
    ALOGV("nativeFinalize()");
    AutoMutex _l(gContextMutex);

    auto ctx = reinterpret_cast<TunerContext*>(nativeContext);
    delete ctx;
}

void HalDeathRecipient::serviceDied(uint64_t cookie __unused,
        const wp<hidl::base::V1_0::IBase>& who __unused) {
    ALOGW("HAL Tuner died unexpectedly");

    auto tunerCallback = mTunerCallback.promote();
    if (tunerCallback == nullptr) return;

    tunerCallback->hardwareFailure();
}

// TODO(b/62713378): implement support for multiple tuners open at the same time
static void notifyAudioService(TunerContext& ctx, bool connected) {
    if (!ctx.mWithAudio) return;

    ALOGD("Notifying AudioService about new state: %d", connected);
    auto token = IPCThreadState::self()->clearCallingIdentity();
    AudioSystem::setDeviceConnectionState(AUDIO_DEVICE_IN_FM_TUNER,
            connected ? AUDIO_POLICY_DEVICE_STATE_AVAILABLE : AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
            nullptr, kAudioDeviceName);
    IPCThreadState::self()->restoreCallingIdentity(token);
}

void setHalTuner(JNIEnv *env, JavaRef<jobject> const &jTuner, sp<V1_0::ITuner> halTuner) {
    ALOGV("setHalTuner(%p)", halTuner.get());
    ALOGE_IF(halTuner == nullptr, "HAL tuner is a nullptr");

    AutoMutex _l(gContextMutex);
    auto& ctx = getNativeContext(env, jTuner);

    if (ctx.mIsClosed) {
        ALOGD("Tuner was closed during initialization");
        // dropping the last reference will close HAL tuner
        return;
    }
    if (ctx.mHalTuner != nullptr) {
        ALOGE("HAL tuner is already set.");
        return;
    }

    ctx.mHalTuner = halTuner;
    ctx.mHalTuner11 = V1_1::ITuner::castFrom(halTuner).withDefault(nullptr);
    ALOGW_IF(ctx.mHalRev >= HalRevision::V1_1 && ctx.mHalTuner11 == nullptr,
            "Provided tuner does not implement 1.1 HAL");

    ctx.mHalDeathRecipient = new HalDeathRecipient(getNativeCallback(env, jTuner));
    halTuner->linkToDeath(ctx.mHalDeathRecipient, 0);

    notifyAudioService(ctx, true);
}

static sp<V1_0::ITuner> getHalTuner(const TunerContext& ctx) {
    auto tuner = ctx.mHalTuner;
    LOG_ALWAYS_FATAL_IF(tuner == nullptr, "HAL tuner is not open");
    return tuner;
}

sp<V1_0::ITuner> getHalTuner(jlong nativeContext) {
    AutoMutex _l(gContextMutex);
    return getHalTuner(getNativeContext(nativeContext));
}

sp<V1_1::ITuner> getHalTuner11(jlong nativeContext) {
    AutoMutex _l(gContextMutex);
    return getNativeContext(nativeContext).mHalTuner11;
}

sp<ITunerCallback> getNativeCallback(JNIEnv *env, JavaRef<jobject> const &tuner) {
    return TunerCallback::getNativeCallback(env,
            env->GetObjectField(tuner.get(), gjni.Tuner.tunerCallback));
}

Region getRegion(JNIEnv *env, jobject obj) {
    return static_cast<Region>(env->GetIntField(obj, gjni.Tuner.region));
}

static void nativeClose(JNIEnv *env, jobject obj, jlong nativeContext) {
    AutoMutex _l(gContextMutex);
    auto& ctx = getNativeContext(nativeContext);
    if (ctx.mIsClosed) return;
    ctx.mIsClosed = true;

    if (ctx.mHalTuner == nullptr) {
        ALOGI("Tuner closed during initialization");
        return;
    }

    ALOGI("Closing tuner %p", ctx.mHalTuner.get());

    notifyAudioService(ctx, false);

    ctx.mHalTuner->unlinkToDeath(ctx.mHalDeathRecipient);
    ctx.mHalDeathRecipient = nullptr;

    ctx.mHalTuner11 = nullptr;
    ctx.mHalTuner = nullptr;
}

static void nativeSetConfiguration(JNIEnv *env, jobject obj, jlong nativeContext, jobject config) {
    ALOGV("nativeSetConfiguration()");
    AutoMutex _l(gContextMutex);
    auto& ctx = getNativeContext(nativeContext);
    auto halTuner = getHalTuner(ctx);
    if (halTuner == nullptr) return;

    Region region_unused;
    BandConfig bandConfigHal = convert::BandConfigToHal(env, config, region_unused);

    if (convert::ThrowIfFailed(env, halTuner->setConfiguration(bandConfigHal))) return;

    ctx.mBand = bandConfigHal.type;
}

static jobject nativeGetConfiguration(JNIEnv *env, jobject obj, jlong nativeContext,
        Region region) {
    ALOGV("nativeSetConfiguration()");
    auto halTuner = getHalTuner(nativeContext);
    if (halTuner == nullptr) return nullptr;

    BandConfig halConfig;
    Result halResult;
    auto hidlResult = halTuner->getConfiguration([&](Result result, const BandConfig& config) {
        halResult = result;
        halConfig = config;
    });
    if (convert::ThrowIfFailed(env, hidlResult, halResult)) {
        return nullptr;
    }

    return convert::BandConfigFromHal(env, halConfig, region).release();
}

static void nativeStep(JNIEnv *env, jobject obj, jlong nativeContext,
        bool directionDown, bool skipSubChannel) {
    ALOGV("nativeStep()");
    auto halTuner = getHalTuner(nativeContext);
    if (halTuner == nullptr) return;

    auto dir = convert::DirectionToHal(directionDown);
    convert::ThrowIfFailed(env, halTuner->step(dir, skipSubChannel));
}

static void nativeScan(JNIEnv *env, jobject obj, jlong nativeContext,
        bool directionDown, bool skipSubChannel) {
    ALOGV("nativeScan()");
    auto halTuner = getHalTuner(nativeContext);
    if (halTuner == nullptr) return;

    auto dir = convert::DirectionToHal(directionDown);
    convert::ThrowIfFailed(env, halTuner->scan(dir, skipSubChannel));
}

static void nativeTune(JNIEnv *env, jobject obj, jlong nativeContext, jobject jSelector) {
    ALOGV("nativeTune()");
    AutoMutex _l(gContextMutex);
    auto& ctx = getNativeContext(nativeContext);
    auto halTuner10 = getHalTuner(ctx);
    auto halTuner11 = ctx.mHalTuner11;
    if (halTuner10 == nullptr) return;

    auto selector = convert::ProgramSelectorToHal(env, jSelector);
    if (halTuner11 != nullptr) {
        convert::ThrowIfFailed(env, halTuner11->tune_1_1(selector));
    } else {
        uint32_t channel, subChannel;
        if (!V1_1::utils::getLegacyChannel(selector, &channel, &subChannel)) {
            jniThrowException(env, "java/lang/IllegalArgumentException",
                    "Can't tune to non-AM/FM channel with HAL<1.1");
            return;
        }
        convert::ThrowIfFailed(env, halTuner10->tune(channel, subChannel));
    }
}

static void nativeCancel(JNIEnv *env, jobject obj, jlong nativeContext) {
    ALOGV("nativeCancel()");
    auto halTuner = getHalTuner(nativeContext);
    if (halTuner == nullptr) return;

    convert::ThrowIfFailed(env, halTuner->cancel());
}

static void nativeCancelAnnouncement(JNIEnv *env, jobject obj, jlong nativeContext) {
    ALOGV("%s", __func__);
    auto halTuner = getHalTuner11(nativeContext);
    if (halTuner == nullptr) {
        ALOGI("cancelling announcements is not supported with HAL < 1.1");
        return;
    }

    convert::ThrowIfFailed(env, halTuner->cancelAnnouncement());
}

static jobject nativeGetProgramInformation(JNIEnv *env, jobject obj, jlong nativeContext) {
    ALOGV("nativeGetProgramInformation()");
    AutoMutex _l(gContextMutex);
    auto& ctx = getNativeContext(nativeContext);
    auto halTuner10 = getHalTuner(ctx);
    auto halTuner11 = ctx.mHalTuner11;
    if (halTuner10 == nullptr) return nullptr;

    JavaRef<jobject> jInfo;
    Result halResult;
    Return<void> hidlResult;
    if (halTuner11 != nullptr) {
        hidlResult = halTuner11->getProgramInformation_1_1([&](Result result,
                const V1_1::ProgramInfo& info) {
            halResult = result;
            if (result != Result::OK) return;
            jInfo = convert::ProgramInfoFromHal(env, info);
        });
    } else {
        hidlResult = halTuner10->getProgramInformation([&](Result result,
                const V1_0::ProgramInfo& info) {
            halResult = result;
            if (result != Result::OK) return;
            jInfo = convert::ProgramInfoFromHal(env, info, ctx.mBand);
        });
    }

    if (jInfo != nullptr) return jInfo.release();
    convert::ThrowIfFailed(env, hidlResult, halResult);
    return nullptr;
}

static bool nativeStartBackgroundScan(JNIEnv *env, jobject obj, jlong nativeContext) {
    ALOGV("nativeStartBackgroundScan()");
    auto halTuner = getHalTuner11(nativeContext);
    if (halTuner == nullptr) {
        ALOGI("Background scan is not supported with HAL < 1.1");
        return false;
    }

    auto halResult = halTuner->startBackgroundScan();

    if (halResult.isOk() && halResult == ProgramListResult::UNAVAILABLE) return false;
    return !convert::ThrowIfFailed(env, halResult);
}

static jobject nativeGetProgramList(JNIEnv *env, jobject obj, jlong nativeContext, jstring jFilter) {
    ALOGV("nativeGetProgramList()");
    auto halTuner = getHalTuner11(nativeContext);
    if (halTuner == nullptr) {
        ALOGI("Program list is not supported with HAL < 1.1");
        return nullptr;
    }

    JavaRef<jobject> jList;
    ProgramListResult halResult = ProgramListResult::NOT_INITIALIZED;
    auto filter = env->GetStringUTFChars(jFilter, nullptr);
    auto hidlResult = halTuner->getProgramList(filter,
            [&](ProgramListResult result, const hidl_vec<V1_1::ProgramInfo>& programList) {
        halResult = result;
        if (halResult != ProgramListResult::OK) return;

        jList = make_javaref(env, env->NewObject(gjni.ArrayList.clazz, gjni.ArrayList.cstor));
        for (auto& program : programList) {
            auto jProgram = convert::ProgramInfoFromHal(env, program);
            env->CallBooleanMethod(jList.get(), gjni.ArrayList.add, jProgram.get());
        }
    });

    if (convert::ThrowIfFailed(env, hidlResult, halResult)) return nullptr;

    return jList.release();
}

static bool nativeIsAnalogForced(JNIEnv *env, jobject obj, jlong nativeContext) {
    ALOGV("nativeIsAnalogForced()");
    auto halTuner = getHalTuner11(nativeContext);
    if (halTuner == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException",
                "Forced analog switch is not supported with HAL < 1.1");
        return false;
    }

    bool isForced;
    Result halResult;
    auto hidlResult = halTuner->isAnalogForced([&](Result result, bool isForcedRet) {
        halResult = result;
        isForced = isForcedRet;
    });

    if (convert::ThrowIfFailed(env, hidlResult, halResult)) return false;

    return isForced;
}

static void nativeSetAnalogForced(JNIEnv *env, jobject obj, jlong nativeContext, bool isForced) {
    ALOGV("nativeSetAnalogForced()");
    auto halTuner = getHalTuner11(nativeContext);
    if (halTuner == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException",
                "Forced analog switch is not supported with HAL < 1.1");
        return;
    }

    auto halResult = halTuner->setAnalogForced(isForced);
    convert::ThrowIfFailed(env, halResult);
}

static bool nativeIsAntennaConnected(JNIEnv *env, jobject obj, jlong nativeContext) {
    ALOGV("nativeIsAntennaConnected()");
    auto halTuner = getHalTuner(nativeContext);
    if (halTuner == nullptr) return false;

    bool isConnected = false;
    Result halResult;
    auto hidlResult = halTuner->getConfiguration([&](Result result, const BandConfig& config) {
        halResult = result;
        isConnected = config.antennaConnected;
    });
    convert::ThrowIfFailed(env, hidlResult, halResult);
    return isConnected;
}

static const JNINativeMethod gTunerMethods[] = {
    { "nativeInit", "(IZI)J", (void*)nativeInit },
    { "nativeFinalize", "(J)V", (void*)nativeFinalize },
    { "nativeClose", "(J)V", (void*)nativeClose },
    { "nativeSetConfiguration", "(JLandroid/hardware/radio/RadioManager$BandConfig;)V",
            (void*)nativeSetConfiguration },
    { "nativeGetConfiguration", "(JI)Landroid/hardware/radio/RadioManager$BandConfig;",
            (void*)nativeGetConfiguration },
    { "nativeStep", "(JZZ)V", (void*)nativeStep },
    { "nativeScan", "(JZZ)V", (void*)nativeScan },
    { "nativeTune", "(JLandroid/hardware/radio/ProgramSelector;)V", (void*)nativeTune },
    { "nativeCancel", "(J)V", (void*)nativeCancel },
    { "nativeCancelAnnouncement", "(J)V", (void*)nativeCancelAnnouncement },
    { "nativeGetProgramInformation", "(J)Landroid/hardware/radio/RadioManager$ProgramInfo;",
            (void*)nativeGetProgramInformation },
    { "nativeStartBackgroundScan", "(J)Z", (void*)nativeStartBackgroundScan },
    { "nativeGetProgramList", "(JLjava/lang/String;)Ljava/util/List;",
            (void*)nativeGetProgramList },
    { "nativeIsAnalogForced", "(J)Z", (void*)nativeIsAnalogForced },
    { "nativeSetAnalogForced", "(JZ)V", (void*)nativeSetAnalogForced },
    { "nativeIsAntennaConnected", "(J)Z", (void*)nativeIsAntennaConnected },
};

} // namespace Tuner
} // namespace radio
} // namespace server

void register_android_server_radio_Tuner(JavaVM *vm, JNIEnv *env) {
    using namespace server::radio::Tuner;

    register_android_server_radio_TunerCallback(vm, env);

    auto tunerClass = FindClassOrDie(env, "com/android/server/radio/Tuner");
    gjni.Tuner.nativeContext = GetFieldIDOrDie(env, tunerClass, "mNativeContext", "J");
    gjni.Tuner.region = GetFieldIDOrDie(env, tunerClass, "mRegion", "I");
    gjni.Tuner.tunerCallback = GetFieldIDOrDie(env, tunerClass, "mTunerCallback",
            "Lcom/android/server/radio/TunerCallback;");

    auto arrayListClass = FindClassOrDie(env, "java/util/ArrayList");
    gjni.ArrayList.clazz = MakeGlobalRefOrDie(env, arrayListClass);
    gjni.ArrayList.cstor = GetMethodIDOrDie(env, arrayListClass, "<init>", "()V");
    gjni.ArrayList.add = GetMethodIDOrDie(env, arrayListClass, "add", "(Ljava/lang/Object;)Z");

    auto res = jniRegisterNativeMethods(env, "com/android/server/radio/Tuner",
            gTunerMethods, NELEM(gTunerMethods));
    LOG_ALWAYS_FATAL_IF(res < 0, "Unable to register native methods.");
}

} // namespace android
