plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.iptv.tv"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.iptv.tv"
        minSdk = 21
        targetSdk = 34
        versionCode = 1
        versionName = "2026.09.21"
        ndk { abiFilters += listOf("armeabi-v7a", "arm64-v8a") }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // 用 debug 密钥签名，编出来的 release APK 可直接安装
            signingConfig = signingConfigs.getByName("debug")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
        isCoreLibraryDesugaringEnabled = true
    }
    kotlinOptions { jvmTarget = "17" }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.recyclerview:recyclerview:1.3.2")
    implementation("org.videolan.android:libvlc-all:3.6.0")
    coreLibraryDesugaring("com.android.tools:desugar_jdk_libs:2.0.4")
}
