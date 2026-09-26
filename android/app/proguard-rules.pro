# JNI: native code calls these by name (android/app/src/main/cpp/jni_bridge.cpp, GetMethodID).
# tools\package.ps1 checks every GetMethodID name is still in the release dex.
-keep class com.displaymaster.client.NativeClient {
    native <methods>;
    void onNative*(...);
    boolean isNative*(...);
}
