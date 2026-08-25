plugins {
    id("com.android.application")
    id("kotlin-android")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

android {
    namespace = "com.example.ceyx_example"
    compileSdk = flutter.compileSdkVersion
    ndkVersion = flutter.ndkVersion

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = JavaVersion.VERSION_17.toString()
    }

    defaultConfig {
        // TODO: Specify your own unique Application ID (https://developer.android.com/studio/build/application-id.html).
        applicationId = "com.example.ceyx_example"
        // You can update the following values to match your application needs.
        // For more information, see: https://flutter.dev/to/review-gradle-config.
        minSdk = 24
        targetSdk = flutter.targetSdkVersion
        versionCode = flutter.versionCode
        versionName = flutter.versionName

        ndk {
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                arguments(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_PLATFORM=android-24",
                    "-DDNG_CROSS_BUILD=ON",
                    "-DDNG_PREBUILT_AOT_DIR=${file("../../../native/build-android/host-generators/halide_generated").absolutePath}"
                )
                // The prebuilt Halide AOT .a archives are arm64-only, so the
                // CMake/externalNativeBuild must run for arm64-v8a exclusively
                // (the defaultConfig ndk.abiFilters only filters packaging, not
                // the native build ABIs). Without this, Gradle also configures
                // armeabi-v7a and fails linking the arm64 .a (armelf_linux_eabi).
                abiFilters("arm64-v8a")
                // Only the APK needs the shared library; the CMakeLists also
                // defines cross-compile test/measurement executables that should
                // not be part of the APK build.
                targets("dng_decoder_native")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../../../native/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            // TODO: Add your own signing config for the release build.
            // Signing with the debug keys for now, so `flutter run --release` works.
            signingConfig = signingConfigs.getByName("debug")
            externalNativeBuild {
                cmake {
                    arguments("-DCMAKE_BUILD_TYPE=Release")
                }
            }
        }
        debug {
            externalNativeBuild {
                cmake {
                    arguments("-DCMAKE_BUILD_TYPE=Debug")
                }
            }
        }
    }
}

flutter {
    source = "../.."
}
