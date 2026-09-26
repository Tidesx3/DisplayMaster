// JNI surface for com.displaymaster.client.NativeClient.
#include <android/native_window_jni.h>
#include <jni.h>

#include <memory>
#include <string>

#include "client.h"
#include "dm/input_math.h"
#include "dm/keymap.h"

using namespace dm;

namespace {

JavaVM* g_vm = nullptr;

// Attaches the calling native thread to the JVM for the duration of a callback.
class JniEnv {
public:
    JniEnv() {
        if (g_vm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6) == JNI_EDETACHED) {
            g_vm->AttachCurrentThread(&env_, nullptr);
            attached_ = true;
        }
    }
    ~JniEnv() {
        if (attached_) g_vm->DetachCurrentThread();
    }
    JNIEnv* operator->() const { return env_; }
    JNIEnv* get() const { return env_; }

private:
    JNIEnv* env_ = nullptr;
    bool attached_ = false;
};

std::string to_string(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out(c);
    env->ReleaseStringUTFChars(s, c);
    return out;
}

// Forwards native events to the Kotlin NativeClient object.
class JavaListener final : public ClientListener {
public:
    JavaListener(JNIEnv* env, jobject obj) : obj_(env->NewGlobalRef(obj)) {
        jclass cls = env->GetObjectClass(obj);
        on_state_ = env->GetMethodID(cls, "onNativeState", "(ILjava/lang/String;)V");
        on_config_ = env->GetMethodID(cls, "onNativeVideoConfig", "(IIIIIFFFFI)V");
        on_stats_ = env->GetMethodID(cls, "onNativeStats", "(FFFFI)V");
        is_known_pc_ = env->GetMethodID(cls, "isNativeKnownPc", "(Ljava/lang/String;)Z");
        on_pairing_ = env->GetMethodID(cls, "onNativePairing", "(Ljava/lang/String;Ljava/lang/String;)V");
    }
    ~JavaListener() override {
        JniEnv env;
        env->DeleteGlobalRef(obj_);
    }

    void on_state(State s, const std::string& message) override {
        JniEnv env;
        jstring msg = env->NewStringUTF(message.c_str());
        env->CallVoidMethod(obj_, on_state_, static_cast<jint>(s), msg);
        env->DeleteLocalRef(msg);
    }
    void on_video_config(const proto::VideoConfig& c) override {
        JniEnv env;
        env->CallVoidMethod(obj_, on_config_, static_cast<jint>(c.codec), static_cast<jint>(c.width),
                            static_cast<jint>(c.height), static_cast<jint>(c.fps), static_cast<jint>(c.bitrate_kbps),
                            c.content_x, c.content_y, c.content_w, c.content_h, static_cast<jint>(c.mode));
    }
    void on_stats(const Stats& s) override {
        JniEnv env;
        env->CallVoidMethod(obj_, on_stats_, s.fps, s.mbps, s.rtt_ms, s.decode_ms, static_cast<jint>(s.dropped));
    }

    bool is_known_pc(const std::string& pc_key) override {
        JniEnv env;
        jstring key = env->NewStringUTF(pc_key.c_str());
        const bool known = env->CallBooleanMethod(obj_, is_known_pc_, key);
        env->DeleteLocalRef(key);
        return known;
    }
    void on_pairing(const std::string& code, const std::string& pc_key) override {
        JniEnv env;
        jstring c = env->NewStringUTF(code.c_str());
        jstring key = env->NewStringUTF(pc_key.c_str());
        env->CallVoidMethod(obj_, on_pairing_, c, key);
        env->DeleteLocalRef(c);
        env->DeleteLocalRef(key);
    }

private:
    jobject obj_;
    jmethodID on_state_, on_config_, on_stats_, is_known_pc_, on_pairing_;
};

struct Handle {
    std::unique_ptr<JavaListener> listener;
    std::unique_ptr<Client> client;
};

Client* client_of(jlong h) {
    return reinterpret_cast<Handle*>(h)->client.get();
}

}  // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

JNIEXPORT jlong JNICALL Java_com_displaymaster_client_NativeClient_nativeCreate(JNIEnv* env, jobject self) {
    auto* h = new Handle;
    h->listener = std::make_unique<JavaListener>(env, self);
    h->client = std::make_unique<Client>(h->listener.get());
    return reinterpret_cast<jlong>(h);
}

JNIEXPORT void JNICALL Java_com_displaymaster_client_NativeClient_nativeDestroy(JNIEnv*, jobject, jlong h) {
    auto* handle = reinterpret_cast<Handle*>(h);
    handle->client.reset();  // joins threads before the listener goes away
    delete handle;
}

JNIEXPORT void JNICALL Java_com_displaymaster_client_NativeClient_nativeConnect(
    JNIEnv* env, jobject, jlong h, jstring host, jint port, jstring device_id, jstring name, jstring model, jint sdk,
    jint width, jint height, jint dpi, jint refresh_mhz, jint rotation, jint posture, jint codecs, jint input_caps,
    jint transport, jint mode, jint touch_mode, jint max_fps, jint preferred_codec, jbyteArray identity) {
    proto::Hello hello;
    hello.device_id = to_string(env, device_id);
    hello.device_name = to_string(env, name);
    hello.model = to_string(env, model);
    hello.sdk_int = static_cast<uint16_t>(sdk);
    hello.geometry = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint16_t>(dpi),
                      static_cast<uint32_t>(refresh_mhz), static_cast<uint8_t>(rotation),
                      static_cast<proto::Posture>(posture)};
    hello.decode_codecs = static_cast<uint32_t>(codecs);
    hello.input_caps = static_cast<uint32_t>(input_caps);
    hello.max_bitrate_kbps = 0;
    hello.transport = static_cast<proto::Transport>(transport);
    hello.settings.mode = static_cast<proto::DisplayMode>(mode);
    hello.settings.touch_mode = static_cast<proto::TouchMode>(touch_mode);
    hello.settings.max_fps = static_cast<uint32_t>(max_fps);
    hello.settings.preferred_codec = static_cast<proto::Codec>(preferred_codec);
    // This device's long-term key: present means encrypt (always over Wi-Fi).
    std::optional<noise::Key> key;
    if (identity && env->GetArrayLength(identity) == static_cast<jsize>(noise::kKeySize)) {
        key.emplace();
        env->GetByteArrayRegion(identity, 0, static_cast<jsize>(noise::kKeySize), reinterpret_cast<jbyte*>(key->data()));
    }
    client_of(h)->connect(to_string(env, host), static_cast<uint16_t>(port), hello, key);
}

JNIEXPORT void JNICALL Java_com_displaymaster_client_NativeClient_nativeConfirmPairing(JNIEnv*, jobject, jlong h,
                                                                                       jboolean codes_match) {
    client_of(h)->confirm_pairing(codes_match);
}

// A fresh long-term key for this install (32 bytes from the OS RNG).
JNIEXPORT jbyteArray JNICALL Java_com_displaymaster_client_NativeClient_nativeGenerateIdentity(JNIEnv* env, jclass) {
    const auto kp = noise::KeyPair::generate();
    jbyteArray out = env->NewByteArray(static_cast<jsize>(noise::kKeySize));
    env->SetByteArrayRegion(out, 0, static_cast<jsize>(noise::kKeySize), reinterpret_cast<const jbyte*>(kp.priv.data()));
    return out;
}

JNIEXPORT void JNICALL Java_com_displaymaster_client_NativeClient_nativeDisconnect(JNIEnv*, jobject, jlong h) {
    client_of(h)->disconnect();
}

JNIEXPORT void JNICALL Java_com_displaymaster_client_NativeClient_nativeSetSurface(JNIEnv* env, jobject, jlong h,
                                                                                   jobject surface) {
    client_of(h)->set_surface(surface ? ANativeWindow_fromSurface(env, surface) : nullptr);
}

JNIEXPORT void JNICALL Java_com_displaymaster_client_NativeClient_nativeSendPen(JNIEnv*, jobject, jlong h, jint flags,
                                                                                jfloat x, jfloat y, jfloat pressure,
                                                                                jfloat tilt_rad, jfloat orientation_rad,
                                                                                jlong time_us) {
    proto::Pen p;
    p.flags = static_cast<uint8_t>(flags);
    p.x = x;
    p.y = y;
    p.pressure = pressure;
    const auto t = tilt_orientation_to_xy(tilt_rad, orientation_rad);
    p.tilt_x = t.x_deg;
    p.tilt_y = t.y_deg;
    p.event_time_us = static_cast<uint64_t>(time_us);
    client_of(h)->send(p);
}

JNIEXPORT void JNICALL Java_com_displaymaster_client_NativeClient_nativeSendTouch(
    JNIEnv* env, jobject, jlong h, jint action, jint action_id, jintArray ids, jfloatArray xs, jfloatArray ys,
    jfloatArray pressures, jfloatArray majors, jint count, jlong time_us) {
    proto::Touch t;
    t.action = static_cast<proto::PointerAction>(action);
    t.action_id = static_cast<uint32_t>(action_id);
    t.event_time_us = static_cast<uint64_t>(time_us);
    jint* id = env->GetIntArrayElements(ids, nullptr);
    jfloat* x = env->GetFloatArrayElements(xs, nullptr);
    jfloat* y = env->GetFloatArrayElements(ys, nullptr);
    jfloat* p = env->GetFloatArrayElements(pressures, nullptr);
    jfloat* m = env->GetFloatArrayElements(majors, nullptr);
    t.points.resize(static_cast<size_t>(count));
    for (jint i = 0; i < count; ++i) t.points[i] = {static_cast<uint32_t>(id[i]), x[i], y[i], p[i], m[i]};
    env->ReleaseIntArrayElements(ids, id, JNI_ABORT);
    env->ReleaseFloatArrayElements(xs, x, JNI_ABORT);
    env->ReleaseFloatArrayElements(ys, y, JNI_ABORT);
    env->ReleaseFloatArrayElements(pressures, p, JNI_ABORT);
    env->ReleaseFloatArrayElements(majors, m, JNI_ABORT);
    client_of(h)->send(t);
}

JNIEXPORT void JNICALL Java_com_displaymaster_client_NativeClient_nativeSendMouse(JNIEnv*, jobject, jlong h, jint kind,
                                                                                  jfloat x, jfloat y, jint button,
                                                                                  jboolean down) {
    proto::Mouse m;
    m.kind = static_cast<proto::MouseKind>(kind);
    m.x = x;
    m.y = y;
    m.button = static_cast<proto::MouseButton>(button);
    m.down = down;
    client_of(h)->send(m);
}

// evdev: hardware scancode from KeyEvent.getScanCode() (0 if none).
// set1/extended: direct PC scancode for keys Kotlin mapped from the keycode.
JNIEXPORT void JNICALL Java_com_displaymaster_client_NativeClient_nativeSendKey(JNIEnv*, jobject, jlong h, jint evdev,
                                                                                jint set1, jboolean extended,
                                                                                jboolean down, jint unicode) {
    proto::Key k;
    Scancode sc = evdev > 0 ? evdev_to_set1(static_cast<uint32_t>(evdev)) : Scancode{};
    if (!sc.code && set1 > 0) sc = {static_cast<uint16_t>(set1), static_cast<bool>(extended)};
    k.scancode = sc.code;
    k.flags = static_cast<uint8_t>((down ? proto::kKeyDown : 0) | (sc.extended ? proto::kKeyExtended : 0));
    k.unicode = sc.code ? 0 : static_cast<uint32_t>(unicode);
    if (!k.scancode && !k.unicode) return;
    client_of(h)->send(k);
}

JNIEXPORT void JNICALL Java_com_displaymaster_client_NativeClient_nativeSendGeometry(JNIEnv*, jobject, jlong h,
                                                                                     jint width, jint height, jint dpi,
                                                                                     jint refresh_mhz, jint rotation,
                                                                                     jint posture) {
    client_of(h)->send(proto::DisplayGeometry{static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                              static_cast<uint16_t>(dpi), static_cast<uint32_t>(refresh_mhz),
                                              static_cast<uint8_t>(rotation), static_cast<proto::Posture>(posture)});
}

JNIEXPORT void JNICALL Java_com_displaymaster_client_NativeClient_nativeSendSettings(JNIEnv*, jobject, jlong h,
                                                                                     jint mode, jint touch_mode,
                                                                                     jint max_fps, jint bitrate_kbps,
                                                                                     jint codec) {
    proto::ClientSettings s;
    s.mode = static_cast<proto::DisplayMode>(mode);
    s.touch_mode = static_cast<proto::TouchMode>(touch_mode);
    s.max_fps = static_cast<uint32_t>(max_fps);
    s.bitrate_kbps = static_cast<uint32_t>(bitrate_kbps);
    s.preferred_codec = static_cast<proto::Codec>(codec);
    client_of(h)->send(s);
}

}  // extern "C"
