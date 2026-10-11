// Top-level build file. The game is pure-native (NDK + Vulkan); the only JVM
// code is the instrumented smoke test.
plugins {
  alias(libs.plugins.android.application) apply false
}
