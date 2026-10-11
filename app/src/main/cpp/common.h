#pragma once
#include <android/log.h>
#include <cstdint>

#define LOG_TAG "BreakoutWear"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Drawable primitives the renderer knows how to draw. Everything in the game
// (bricks, paddle, ball, HUD, text) is built from these two.
enum Shape {
    SHAPE_QUAD = 0,        // [-1,1] square: bricks, paddle, HUD, digit segments, text strokes
    SHAPE_DISC,            // unit circle (32-gon): ball, bonus capsules, glows, gear
    SHAPE_COUNT
};

// Fragment-shader fill styles (DrawCmd::style).
enum FillStyle {
    STYLE_FLAT = 0,   // solid colour (default)
    STYLE_GLOW = 1,   // radial alpha falloff — soft glows and the ball halo
};

// One draw: a 2x2 linear transform + NDC translation + RGBA colour, applied to
// a shape. The renderer repacks these into per-instance vertex attributes
// (InstanceData in vk_renderer.cpp) consumed by shape.vert/.frag.
struct DrawCmd {
    float mtx[4] = {};       // m00, m01, m10, m11
    float tx = 0.0f, ty = 0.0f;   // NDC translation
    float color[4] = {};     // r, g, b, a
    float style = 0.0f;      // FillStyle (as float — goes straight into the instance attributes)
    float seed = 0.0f;       // reserved per-draw parameter (unused by current styles)
    int shape = 0;
};
