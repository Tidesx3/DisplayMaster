# JNI: native code calls these by name.
-keep class com.displaymaster.client.NativeClient {
    native <methods>;
    void onNative*(...);
}
