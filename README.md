# Vulkan Breakout Wear

[![CI](https://github.com/jpcottin/VulkanBreakoutWear/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/jpcottin/VulkanBreakoutWear/actions/workflows/ci.yml)

A 2D **Breakout-like** game for **Wear OS**, rendered with **Vulkan**, played
with the **crown**.

Turn the crown to slide the paddle, keep the ball in play and break the wall
of coloured bricks on a round watch face. Sibling project of
[VulkanBreakout](https://github.com/jpcottin/VulkanBreakout) — the phone
game — with the same hand-written renderer, procedural audio engine and CI
architecture, laid out for a disc and driven by a rotary encoder.

> **Status:** engine, gameplay, build, tests and CI are in. The Auto Play
> autopilot lands in the next pull request.

## Gameplay

| Control | Action |
|---------|--------|
| Turn the **crown** | The paddle slides one step per detent, clockwise to the right |
| Drag a finger anywhere | The paddle follows the finger (the crown takes over again on its next turn) |
| Tap anywhere | Launch a ball resting on the paddle (it goes by itself after 1.5 s) |
| Tap the **gear** (bottom of the face) | Open Settings from the title and end screens |
| Tap anywhere on the title / end screen | Start / return to title |

- **Goal:** break every brick in the wall before you run out of balls.
- **The round screen:** the playfield is a lane whose corners clear the rim —
  five big columns of bricks across, the paddle line low in the disc, the
  score and the level at the top, the balls left as dots at the bottom.
  Nothing is drawn past the rim.
- **Progression:** 10 short levels, easy to hard. Level 1 is 3 rows of plain
  bricks; the wall grows to 6 rows by level 10, with **tough** bricks (two
  hits) from level 3, unbreakable **steel** blocks in the way from level 5,
  **armoured** bricks (three hits) from level 7 and holes in the bottom row
  on some levels. The ball starts faster on every level and **speeds up as
  the wall thins** (up to +20%).
- **Bricks:** one bright colour per row (red, orange, yellow, green, cyan,
  blue) with a bevelled top and a dark bottom edge; bricks that take more
  than one hit show an inner core per hit left; steel is grey with rivets.
  Plain 10 pts · tough 20 pts · armoured 30 pts — all ×level.
- **Aim with the paddle:** where the ball meets the paddle sets the rebound —
  the centre sends it (almost) straight up, the tips 65° out. The ball never
  leaves the paddle perfectly vertical and never shuttles near-horizontally
  after a bounce, so there are no dead loops.
- **Bonuses:** 12% drop chance on each broken brick: 🔵 **Wide paddle**
  (+55% width, 10 s) · 🟣 **Slow ball** (×0.6 speed, 8 s) · 🟠 **Multi-ball**
  (every ball in play splits into three, up to six) · 🟢 **Extra life** (rare,
  up to 9). +25 pts ×level on pickup; the timed bonuses show as small bars
  under the paddle line and end with the ball.
- **Lives:** 5 balls. A ball that passes the paddle is gone — a fresh one is
  served on the paddle. Losing one of several balls costs nothing. The game
  ends when the last ball is lost.
- **Level clear:** +100 pts ×level. Clearing level 10 wins the game.
- **High scores:** top-5 leaderboard persisted on the watch; gold/silver/
  bronze podium on the title screen; gold pulsing score + rank medal on game
  over when a new record is set.
- **Screen shake** and a **haptic pulse** (50 ms) when a ball is lost.
- **Sound:** all effects synthesised in real time — paddle blip, wall tick,
  brick click, a brick-break pop pitched by row (the wall is a keyboard, top
  row highest), ball-lost slide, level-clear and extra-life jingles, pickup
  sparkle, and a 92 BPM ambient bed.
- **Settings:** gear icon opens an overlay from the title and end screens.
  Toggles: Sound on/off. Persists across app restarts.
- **Resumes after process death:** level, score and lives are saved with the
  activity state; the wall comes back fresh.

## Tech

- **Pure native C++17** — Android
  [`NativeActivity`](https://developer.android.com/ndk/reference/group/native-activity)
  with `native_app_glue`; no Kotlin, no Compose, no Java (the only Kotlin file
  is the instrumented smoke test). A standalone Wear OS app: no phone
  companion.
- **The crown over the NDK** — the rotary encoder arrives as a motion event
  from `AINPUT_SOURCE_ROTARY_ENCODER` with the distance on
  `AMOTION_EVENT_AXIS_SCROLL`, one event per detent; the game turns detents
  into paddle steps.
- **Hand-written Vulkan 1.0 2D renderer** — one graphics pipeline, one vertex
  buffer holding two shapes (a quad and a disc), per-instance transform +
  translation + RGBA + fill style, FIFO present, two frames in flight.
  Consecutive draws of the same shape batch into one instanced draw call.
- **Everything is geometry, no textures** — bricks are bevelled quads, the
  paddle a rounded bar, the ball a disc under a radial-falloff glow. Stroke
  vector font for text; 7-segment font for numbers.
- **Procedural audio via [Oboe](https://github.com/google/oboe)** — an 8-voice
  synth on a low-latency mono float stream: paddle blip, wall tick, brick
  click, pitched brick-break pop, ball-lost slide, level-clear and extra-life
  jingles, pickup sparkle, and a 92 BPM ambient bed. No audio files.
- **GLSL → SPIR-V** compiled at build time with the NDK's `glslc` (`-mfmt=c`)
  and `#include`d directly as C arrays — no runtime shader compiler, no assets.
- **Battery-aware** — the title and end screens run at half rate, and the
  game pauses the moment the activity loses focus (wrist down, a
  notification).
- **Runtime diagnostics via Logcat** — every launch logs a full Vulkan
  extension audit and a periodic FPS line. Filter with
  `adb logcat -s BreakoutWear`.
- `minSdk 30` (Wear OS 3), AGP 9, NDK r29, CMake 3.22.

## Build & run

```bash
# Build
./gradlew assembleDebug

# Install and launch on a Wear OS emulator or watch
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.jpcottin.vulkanbreakoutwear/android.app.NativeActivity
```

Requires a Wear OS device with a Vulkan driver. On the emulator use a Wear OS
4 or 5 image (API 33 to 35, `android-wear`); the newer API 36 and 37 images
(`android-wear-signed`) ship a Mesa-based Vulkan driver that cannot create an
instance on the current emulator (it looks for a virtio-gpu render node the
emulator does not expose), so the game exits at once with "Vulkan unavailable"
there.
On the emulator the crown is the rotary input in the extended controls; from
a shell, one detent is

```bash
adb shell sendevent /dev/input/event13 2 8 1 && adb shell sendevent /dev/input/event13 0 0 0
```

(`getevent -pl` names the rotary device; `2 8` is `EV_REL REL_WHEEL`, `1`
clockwise, `-1` anticlockwise).

## Testing

### Native unit tests (Google Test)

66 tests covering the wall (five columns in the lane, rows per level, every
wall inside the disc, tough/armoured/steel bricks arriving with the levels,
holes, level clamp at 10), serve and launch (grace, tap, self-launch, the
held ball following the paddle), the crown (one detent one step, clamped to
the lane, gliding rather than jumping) and the finger (drag, hand-over to
the crown, cancel), the huge-frame dt clamp, ball physics (lane walls,
ceiling, paddle bounce and its angle by hit position, never perfectly
vertical, lost ball costs a life and re-serves, game over on the last life,
brick breaking and scoring, two-hit bricks, steel, side hits reflecting
horizontally, speed ramp, per-level speed, no tunnelling at the clamped
worst-case dt), bonuses (capsule fall and pickup, fall-through, wide paddle
and its clamp-back on expiry, slow ball, multi-ball split and cap, extra life
up to nine, losing one of several balls, bonuses ending with the ball),
progression (level-scaled clear bonus, in-flight wipe on clear, win at level
10, game over grace tap, title-return cleanup, no gear during a round),
process-death session restore (bounds-checked), high-score persistence
(ordering, displacement, cap, save and reload, zero-score guard, tie guard),
the settings state machine (gear tap, sound toggle, back button, no gear
inside Settings, persistence), the title demo (ball inside the lane, the
crown moving the demo paddle) and render coverage of every reachable state
with every shape inside the disc.

They build and run on the host in a few seconds — no device, no emulator:

```bash
scripts/host-tests.sh                              # all tests
scripts/host-tests.sh --gtest_filter='Crown.*'     # a subset
```

The same binary runs on a connected watch or emulator through Gradle:

```bash
./gradlew runNativeTests                   # ARM watch (default)
./gradlew runNativeTests -PtestAbi=x86_64  # x86_64 emulator
ANDROID_SERIAL=emulator-5556 ./gradlew runNativeTests   # several devices? pin one
```

### Instrumented smoke test

Launches the `NativeActivity` on a connected watch or emulator, waits for the
renderer to present its first frame, asserts the activity is still `RESUMED`,
and captures a screenshot:

```bash
./gradlew connectedAndroidTest
```

## CI/CD

GitHub Actions runs on every push and pull request to `main`; every job gates
merges.

| Job | What it does | Artifacts |
|-----|-------------|-----------|
| **Build APK** | Compiles the debug APK | `debug-apk` |
| **Host tests** | Builds `game.cpp` and the Google Test suite with the host compiler and runs it | — |
| **Native Tests** | Runs the same 66 Google Test cases on x86\_64 Wear OS emulators (API 33 + API 34, `wearos_large_round`) | — |
| **Smoke Test** | Runs the Android instrumented test on the same Wear OS emulators and captures an in-game screenshot. The test asserts the activity is RESUMED **and** that the renderer logged `First frame presented`, so a dead Vulkan path fails loudly | `smoke-screenshot-api*`, `smoke-test-results-api*`, `smoke-logcat-api*` |

## License

[Apache License 2.0](LICENSE).
