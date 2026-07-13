# TurboNet Media ProGuard Rules

# Keep native methods
-keepclasseswithmembernames class * {
    native <methods>;
}

# Keep TurboNet Media API
-keep class com.turbonet.media.** { *; }

# Keep enums
-keepclassmembers enum * {
    public static **[] values();
    public static ** valueOf(java.lang.String);
}

# Keep Parcelable implementations
-keep class * implements android.os.Parcelable {
    public static final android.os.Parcelable$Creator *;
}

# Keep callbacks and listeners
-keep class * implements com.turbonet.media.*.Callback { *; }
-keep class * implements com.turbonet.media.*.Listener { *; }

# Suppress warnings for native libraries
-dontwarn com.turbonet.media.**
