# dlms losss — ProGuard rules (mas ari)

# Keep JNI bridge class (native methods must not be renamed)
-keep class com.masari.dlmslosss.DlmsNativeInterface { *; }
-keepclasseswithmembernames class * {
    native <methods>;
}

# Keep ViewModel and LiveData
-keep class androidx.lifecycle.** { *; }

# Oboe is native — no Java classes to keep
