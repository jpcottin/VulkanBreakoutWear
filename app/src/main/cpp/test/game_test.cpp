#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>

// android/log.h is available on-device; the test binary links liblog.
#include "game.h"

// ── helpers ───────────────────────────────────────────────────────────────────

// A round watch: a square viewport, the disc of radius 1 inscribed in it.
static const int kW = 480;
static const int kH = 480;

// World -> pixel, matching Game's mapping (x in [-1,1], y in [-1,1]).
static float pxOf(float wx) { return (wx + 1.0f) * 0.5f * kW; }
static float pyOf(float wy) { return (wy + 1.0f) * 0.5f * kH; }

static void step(Game& g, float seconds, float dt = 1.0f / 60.0f) {
    for (float t = 0.0f; t < seconds; t += dt) g.update(dt);
}

// Tap = press and release; the game consumes it on the next update().
static void tap(Game& g, float px, float py) {
    g.onPointerDown(0, px, py);
    g.onPointerUp(0);
}

static void openSettings(Game& g) {
    tap(g, pxOf(0.0f), pyOf(0.82f));   // gear, bottom of the disc
    g.update(0.016f);
}

static const char* scratchDir() {
    const char* dir = getenv("TMPDIR");
    if (!dir || !dir[0]) dir = "/data/local/tmp";
    return dir;
}

// Every shape must sit inside the disc: centre plus extent within radius 1.
// Full-screen overlays (extent near the whole square) are exempt.
static void expectInsideTheDisc(const std::vector<DrawCmd>& cmds, const char* what) {
    for (const auto& c : cmds) {
        float ex = fabsf(c.mtx[0]) + fabsf(c.mtx[1]);
        float ey = fabsf(c.mtx[2]) + fabsf(c.mtx[3]);
        if (ex > 0.9f && ey > 0.9f) continue;
        // The far corner of the shape's bounding box.
        float cx = fabsf(c.tx) + ex, cy = fabsf(c.ty) + ey;
        EXPECT_LE(sqrtf(cx * cx + cy * cy), 1.02f)
            << what << ": shape past the rim at (" << c.tx << ", " << c.ty << ")";
    }
}

// ── Initial state ─────────────────────────────────────────────────────────────

TEST(InitialState, StartsOnTitle) {
    Game g;
    g.setViewport(kW, kH);
    EXPECT_TRUE(g.inTitleForTest());
    EXPECT_EQ(g.lives(), 5);
    EXPECT_EQ(g.score(), 0);
    EXPECT_EQ(g.level(), 1);
}

// Five big columns fit across the lane; the wall stays clear of the rim.
TEST(Wall, FiveColumnsAcrossTheLane) {
    Game g;
    g.setViewport(kW, kH);
    EXPECT_EQ(g.wallColsForTest(), 5);
    EXPECT_EQ(g.brickTotalForTest(), 3 * 5);     // the title demo wall: three rows
    for (int i = 0; i < g.brickTotalForTest(); i++) {
        float x = g.brickXForTest(i), y = g.brickYForTest(i);
        EXPECT_LT(fabsf(x) + 0.11f, 0.66f);        // inside the lane
        EXPECT_LT(sqrtf(x * x + y * y) + 0.12f, 1.0f);   // inside the disc
    }
}

// ── The crown ────────────────────────────────────────────────────────────────

TEST(Crown, OneDetentStepsThePaddle) {
    Game g;
    g.setViewport(kW, kH);
    g.onRotary(1.0f);
    EXPECT_NEAR(g.crownXForTest(), 0.10f, 1e-5f);
    g.onRotary(-3.0f);
    EXPECT_NEAR(g.crownXForTest(), -0.20f, 1e-5f);
}

TEST(Crown, StaysInsideTheLane) {
    Game g;
    g.setViewport(kW, kH);
    for (int i = 0; i < 40; i++) g.onRotary(1.0f);
    EXPECT_LE(g.crownXForTest() + 0.16f, 0.66f + 1e-5f);   // paddle edge at the wall
    for (int i = 0; i < 80; i++) g.onRotary(-1.0f);
    EXPECT_GE(g.crownXForTest() - 0.16f, -0.66f - 1e-5f);
}

TEST(Crown, TurningItMovesTheTitlePaddle) {
    Game g;
    g.setViewport(kW, kH);
    for (int i = 0; i < 4; i++) g.onRotary(1.0f);   // crown says 0.40
    step(g, 0.5f);
    EXPECT_NEAR(g.demoPaddleXForTest(), 0.40f, 0.02f);
}

TEST(Crown, TitlePaddleGoesBackToChasingTheBallWhenTheCrownRests) {
    Game g;
    g.setViewport(kW, kH);
    for (int i = 0; i < 4; i++) g.onRotary(1.0f);
    step(g, 0.5f);
    ASSERT_NEAR(g.demoPaddleXForTest(), 0.40f, 0.02f);
    step(g, 3.0f);                                   // the crown has rested
    EXPECT_NEAR(g.demoPaddleXForTest(), g.demoBallXForTest(), 0.08f);
}

// ── Title demo ───────────────────────────────────────────────────────────────

TEST(Title, DemoBallStaysInsideTheLane) {
    Game g;
    g.setViewport(kW, kH);
    for (int i = 0; i < 60 * 20; i++) {
        g.update(1.0f / 60.0f);
        ASSERT_LE(fabsf(g.demoBallXForTest()) + 0.03f, 0.66f + 1e-4f);
        ASSERT_LT(g.demoBallYForTest(), 0.60f);      // never below the paddle line
    }
}

TEST(Update, ClampsHugeDtHitch) {
    // A giant frame is clamped to 0.05 s so the demo ball can't warp through a
    // wall on a hitch: a 1 s frame must move it exactly like a 0.05 s one.
    Game huge; huge.setViewport(kW, kH);
    Game ref;  ref.setViewport(kW, kH);
    huge.update(1.0f);
    ref.update(0.05f);
    EXPECT_FLOAT_EQ(huge.demoBallXForTest(), ref.demoBallXForTest());
    EXPECT_FLOAT_EQ(huge.demoBallYForTest(), ref.demoBallYForTest());
}

// ── Settings ─────────────────────────────────────────────────────────────────

TEST(Settings, GearTapOpensSettingsFromTitle) {
    Game g;
    g.setViewport(kW, kH);
    openSettings(g);
    EXPECT_TRUE(g.inSettingsForTest());
}

TEST(Settings, SoundRowToggles) {
    Game g;
    g.setViewport(kW, kH);
    openSettings(g);
    ASSERT_TRUE(g.inSettingsForTest());
    ASSERT_TRUE(g.soundEnabledForTest());
    tap(g, pxOf(0.0f), pyOf(-0.22f));           // sound row
    g.update(0.016f);
    EXPECT_FALSE(g.soundEnabledForTest());
    tap(g, pxOf(0.0f), pyOf(-0.22f));
    g.update(0.016f);
    EXPECT_TRUE(g.soundEnabledForTest());
}

TEST(Settings, BackReturnsToPreviousState) {
    Game g;
    g.setViewport(kW, kH);
    openSettings(g);
    ASSERT_TRUE(g.inSettingsForTest());
    tap(g, pxOf(0.0f), pyOf(0.56f));            // back button
    g.update(0.016f);
    EXPECT_TRUE(g.inTitleForTest());
}

TEST(Settings, GearTapIsIgnoredInsideSettings) {
    Game g;
    g.setViewport(kW, kH);
    openSettings(g);
    ASSERT_TRUE(g.inSettingsForTest());
    openSettings(g);                            // the gear is not drawn here
    EXPECT_TRUE(g.inSettingsForTest());
}

TEST(Settings, PersistAcrossInstances) {
    const char* dir = scratchDir();
    char path[512];
    snprintf(path, sizeof(path), "%s/settings.bin", dir);
    remove(path);

    {
        Game g;
        g.setViewport(kW, kH);
        g.setDataPath(dir);
        openSettings(g);
        tap(g, pxOf(0.0f), pyOf(-0.22f));       // sound off
        g.update(0.016f);
        ASSERT_FALSE(g.soundEnabledForTest());
    }
    {
        Game g;
        g.setViewport(kW, kH);
        g.setDataPath(dir);
        EXPECT_FALSE(g.soundEnabledForTest());
    }
    remove(path);
}

// ── Render coverage ───────────────────────────────────────────────────────────

// Drive render() through every reachable state and require output that
// stays inside the disc, so a crash or a clipped element shows up here
// instead of on a watch.
TEST(Render, ProducesOutputInsideTheDiscInEveryReachableState) {
    Game g;
    g.setViewport(kW, kH);
    std::vector<DrawCmd> cmds;

    step(g, 0.5f);
    g.render(cmds);                                    // TITLE (+ demo wall)
    EXPECT_GT(cmds.size(), 0u) << "TITLE drew nothing";
    expectInsideTheDisc(cmds, "TITLE");
    size_t titleCmds = cmds.size();

    openSettings(g);
    cmds.clear(); g.render(cmds);                      // SETTINGS overlay
    EXPECT_GT(cmds.size(), titleCmds) << "SETTINGS drew nothing on top";
    expectInsideTheDisc(cmds, "SETTINGS");
}
