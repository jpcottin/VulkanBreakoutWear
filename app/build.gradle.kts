plugins {
  alias(libs.plugins.android.application)
}

// Push and run the native Google Test binary on the connected device/emulator.
// Usage: ./gradlew runNativeTests
tasks.register("runNativeTests") {
    dependsOn("externalNativeBuildDebug")
    // ABI defaults to arm64-v8a (local device); override with -PtestAbi=x86_64 for CI.
    val abi = (project.findProperty("testAbi") as String?) ?: "arm64-v8a"
    val intermediates = layout.buildDirectory.dir("intermediates").get().asFile
    doLast {
        // AGP writes the binary under intermediates/cxx/<variant>/<hash>/obj/<abi>/
        // (the hash changes with the native config) and may leave a stale copy
        // in the legacy intermediates/cmake/debug/obj/ tree: take the newest.
        val binFile = intermediates.walkTopDown()
            .filter { it.isFile && it.name == "game_tests" && it.parentFile.name == abi }
            .maxByOrNull { it.lastModified() }
            ?: throw GradleException("Native test binary for ABI $abi not found under $intermediates. " +
                    "Ensure externalNativeBuildDebug has run for ABI $abi.")
        val binPath = binFile.absolutePath
        println("Using native test binary: $binPath")
        fun adb(vararg args: String) {
            println("Executing: adb ${args.joinToString(" ")}")
            val rc = ProcessBuilder("adb", *args).inheritIO().start().waitFor()
            if (rc != 0) {
                throw GradleException("adb ${args.toList()} exited $rc")
            }
        }
        adb("push", binPath, "/data/local/tmp/game_tests")
        adb("shell", "chmod", "+x", "/data/local/tmp/game_tests")
        adb("shell", "/data/local/tmp/game_tests")
    }
}

android {
    namespace = "com.jpcottin.vulkanbreakoutwear"
    compileSdk = 36
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "com.jpcottin.vulkanbreakoutwear"
        // Wear OS 3 and up; Vulkan itself only needs API 24.
        minSdk = 30
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                arguments += "-DANDROID_STL=c++_static"
            }
        }
        ndk {
            // Emulator is x86_64. Pixel Watches run a 32-bit userspace
            // (armeabi-v7a only); arm64-v8a covers 64-bit watches.
            abiFilters += listOf("armeabi-v7a", "arm64-v8a", "x86_64")
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
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures {
        compose = false
        aidl = false
        buildConfig = false
    }
}

dependencies {
    androidTestImplementation(libs.androidx.test.runner)
    androidTestImplementation(libs.androidx.test.ext.junit)
}
