plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.py1212.yukkuri"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.py1212.yukkuri"
        minSdk = 23
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DCMAKE_BUILD_TYPE=Release")
                cFlags += listOf("-O2", "-ffast-math", "-std=c99")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
        debug {
            isJniDebuggable = false
        }
    }

    packaging {
        // 8MB の辞書は圧縮せずそのまま (起動時の展開コストを避ける)
        jniLibs.useLegacyPackaging = false
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.constraintlayout:constraintlayout:2.1.4")
}

// 読み辞書 (assets/reading_dict.bin) が無ければ kuromoji から生成する。
// 生成には node が必要 (CI では setup-node 済み)。
val buildReadingDict by tasks.registering(Exec::class) {
    val out = layout.projectDirectory.file("src/main/assets/reading_dict.bin").asFile
    val dicts = rootProject.file("node_modules/kuromoji/dict")
    inputs.dir(dicts)
    outputs.file(out)
    onlyIf { !out.exists() }
    workingDir = rootProject.projectDir
    commandLine("node", "scripts/build-dict.mjs")
}

tasks.named("preBuild") { dependsOn(buildReadingDict) }
