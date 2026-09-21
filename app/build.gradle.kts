import java.time.LocalDate
import java.time.ZoneId
import java.time.format.DateTimeFormatter

// 版本号 = 编译日期（北京时间），如 2026-09-21 编译 => 26.09.21
// GitHub Actions 会用 -PbuildDate=26.09.21 传入；本地编译则取当天日期
val buildDate: String = (project.findProperty("buildDate") as String?)
    ?: LocalDate.now(ZoneId.of("Asia/Shanghai")).format(DateTimeFormatter.ofPattern("yy.MM.dd"))

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
        versionCode = buildDate.replace(".", "").toInt()   // 26.09.21 -> 260921，随日期递增
        versionName = buildDate
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

    // APK 文件名：IPTV Player v26.09.21.apk
    applicationVariants.all {
        outputs.all {
            (this as com.android.build.gradle.internal.api.BaseVariantOutputImpl).outputFileName =
                "IPTV Player v$buildDate.apk"
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.recyclerview:recyclerview:1.3.2")
    implementation("org.videolan.android:libvlc-all:3.6.0")
    coreLibraryDesugaring("com.android.tools:desugar_jdk_libs:2.0.4")
}
