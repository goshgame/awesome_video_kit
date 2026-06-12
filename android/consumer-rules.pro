# JNI entry points are bound to the exact NativeBridge class name.
-keep class com.goshlive.awesomevideokit.internal.NativeBridge { *; }
-keepclasseswithmembernames class com.goshlive.awesomevideokit.internal.NativeBridge {
    native <methods>;
}

# Keep the plugin entry and nested manager types stable for release builds.
-keep class com.goshlive.awesome_video_kit.AwesomeVideoKitPlugin { *; }
-keep class com.goshlive.awesome_video_kit.AwesomeVideoKitManagerJni { *; }
-keep class com.goshlive.awesome_video_kit.AwesomeVideoKitManagerJni$* { *; }

# Native code reflects on onProgress(int); renaming it breaks callbacks in release.
-keep class * implements com.goshlive.awesome_video_kit.AwesomeVideoKitManagerJni$ProgressCallback { *; }
-keepclassmembers class * implements com.goshlive.awesome_video_kit.AwesomeVideoKitManagerJni$ProgressCallback {
    void onProgress(int);
}
