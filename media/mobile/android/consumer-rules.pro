# Consumer ProGuard rules for library users

# Keep public API
-keep public class com.turbonet.media.** {
    public *;
    protected *;
}

# Keep native methods
-keepclasseswithmembernames class * {
    native <methods>;
}
