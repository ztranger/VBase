plugins {
    id("com.android.application")
}

android {
    namespace = "com.hpg.vbase"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.hpg.vbase"
        minSdk = 30 // API 30: нативный AImageDecoder для загрузки текстур (Vulkan 1.1 тоже ок)
        targetSdk = 36
        versionCode = 1
        versionName = "0.1"

        ndk {
            // arm64 — реальные устройства, x86_64 — эмулятор
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
    }

    buildFeatures {
        // Prefab даёт доступ к нативным библиотекам из AAR (game-activity)
        prefab = true
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

dependencies {
    // GameActivity: жизненный цикл и ввод доставляются прямо в нативный код
    implementation("androidx.games:games-activity:3.0.5")
    // GameActivity наследует AppCompatActivity, а games-activity POM
    // не тянет appcompat транзитивно — добавляем явно (нужен и для темы Theme.AppCompat)
    implementation("androidx.appcompat:appcompat:1.6.1")
}
