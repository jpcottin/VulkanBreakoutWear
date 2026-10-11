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

// Geometry shared with game.cpp.
static const float kFieldHW  = 0.66f;
static const float kPaddleY  = 0.60f;
static const float kPaddleHW = 0.160f;
static const float kTopWallY = -0.60f;
static const float kBallR    = 0.030f;

static void step(Game& g, float seconds, float dt = 1.0f / 60.0f) {
    for (float t = 0.0f; t < seconds; t += dt) g.update(dt);
}

static void startPlaying(Game& g) {
    g.setViewport(kW, kH);
    g.triggerNewGameForTest();
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

// Index of the brick at (row, col) — bricks_ is built row-major.
static int slot(const Game& g, int row, int col) { return row * g.wallColsForTest() + col; }

// Drop the ball into the pit, away from the paddle: one life gone.
static void loseABall(Game& g) {
    g.setBallForTest(0.45f, 0.68f, 0.0f, 1.0f);
    step(g, 0.3f);
}

// Drive a game straight to a game-over at the given score, which saves the
// high-score table to disk. ASSERTs live in a void helper (legal in gtest).
static void gameOverAtScore(Game& g, long s) {
    ASSERT_TRUE(g.isPlayingForTest());
    g.setScoreForTest(s);
    g.setLivesForTest(1);
    loseABall(g);
    ASSERT_TRUE(g.isGameOverForTest());
}

static const char* scratchDir() {
    const char* dir = getenv("TMPDIR");
    if (!dir || !dir[0]) dir = "/data/local/tmp";
    return dir;
}

// Every shape must sit inside the disc: the far corner of its bounding box
// within radius 1. Full-screen overlays (extent near the whole square) are
// exempt.
static void expectInsideTheDisc(const std::vector<DrawCmd>& cmds, const char* what) {
    for (const auto& c : cmds) {
        float ex = fabsf(c.mtx[0]) + fabsf(c.mtx[1]);
        float ey = fabsf(c.mtx[2]) + fabsf(c.mtx[3]);
        if (ex > 0.9f && ey > 0.9f) continue;
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
}

TEST(InitialState, TapStartsGame) {
    Game g;
    g.setViewport(kW, kH);
    tap(g, kW * 0.5f, kH * 0.5f);
    g.update(0.016f);
    EXPECT_TRUE(g.isPlayingForTest());
    EXPECT_EQ(g.lives(), 5);
    EXPECT_EQ(g.score(), 0);
    EXPECT_EQ(g.level(), 1);
    EXPECT_TRUE(g.ballHeldForTest());             // the ball waits on the paddle
    EXPECT_EQ(g.ballCount(), 1);
    EXPECT_FLOAT_EQ(g.paddleX(), 0.0f);
}

// ── The wall ──────────────────────────────────────────────────────────────────

TEST(Wall, FiveColumnsAcrossTheLane) {
    Game g;
    g.setViewport(kW, kH);
    EXPECT_EQ(g.wallColsForTest(), 5);
    EXPECT_EQ(g.brickTotalForTest(), 3 * 5);     // the title demo wall: three rows
}

TEST(Wall, Level1IsThreeRowsOfNormalBricks) {
    Game g;
    startPlaying(g);
    EXPECT_EQ(g.wallRowsForTest(), 3);
    EXPECT_EQ(g.brickTotalForTest(), 15);
    EXPECT_EQ(g.breakableLeftForTest(), 15);
    for (int i = 0; i < g.brickTotalForTest(); i++) {
        EXPECT_TRUE(g.brickAliveForTest(i));
        EXPECT_EQ(g.brickTypeForTest(i), (int)Game::BRICK_NORMAL);
        EXPECT_EQ(g.brickHpForTest(i), 1);
    }
}

TEST(Wall, RowsGrowWithLevel) {
    Game g;
    g.setViewport(kW, kH);
    const int expected[10] = {3, 3, 4, 4, 4, 5, 5, 5, 6, 6};
    for (int L = 1; L <= 10; L++) {
        g.startLevelForTest(L);
        EXPECT_EQ(g.wallRowsForTest(), expected[L - 1]) << "level " << L;
        EXPECT_EQ(g.level(), L);
    }
}

// Every wall sits inside the lane, below the ceiling, above the middle of
// the disc, and clear of the rim.
TEST(Wall, EveryWallFitsTheDisc) {
    Game g;
    g.setViewport(kW, kH);
    for (int L = 1; L <= 10; L++) {
        g.startLevelForTest(L);
        for (int i = 0; i < g.brickTotalForTest(); i++) {
            float x = g.brickXForTest(i), y = g.brickYForTest(i);
            EXPECT_LT(fabsf(x) + 0.11f, kFieldHW) << "level " << L;
            EXPECT_GT(y - 0.045f, kTopWallY) << "level " << L;
            EXPECT_LT(y + 0.045f, 0.15f) << "level " << L;
            float cx = fabsf(x) + 0.11f, cy = fabsf(y) + 0.045f;
            EXPECT_LT(sqrtf(cx * cx + cy * cy), 1.0f) << "level " << L;
        }
    }
}

TEST(Wall, TougherBricksArriveWithTheLevels) {
    Game g;
    g.setViewport(kW, kH);
    g.startLevelForTest(3);
    EXPECT_EQ(g.brickTypeForTest(slot(g, 0, 0)), (int)Game::BRICK_TOUGH);
    EXPECT_EQ(g.brickHpForTest(slot(g, 0, 0)), 2);
    EXPECT_EQ(g.brickTypeForTest(slot(g, 1, 0)), (int)Game::BRICK_NORMAL);
    g.startLevelForTest(7);
    EXPECT_EQ(g.brickTypeForTest(slot(g, 0, 0)), (int)Game::BRICK_ARMOR);
    EXPECT_EQ(g.brickHpForTest(slot(g, 0, 0)), 3);
    EXPECT_EQ(g.brickTypeForTest(slot(g, 1, 0)), (int)Game::BRICK_TOUGH);
}

TEST(Wall, SteelDoesNotCountTowardTheClear) {
    Game g;
    g.setViewport(kW, kH);
    g.startLevelForTest(5);                   // row 2, col 2: steel
    int steel = 0;
    for (int i = 0; i < g.brickTotalForTest(); i++)
        if (g.brickTypeForTest(i) == (int)Game::BRICK_STEEL) steel++;
    EXPECT_EQ(steel, 1);
    EXPECT_EQ(g.brickTypeForTest(slot(g, 2, 2)), (int)Game::BRICK_STEEL);
    EXPECT_EQ(g.breakableLeftForTest(), g.brickTotalForTest() - steel);
}

TEST(Wall, Level4BottomRowHasHoles) {
    Game g;
    g.setViewport(kW, kH);
    g.startLevelForTest(4);
    int bottom = g.wallRowsForTest() - 1;
    EXPECT_TRUE(g.brickAliveForTest(slot(g, bottom, 0)));
    EXPECT_FALSE(g.brickAliveForTest(slot(g, bottom, 1)));
    EXPECT_TRUE(g.brickAliveForTest(slot(g, bottom, 2)));
}

TEST(Wall, LevelClampsAtTen) {
    Game g;
    g.setViewport(kW, kH);
    g.startLevelForTest(11);
    EXPECT_EQ(g.level(), 10);
    EXPECT_EQ(g.wallRowsForTest(), 6);
}

// ── Serve and launch ─────────────────────────────────────────────────────────

TEST(Serve, EarlyTapIsIgnored) {
    Game g;
    startPlaying(g);
    tap(g, kW * 0.5f, kH * 0.5f);               // right at the serve
    g.update(1.0f / 60.0f);
    EXPECT_TRUE(g.ballHeldForTest());
}

TEST(Serve, TapLaunchesAfterTheGrace) {
    Game g;
    startPlaying(g);
    step(g, 0.6f);
    ASSERT_TRUE(g.ballHeldForTest());
    tap(g, kW * 0.5f, kH * 0.5f);
    g.update(1.0f / 60.0f);
    EXPECT_FALSE(g.ballHeldForTest());
    EXPECT_LT(g.ballDyForTest(0), -0.5f);       // upward
    EXPECT_GT(fabsf(g.ballDxForTest(0)), 0.2f); // and to one side
}

TEST(Serve, BallLaunchesByItselfWhenNobodyTaps) {
    Game g;
    startPlaying(g);
    step(g, 1.2f);
    EXPECT_TRUE(g.ballHeldForTest());           // still waiting
    step(g, 0.5f);
    EXPECT_FALSE(g.ballHeldForTest());          // 1.5 s: off it goes
}

TEST(Serve, HeldBallFollowsThePaddle) {
    Game g;
    startPlaying(g);
    for (int i = 0; i < 3; i++) g.onRotary(1.0f);
    step(g, 0.3f);
    ASSERT_TRUE(g.ballHeldForTest());
    EXPECT_NEAR(g.paddleX(), 0.30f, 0.02f);
    EXPECT_NEAR(g.ballXForTest(0), g.paddleX(), 1e-5f);
}

// ── Paddle control: the crown and the finger ─────────────────────────────────

TEST(Crown, OneDetentOneStep) {
    Game g;
    startPlaying(g);
    g.onRotary(1.0f);
    step(g, 0.3f);
    EXPECT_NEAR(g.paddleX(), 0.10f, 0.01f);
    g.onRotary(-3.0f);
    step(g, 0.3f);
    EXPECT_NEAR(g.paddleX(), -0.20f, 0.01f);
}

TEST(Crown, PaddleStaysInsideTheLane) {
    Game g;
    startPlaying(g);
    for (int i = 0; i < 40; i++) g.onRotary(1.0f);
    step(g, 1.0f);
    EXPECT_LE(g.paddleX() + kPaddleHW, kFieldHW + 1e-5f);
    for (int i = 0; i < 80; i++) g.onRotary(-1.0f);
    step(g, 1.0f);
    EXPECT_GE(g.paddleX() - kPaddleHW, -kFieldHW - 1e-5f);
}

TEST(Crown, PaddleGlidesRatherThanJumps) {
    Game g;
    startPlaying(g);
    for (int i = 0; i < 6; i++) g.onRotary(1.0f);   // target 0.60 of the lane
    g.update(1.0f / 60.0f);
    EXPECT_LT(g.paddleX(), 0.10f);               // one frame: not there yet
    step(g, 0.5f);
    EXPECT_NEAR(g.paddleX(), 0.50f, 0.01f);      // clamped to the lane: 0.66 - 0.16
}

TEST(Finger, DragMovesThePaddle) {
    Game g;
    startPlaying(g);
    g.onPointerDown(0, pxOf(0.30f), pyOf(0.0f));   // anywhere on the face
    step(g, 0.5f);
    EXPECT_NEAR(g.paddleX(), 0.30f, 0.02f);
    g.onPointerMove(0, pxOf(-0.20f), pyOf(0.3f));
    step(g, 0.5f);
    EXPECT_NEAR(g.paddleX(), -0.20f, 0.02f);
}

TEST(Finger, CrownTakesOverAfterTheFingerLifts) {
    Game g;
    startPlaying(g);
    g.onPointerDown(0, pxOf(0.30f), pyOf(0.0f));
    step(g, 0.5f);
    g.onPointerUp(0);
    g.onRotary(-2.0f);                           // from 0.30 to 0.10
    step(g, 0.5f);
    EXPECT_NEAR(g.paddleX(), 0.10f, 0.02f);
}

TEST(Finger, CancelReleasesAllPointers) {
    Game g;
    startPlaying(g);
    g.onPointerDown(0, pxOf(0.40f), pyOf(0.0f));
    step(g, 0.05f);
    g.onPointersCancel();
    g.onRotary(-1.0f);                           // the crown must win now
    float target = g.paddleTargetForTest();
    step(g, 1.0f);
    EXPECT_NEAR(g.paddleX(), target, 0.01f);
    EXPECT_LT(g.paddleX(), 0.40f);
}

TEST(Update, ClampsHugeDtHitch) {
    // A giant frame is clamped to 0.05 s so the paddle can't warp on a hitch.
    // Same crown position: a 1 s frame must move it exactly like a 0.05 s one.
    Game huge; startPlaying(huge);
    Game ref;  startPlaying(ref);
    for (int i = 0; i < 5; i++) { huge.onRotary(1.0f); ref.onRotary(1.0f); }
    huge.update(1.0f);
    ref.update(0.05f);
    EXPECT_FLOAT_EQ(huge.paddleX(), ref.paddleX());
    EXPECT_GT(huge.paddleX(), 0.0f);
    EXPECT_LT(huge.paddleX(), 0.50f);
}

// ── Ball physics ─────────────────────────────────────────────────────────────

TEST(Ball, BouncesOffTheLaneWalls) {
    Game g;
    startPlaying(g);
    g.setBallForTest(kFieldHW - 0.06f, 0.30f, 1.0f, -0.3f);   // heading for the right wall
    step(g, 0.2f);
    EXPECT_LT(g.ballDxForTest(0), 0.0f);
    EXPECT_LE(g.ballXForTest(0) + kBallR, kFieldHW + 1e-4f);
    g.setBallForTest(-kFieldHW + 0.06f, 0.30f, -1.0f, -0.3f);
    step(g, 0.2f);
    EXPECT_GT(g.ballDxForTest(0), 0.0f);
}

TEST(Ball, BouncesOffTheCeiling) {
    Game g;
    startPlaying(g);
    int keep = slot(g, 2, 0);                   // one brick stays: the level goes on
    for (int i = 0; i < g.brickTotalForTest(); i++)
        if (i != keep) g.killBrickForTest(i);
    g.setBallForTest(0.30f, kTopWallY + 0.08f, 0.0f, -1.0f);
    step(g, 0.15f);
    EXPECT_GT(g.ballDyForTest(0), 0.0f);
    EXPECT_GE(g.ballYForTest(0) - kBallR, kTopWallY - 1e-4f);
}

TEST(Ball, BouncesOffThePaddle) {
    Game g;
    startPlaying(g);
    g.setBallForTest(0.0f, kPaddleY - 0.15f, 0.0f, 1.0f);   // falling onto the paddle
    step(g, 0.4f);
    EXPECT_LT(g.ballDyForTest(0), 0.0f);
    EXPECT_EQ(g.paddleHitsForTest(), 1);
    EXPECT_EQ(g.lives(), 5);
}

TEST(Ball, PaddleTipSendsTheBallSideways) {
    Game g;
    startPlaying(g);
    g.setBallForTest(0.14f, kPaddleY - 0.15f, 0.0f, 1.0f);   // near the right tip
    step(g, 0.4f);
    ASSERT_EQ(g.paddleHitsForTest(), 1);
    EXPECT_GT(g.ballDxForTest(0), 0.70f);
    EXPECT_LT(g.ballDyForTest(0), 0.0f);
}

TEST(Ball, PaddleCentreSendsItNearlyStraightUp) {
    Game g;
    startPlaying(g);
    g.setBallForTest(0.0f, kPaddleY - 0.15f, 0.0f, 1.0f);
    step(g, 0.4f);
    ASSERT_EQ(g.paddleHitsForTest(), 1);
    EXPECT_LT(fabsf(g.ballDxForTest(0)), 0.20f);
    EXPECT_GT(fabsf(g.ballDxForTest(0)), 0.10f);              // never exactly vertical
}

TEST(Ball, PassingThePaddleCostsALifeAndServesAgain) {
    Game g;
    startPlaying(g);
    loseABall(g);
    EXPECT_EQ(g.lives(), 4);
    EXPECT_EQ(g.ballsLostForTest(), 1);
    EXPECT_TRUE(g.isPlayingForTest());
    EXPECT_TRUE(g.ballHeldForTest());
    EXPECT_EQ(g.ballCount(), 1);
}

TEST(Ball, LosingTheLastLifeEndsTheGame) {
    Game g;
    startPlaying(g);
    for (int i = 0; i < 5; i++) loseABall(g);
    EXPECT_EQ(g.lives(), 0);
    EXPECT_TRUE(g.isGameOverForTest());
    EXPECT_EQ(g.ballCount(), 0);
}

TEST(Ball, BreaksABrickAndScores) {
    Game g;
    startPlaying(g);
    int target = slot(g, 2, 2);                 // bottom row, middle
    g.setBallForTest(g.brickXForTest(target), g.brickYForTest(target) + 0.15f, 0.0f, -1.0f);
    step(g, 0.4f);
    EXPECT_FALSE(g.brickAliveForTest(target));
    EXPECT_EQ(g.score(), 10L * g.level());
    EXPECT_EQ(g.bricksBrokenForTest(), 1);
    EXPECT_GT(g.ballDyForTest(0), 0.0f);        // and came back down
}

TEST(Ball, ToughBrickTakesTwoHits) {
    Game g;
    g.setViewport(kW, kH);
    g.startLevelForTest(3);                     // row 0 is tough
    int target = slot(g, 0, 2);
    for (int r = 1; r < g.wallRowsForTest(); r++) g.killBrickForTest(slot(g, r, 2));
    long before = g.score();
    g.setBallForTest(g.brickXForTest(target), g.brickYForTest(target) + 0.15f, 0.0f, -1.0f);
    step(g, 0.3f);
    EXPECT_TRUE(g.brickAliveForTest(target));
    EXPECT_EQ(g.brickHpForTest(target), 1);
    EXPECT_EQ(g.score(), before);               // a dent does not score
    g.setBallForTest(g.brickXForTest(target), g.brickYForTest(target) + 0.15f, 0.0f, -1.0f);
    step(g, 0.3f);
    EXPECT_FALSE(g.brickAliveForTest(target));
    EXPECT_EQ(g.score() - before, 20L * 3);
}

TEST(Ball, SteelBrickNeverBreaks) {
    Game g;
    g.setViewport(kW, kH);
    g.startLevelForTest(5);                     // row 2, col 2: steel
    int target = slot(g, 2, 2);
    ASSERT_EQ(g.brickTypeForTest(target), (int)Game::BRICK_STEEL);
    for (int r = 3; r < g.wallRowsForTest(); r++) g.killBrickForTest(slot(g, r, 2));
    for (int i = 0; i < 5; i++) {
        g.setBallForTest(g.brickXForTest(target), g.brickYForTest(target) + 0.15f, 0.0f, -1.0f);
        step(g, 0.3f);
        EXPECT_GT(g.ballDyForTest(0), 0.0f);    // bounced back
    }
    EXPECT_TRUE(g.brickAliveForTest(target));
    EXPECT_EQ(g.score(), 0);
}

TEST(Ball, SideHitReflectsHorizontally) {
    Game g;
    startPlaying(g);
    int target = slot(g, 2, 2);
    for (int i = 0; i < g.brickTotalForTest(); i++)
        if (i != target) g.killBrickForTest(i);
    g.setBallForTest(g.brickXForTest(target) - 0.25f, g.brickYForTest(target), 1.0f, 0.02f);
    step(g, 0.4f);
    EXPECT_FALSE(g.brickAliveForTest(target));
    EXPECT_LT(g.ballDxForTest(0), 0.0f);
}

TEST(Ball, SpeedsUpAsTheWallThins) {
    Game g;
    startPlaying(g);
    float base = g.ballSpeedForTest();
    int target = slot(g, 2, 2);
    for (int i = 0; i < g.brickTotalForTest(); i++)
        if (i != target) g.killBrickForTest(i);
    g.setBallForTest(g.brickXForTest(target), g.brickYForTest(target) + 0.15f, 0.0f, -1.0f);
    step(g, 0.3f);
    ASSERT_FALSE(g.brickAliveForTest(target));
    EXPECT_GT(g.ballSpeedForTest(), base * 1.15f);
}

TEST(Ball, FasterWithEveryLevel) {
    Game g;
    g.setViewport(kW, kH);
    g.startLevelForTest(1);
    float s1 = g.ballSpeedForTest();
    g.startLevelForTest(10);
    EXPECT_GT(g.ballSpeedForTest(), s1 * 1.5f);
}

// Regression guard: at the clamped worst-case dt (0.05 s) the ball moves
// more than its own diameter per frame. Substepped collision must keep it
// inside the lane and breaking bricks instead of tunnelling through them.
TEST(Ball, NeverTunnelsAtTheClampedWorstCaseDt) {
    Game g;
    startPlaying(g);
    g.setBallForTest(0.0f, 0.30f, 0.35f, -1.0f);
    for (int i = 0; i < 600; i++) {
        g.setPaddleXForTest(g.ballXForTest(0));   // the paddle is always underneath
        g.update(0.05f);
        ASSERT_EQ(g.ballCount(), 1) << "ball lost at frame " << i;
        ASSERT_LE(fabsf(g.ballXForTest(0)) + kBallR, kFieldHW + 1e-3f) << "frame " << i;
        ASSERT_GE(g.ballYForTest(0) - kBallR, kTopWallY - 1e-3f) << "frame " << i;
        if (!g.isPlayingForTest()) break;         // the wall is gone
    }
    EXPECT_GT(g.bricksBrokenForTest(), 3);
}

// ── Bonuses ──────────────────────────────────────────────────────────────────

TEST(Bonus, FallingCapsuleIsCollectedByThePaddle) {
    Game g;
    startPlaying(g);
    g.spawnTestPowerUp(g.paddleX(), 0.30f, (int)Game::PU_WIDE);
    step(g, 1.0f);
    EXPECT_TRUE(g.wideActiveForTest());
    EXPECT_EQ(g.score(), 25L * g.level());
    EXPECT_EQ(g.powerUpCountForTest(), 0);
}

TEST(Bonus, UncollectedCapsuleFallsPast) {
    Game g;
    startPlaying(g);
    g.spawnTestPowerUp(g.paddleX() + 0.45f, 0.30f, (int)Game::PU_WIDE);
    step(g, 3.0f);                              // (the ball launches itself meanwhile)
    EXPECT_FALSE(g.wideActiveForTest());
    EXPECT_EQ(g.powerUpCountForTest(), 0);
}

TEST(Bonus, WidePaddleWidensThenExpires) {
    Game g;
    startPlaying(g);
    float normal = g.paddleHalfWForTest();
    g.activatePowerUpForTest((int)Game::PU_WIDE);
    EXPECT_GT(g.paddleHalfWForTest(), normal * 1.4f);
    step(g, 11.0f);
    EXPECT_FALSE(g.wideActiveForTest());
    EXPECT_FLOAT_EQ(g.paddleHalfWForTest(), normal);
}

TEST(Bonus, WidePaddleExpiringNearTheWallIsClampedBack) {
    Game g;
    startPlaying(g);
    g.activatePowerUpForTest((int)Game::PU_WIDE);
    g.setPaddleXForTest(kFieldHW - 0.25f);      // flush with the wall, wide
    step(g, 11.0f);
    EXPECT_LE(g.paddleX() + kPaddleHW, kFieldHW + 1e-5f);
}

TEST(Bonus, SlowBallSlowsThenExpires) {
    Game g;
    startPlaying(g);
    float normal = g.ballSpeedForTest();
    g.activatePowerUpForTest((int)Game::PU_SLOW);
    EXPECT_NEAR(g.ballSpeedForTest(), normal * 0.6f, 1e-4f);
    step(g, 9.0f);                              // (the ball launches itself and may break bricks)
    EXPECT_FALSE(g.slowActiveForTest());
    EXPECT_GE(g.ballSpeedForTest(), normal);
}

TEST(Bonus, MultiBallSplitsTheBallInThree) {
    Game g;
    startPlaying(g);
    g.activatePowerUpForTest((int)Game::PU_MULTI);
    EXPECT_FALSE(g.ballHeldForTest());          // a held ball is launched first
    EXPECT_EQ(g.ballCount(), 3);
    for (int i = 0; i < 3; i++) EXPECT_LT(g.ballDyForTest(i), 0.0f);
    EXPECT_NE(g.ballDxForTest(1), g.ballDxForTest(2));
}

TEST(Bonus, MultiBallCapsAtSix) {
    Game g;
    startPlaying(g);
    g.activatePowerUpForTest((int)Game::PU_MULTI);
    g.activatePowerUpForTest((int)Game::PU_MULTI);
    g.activatePowerUpForTest((int)Game::PU_MULTI);
    EXPECT_EQ(g.ballCount(), 6);
}

TEST(Bonus, LosingOneOfSeveralBallsCostsNoLife) {
    Game g;
    startPlaying(g);
    int keep = slot(g, 2, 0);                    // one brick stays: the level goes on
    for (int i = 0; i < g.brickTotalForTest(); i++)
        if (i != keep) g.killBrickForTest(i);
    g.setBallForTest(0.0f, 0.20f, 0.0f, -1.0f);  // one ball flying up
    g.addBallForTest(0.45f, 0.68f, 0.0f, 1.0f);  // one dropping into the pit
    step(g, 0.3f);
    EXPECT_EQ(g.ballCount(), 1);
    EXPECT_EQ(g.lives(), 5);
}

TEST(Bonus, ExtraLifeAddsOneUpToNine) {
    Game g;
    startPlaying(g);
    g.spawnTestPowerUp(g.paddleX(), 0.30f, (int)Game::PU_LIFE);
    step(g, 1.0f);
    EXPECT_EQ(g.lives(), 6);
    for (int i = 0; i < 10; i++) g.activatePowerUpForTest((int)Game::PU_LIFE);
    EXPECT_EQ(g.lives(), 9);
}

TEST(Bonus, TimedBonusesEndWithTheBall) {
    Game g;
    startPlaying(g);
    g.activatePowerUpForTest((int)Game::PU_WIDE);
    g.activatePowerUpForTest((int)Game::PU_SLOW);
    loseABall(g);
    EXPECT_FALSE(g.wideActiveForTest());
    EXPECT_FALSE(g.slowActiveForTest());
}

// ── Level clear / win ─────────────────────────────────────────────────────────

TEST(Progression, ClearingTheWallPaysABonusAndAdvances) {
    Game g;
    startPlaying(g);
    g.spawnTestPowerUp(0.3f, 0.0f, (int)Game::PU_WIDE);
    g.setBallForTest(0.0f, 0.2f, 0.0f, -1.0f);
    long before = g.score();
    g.clearWallForTest();
    g.update(0.016f);
    EXPECT_TRUE(g.isLevelClearForTest());
    EXPECT_EQ(g.score() - before, 100L * 1);
    EXPECT_EQ(g.ballCount(), 0);
    EXPECT_EQ(g.powerUpCountForTest(), 0);
    step(g, 2.0f);
    EXPECT_TRUE(g.isPlayingForTest());
    EXPECT_EQ(g.level(), 2);
    EXPECT_TRUE(g.ballHeldForTest());
}

TEST(Progression, LevelClearBonusScalesWithLevel) {
    Game g;
    g.setViewport(kW, kH);
    g.startLevelForTest(9);
    long before = g.score();
    g.clearWallForTest();
    g.update(0.016f);
    ASSERT_TRUE(g.isLevelClearForTest());
    EXPECT_EQ(g.score() - before, 100L * 9);
}

TEST(Progression, ClearingLevelTenWins) {
    Game g;
    g.setViewport(kW, kH);
    g.startLevelForTest(10);
    g.setScoreForTest(500);
    g.clearWallForTest();
    g.update(0.016f);
    EXPECT_TRUE(g.isWinForTest());
    EXPECT_EQ(g.score(), 500 + 100L * 10);
    EXPECT_EQ(g.highScoreForTest(0), g.score());
}

TEST(Progression, GameOverTapReturnsToTitle) {
    Game g;
    startPlaying(g);
    g.setLivesForTest(1);
    loseABall(g);
    ASSERT_TRUE(g.isGameOverForTest());
    step(g, 1.0f);
    tap(g, kW * 0.5f, kH * 0.5f);
    g.update(0.016f);
    EXPECT_TRUE(g.inTitleForTest());
}

TEST(Progression, GameOverIgnoresEarlyTap) {
    Game g;
    startPlaying(g);
    g.setLivesForTest(1);
    loseABall(g);
    ASSERT_TRUE(g.isGameOverForTest());
    tap(g, kW * 0.5f, kH * 0.5f);               // within the 0.6 s grace
    g.update(0.016f);
    EXPECT_TRUE(g.isGameOverForTest());
    step(g, 0.7f);
    tap(g, kW * 0.5f, kH * 0.5f);               // after the grace
    g.update(0.016f);
    EXPECT_TRUE(g.inTitleForTest());
}

TEST(Progression, TitleReturnClearsLeftovers) {
    Game g;
    startPlaying(g);
    g.activatePowerUpForTest((int)Game::PU_WIDE);
    g.spawnTestPowerUp(0.2f, 0.0f, (int)Game::PU_SLOW);
    g.setLivesForTest(1);
    loseABall(g);
    ASSERT_TRUE(g.isGameOverForTest());
    ASSERT_EQ(g.powerUpCountForTest(), 1);
    step(g, 0.7f);
    tap(g, kW * 0.5f, kH * 0.5f);
    g.update(0.016f);
    ASSERT_TRUE(g.inTitleForTest());
    EXPECT_EQ(g.powerUpCountForTest(), 0);
    EXPECT_EQ(g.ballCount(), 0);
    EXPECT_FALSE(g.wideActiveForTest());
    EXPECT_EQ(g.brickTotalForTest(), 3 * 5);    // the demo wall is back
    EXPECT_EQ(g.level(), 1);
}

// The gear lives at the bottom of the disc on the title and end screens,
// never during a round: a tap there mid-round must not open Settings.
TEST(Progression, NoGearDuringARound) {
    Game g;
    startPlaying(g);
    tap(g, pxOf(0.0f), pyOf(0.82f));
    g.update(0.016f);
    EXPECT_TRUE(g.isPlayingForTest());
    g.setLivesForTest(1);
    loseABall(g);
    ASSERT_TRUE(g.isGameOverForTest());
    tap(g, pxOf(0.0f), pyOf(0.82f));
    g.update(0.016f);
    EXPECT_TRUE(g.inSettingsForTest());         // on the end screen it works
}

// ── Process-death resume ─────────────────────────────────────────────────────

TEST(RestoreSession, ResumesLevelScoreAndLives) {
    Game g;
    g.setViewport(kW, kH);
    g.restoreSession(5, 4200, 2);
    EXPECT_TRUE(g.isPlayingForTest());
    EXPECT_EQ(g.level(), 5);
    EXPECT_EQ(g.score(), 4200);
    EXPECT_EQ(g.lives(), 2);
    EXPECT_EQ(g.wallRowsForTest(), 4);
    EXPECT_TRUE(g.ballHeldForTest());
}

TEST(RestoreSession, RejectsOutOfRangeInput) {
    for (int badLevel : {0, 11}) {
        Game g; g.setViewport(kW, kH);
        g.restoreSession(badLevel, 100, 2);
        EXPECT_TRUE(g.inTitleForTest()) << "level=" << badLevel;
    }
    { Game g; g.setViewport(kW, kH); g.restoreSession(3, -1,  2); EXPECT_TRUE(g.inTitleForTest()); }
    { Game g; g.setViewport(kW, kH); g.restoreSession(3, 100, 0); EXPECT_TRUE(g.inTitleForTest()); }
    { Game g; g.setViewport(kW, kH); g.restoreSession(3, 100, 10); EXPECT_TRUE(g.inTitleForTest()); }
}

// ── High scores ──────────────────────────────────────────────────────────────

TEST(HighScores, TableOrdersDisplacesAndCaps) {
    Game g;
    startPlaying(g);   // dataPath unset: saves are no-ops, table is in-memory
    const long runs[6] = {300, 100, 500, 200, 400, 250};
    for (long r : runs) {
        gameOverAtScore(g, r);
        step(g, 0.7f);
        tap(g, kW * 0.5f, kH * 0.5f);
        g.update(1.0f / 60.0f);                // -> TITLE
        ASSERT_TRUE(g.inTitleForTest());
        tap(g, kW * 0.5f, kH * 0.5f);
        g.update(1.0f / 60.0f);                // -> fresh PLAYING run
    }
    EXPECT_EQ(g.highScoreForTest(0), 500);
    EXPECT_EQ(g.highScoreForTest(1), 400);
    EXPECT_EQ(g.highScoreForTest(2), 300);
    EXPECT_EQ(g.highScoreForTest(3), 250);
    EXPECT_EQ(g.highScoreForTest(4), 200);
}

TEST(HighScores, SavedOnGameOverAndReloaded) {
    const char* dir = scratchDir();
    char path[512];
    snprintf(path, sizeof(path), "%s/highscores.bin", dir);
    remove(path);
    {
        Game g;
        g.setViewport(kW, kH);
        g.setDataPath(dir);
        g.triggerNewGameForTest();
        gameOverAtScore(g, 1234);
    }
    {
        Game g;
        g.setViewport(kW, kH);
        g.setDataPath(dir);
        EXPECT_EQ(g.highScoreForTest(0), 1234);
    }
    remove(path);
}

TEST(HighScores, ZeroScoreIsNotRecorded) {
    Game g;
    startPlaying(g);
    gameOverAtScore(g, 0);
    EXPECT_EQ(g.highScoreForTest(0), 0);
    EXPECT_FALSE(g.newHighScoreForTest());
}

TEST(HighScores, TieWithExistingEntryDoesNotDuplicate) {
    Game g;
    startPlaying(g);
    gameOverAtScore(g, 30);
    ASSERT_EQ(g.highScoreForTest(0), 30L);
    EXPECT_TRUE(g.newHighScoreForTest());
    step(g, 0.7f);
    tap(g, kW * 0.5f, kH * 0.5f);
    g.update(0.016f);                           // -> TITLE
    tap(g, kW * 0.5f, kH * 0.5f);
    g.update(0.016f);                           // -> new round
    gameOverAtScore(g, 30);
    EXPECT_EQ(g.highScoreForTest(0), 30L);
    EXPECT_EQ(g.highScoreForTest(1), 0L);
    EXPECT_FALSE(g.newHighScoreForTest());
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

TEST(Settings, AutoPlayRowToggles) {
    Game g;
    g.setViewport(kW, kH);
    openSettings(g);
    ASSERT_FALSE(g.autoPlayActiveForTest());
    tap(g, pxOf(0.0f), pyOf(0.08f));            // auto play row
    g.update(0.016f);
    EXPECT_TRUE(g.autoPlayActiveForTest());
    tap(g, pxOf(0.0f), pyOf(0.08f));
    g.update(0.016f);
    EXPECT_FALSE(g.autoPlayActiveForTest());
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
    openSettings(g);
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
        tap(g, pxOf(0.0f), pyOf(0.08f));        // auto play on
        g.update(0.016f);
        ASSERT_FALSE(g.soundEnabledForTest());
        ASSERT_TRUE(g.autoPlayActiveForTest());
    }
    {
        Game g;
        g.setViewport(kW, kH);
        g.setDataPath(dir);
        EXPECT_FALSE(g.soundEnabledForTest());
        EXPECT_TRUE(g.autoPlayActiveForTest());
    }
    remove(path);
}

// ── Auto Play ────────────────────────────────────────────────────────────────

TEST(AutoPlay, LaunchesTheHeldBallAfterTheGrace) {
    Game g;
    startPlaying(g);
    g.setAutoPlayForTest(true);
    step(g, 0.3f);
    EXPECT_TRUE(g.ballHeldForTest());            // the grace is respected
    step(g, 0.4f);
    EXPECT_FALSE(g.ballHeldForTest());           // and it does not wait for the self-launch
}

TEST(AutoPlay, PredictsWhereTheBallLands) {
    Game g;
    startPlaying(g);
    g.setAutoPlayForTest(true);
    int keep = slot(g, 2, 0);                     // one brick stays, far from the ball's path
    for (int i = 0; i < g.brickTotalForTest(); i++)
        if (i != keep) g.killBrickForTest(i);
    g.setBallForTest(-0.30f, 0.15f, 0.6f, 1.0f);  // falling to the right, nothing in the way
    g.update(1.0f / 60.0f);
    ASSERT_TRUE(g.aiHasLandingForTest());
    float predictedX = g.aiLandingXForTest();
    float predictedT = g.aiLandingTForTest();
    // Let it fall with the paddle parked out of the way and see where it was
    // when it crossed the paddle line.
    g.setAutoPlayForTest(false);
    float actualX = 0.0f, actualT = 0.0f;
    for (int i = 0; i < 300; i++) {
        g.setPaddleXForTest(-0.50f);              // far left, away from the ball
        g.update(1.0f / 60.0f);
        actualT += 1.0f / 60.0f;
        if (g.ballCount() == 0 || g.ballYForTest(0) + kBallR >= kPaddleY - 0.025f) {
            actualX = g.ballCount() ? g.ballXForTest(0) : actualX;
            break;
        }
        actualX = g.ballXForTest(0);
    }
    EXPECT_NEAR(predictedX, actualX, 0.04f);
    EXPECT_NEAR(predictedT, actualT, 0.10f);
}

TEST(AutoPlay, GoesUnderTheBall) {
    Game g;
    startPlaying(g);
    g.setAutoPlayForTest(true);
    g.setBallForTest(0.35f, 0.20f, 0.0f, 1.0f);   // dropping straight down at x 0.35
    g.update(1.0f / 60.0f);
    ASSERT_TRUE(g.aiHasTargetForTest());
    EXPECT_NEAR(g.aiTargetXForTest(), 0.35f, kPaddleHW);
    step(g, 1.0f);
    EXPECT_EQ(g.lives(), 5);
    EXPECT_GE(g.paddleHitsForTest(), 1);
}

TEST(AutoPlay, CatchesABallComingOffTheWall) {
    Game g;
    startPlaying(g);
    g.setAutoPlayForTest(true);
    g.setBallForTest(0.50f, 0.30f, 1.0f, 0.5f);   // heading for the right wall, then back
    g.update(1.0f / 60.0f);
    ASSERT_TRUE(g.aiHasLandingForTest());
    EXPECT_LT(g.aiLandingXForTest(), 0.50f);      // it comes down left of where it is now
    step(g, 1.5f);
    EXPECT_EQ(g.lives(), 5);
    EXPECT_GE(g.paddleHitsForTest(), 1);
}

TEST(AutoPlay, AimsTheReboundAtTheLastBrick) {
    Game g;
    startPlaying(g);
    g.setAutoPlayForTest(true);
    // One brick left, far to the left; the ball drops straight down on the
    // right. The plan takes it off-centre so a rebound reaches the brick —
    // within a couple of trips, not by luck.
    int keep = slot(g, 2, 0);
    for (int i = 0; i < g.brickTotalForTest(); i++)
        if (i != keep) g.killBrickForTest(i);
    g.setBallForTest(0.40f, 0.15f, 0.0f, 1.0f);
    g.update(1.0f / 60.0f);
    ASSERT_TRUE(g.aiHasLandingForTest());
    EXPECT_NE(g.aiTargetXForTest(), 0.40f);       // not a centre hit
    for (int i = 0; i < 60 * 12 && g.isPlayingForTest(); i++) g.update(1.0f / 60.0f);
    EXPECT_TRUE(g.isLevelClearForTest()) << "the last brick is still up after 12 s";
    EXPECT_EQ(g.lives(), 5);
}

TEST(AutoPlay, NeverPlansADeadCentreHit) {
    // |u| < 0.12 all leave at the 8-degree minimum, left or right by the sign
    // of a hair's width of paddle placement: the plan never lives there.
    Game g;
    startPlaying(g);
    g.setAutoPlayForTest(true);
    for (int i = 0; i < 20; i++) {
        g.setBallForTest(-0.45f + 0.045f * (float)i, 0.15f, 0.0f, 1.0f);
        g.update(1.0f / 60.0f);
        ASSERT_TRUE(g.aiHasLandingForTest());
        EXPECT_GE(fabsf(g.aiPlanUForTest()), 0.15f) << "ball " << i;
    }
}

TEST(AutoPlay, CollectsACapsuleItCanReachInTime) {
    Game g;
    startPlaying(g);
    g.setAutoPlayForTest(true);
    g.setBallForTest(0.0f, 0.10f, 0.0f, -1.0f);   // the ball is on its way up: time to spare
    g.spawnTestPowerUp(0.35f, 0.30f, (int)Game::PU_LIFE);
    g.update(1.0f / 60.0f);
    ASSERT_TRUE(g.aiHasTargetForTest());
    EXPECT_NEAR(g.aiTargetXForTest(), 0.35f, 0.02f);
    step(g, 1.0f);
    EXPECT_EQ(g.lives(), 6);
}

TEST(AutoPlay, LeavesACapsuleTheBallDoesNotAllow) {
    Game g;
    startPlaying(g);
    g.setAutoPlayForTest(true);
    // The ball lands on the left in a third of a second; a capsule on the
    // far right would cost the ball. Catch the ball.
    g.setBallForTest(-0.45f, 0.35f, 0.0f, 1.0f);
    g.spawnTestPowerUp(0.48f, 0.45f, (int)Game::PU_WIDE);
    g.update(1.0f / 60.0f);
    ASSERT_TRUE(g.aiHasTargetForTest());
    EXPECT_LT(g.aiTargetXForTest(), 0.0f);
    step(g, 1.0f);
    EXPECT_EQ(g.lives(), 5);
}

TEST(AutoPlay, SkipsAWideCapsuleWhileWideWithTimeLeft) {
    Game g;
    startPlaying(g);
    g.setAutoPlayForTest(true);
    g.activatePowerUpForTest((int)Game::PU_WIDE);
    g.setBallForTest(0.0f, 0.10f, 0.0f, -1.0f);
    g.spawnTestPowerUp(0.35f, 0.30f, (int)Game::PU_WIDE);
    g.update(1.0f / 60.0f);
    ASSERT_TRUE(g.aiHasTargetForTest());
    EXPECT_GT(fabsf(g.aiTargetXForTest() - 0.35f), 0.05f);
    // An extra life in the same spot is always worth it.
    Game h;
    startPlaying(h);
    h.setAutoPlayForTest(true);
    h.activatePowerUpForTest((int)Game::PU_WIDE);
    h.setBallForTest(0.0f, 0.10f, 0.0f, -1.0f);
    h.spawnTestPowerUp(0.35f, 0.30f, (int)Game::PU_LIFE);
    h.update(1.0f / 60.0f);
    ASSERT_TRUE(h.aiHasTargetForTest());
    EXPECT_NEAR(h.aiTargetXForTest(), 0.35f, 0.02f);
}

TEST(AutoPlay, SavesTheBallThatComesDownFirst) {
    Game g;
    startPlaying(g);
    g.setAutoPlayForTest(true);
    g.setBallForTest(-0.40f, 0.40f, 0.0f, 1.0f);   // this one lands in a quarter second
    g.addBallForTest(0.40f, -0.40f, 0.0f, 1.0f);   // this one takes over a second
    g.update(1.0f / 60.0f);
    ASSERT_TRUE(g.aiHasTargetForTest());
    EXPECT_LT(g.aiTargetXForTest(), 0.0f);
}

TEST(AutoPlay, ReplansWhenTheBallBounces) {
    Game g;
    startPlaying(g);
    g.setAutoPlayForTest(true);
    g.setBallForTest(0.45f, 0.30f, 1.0f, 0.3f);    // about to hit the right wall
    g.update(1.0f / 60.0f);
    float before = g.aiLandingXForTest();
    step(g, 0.5f);                                 // bounced: new direction, new plan
    ASSERT_TRUE(g.aiHasLandingForTest());
    EXPECT_NEAR(g.aiLandingXForTest(), before, 0.05f);   // same landing, re-derived
    EXPECT_LT(g.ballDxForTest(0), 0.0f);
}

TEST(AutoPlay, DrivesThePaddleLikeTheCrownDoes) {
    // The autopilot writes the same paddle target the crown writes: no
    // second control path, and a crown turn mid-round is simply overridden
    // on the next frame.
    Game g;
    startPlaying(g);
    g.setAutoPlayForTest(true);
    g.setBallForTest(0.40f, 0.20f, 0.0f, 1.0f);
    g.update(1.0f / 60.0f);
    EXPECT_NEAR(g.paddleTargetForTest(), g.aiTargetXForTest(), 1e-5f);
}

TEST(AutoPlay, ClearsLevelOneWithoutLosingABall) {
    for (uint32_t seed = 1; seed <= 4; seed++) {
        Game g;
        g.setViewport(kW, kH);
        g.seedRng(seed * 0x9E3779B9u);
        g.triggerNewGameForTest();
        g.setAutoPlayForTest(true);
        for (int i = 0; i < 60 * 120 && g.isPlayingForTest(); i++)
            g.update(1.0f / 60.0f);
        EXPECT_TRUE(g.isLevelClearForTest()) << "seed " << seed << ": bricks left "
                                              << g.breakableLeftForTest();
        EXPECT_EQ(g.lives(), 5) << "seed " << seed;
    }
}

TEST(AutoPlay, HandlesMultiBall) {
    Game g;
    g.setViewport(kW, kH);
    g.startLevelForTest(3);
    g.setAutoPlayForTest(true);
    step(g, 1.0f);                                 // launched
    g.activatePowerUpForTest((int)Game::PU_MULTI);
    ASSERT_EQ(g.ballCount(), 3);
    int lives = g.lives();
    for (int i = 0; i < 60 * 20 && g.isPlayingForTest(); i++) g.update(1.0f / 60.0f);
    EXPECT_GE(g.lives(), lives);                   // three balls up, none of them cost a life
}

// ── Title demo ───────────────────────────────────────────────────────────────

TEST(Title, DemoBallStaysInsideTheLane) {
    Game g;
    g.setViewport(kW, kH);
    for (int i = 0; i < 60 * 20; i++) {
        g.update(1.0f / 60.0f);
        ASSERT_LE(fabsf(g.demoBallXForTest()) + kBallR, kFieldHW + 1e-4f);
        ASSERT_LT(g.demoBallYForTest(), kPaddleY);
    }
}

TEST(Title, CrownMovesTheDemoPaddle) {
    Game g;
    g.setViewport(kW, kH);
    for (int i = 0; i < 4; i++) g.onRotary(1.0f);
    step(g, 0.5f);
    EXPECT_NEAR(g.demoPaddleXForTest(), 0.40f, 0.02f);
    step(g, 3.0f);                               // the crown has rested: back to the ball
    EXPECT_NEAR(g.demoPaddleXForTest(), g.demoBallXForTest(), 0.08f);
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
    tap(g, pxOf(0.0f), pyOf(0.56f));
    g.update(0.016f);

    g.triggerNewGameForTest();
    g.activatePowerUpForTest((int)Game::PU_WIDE);
    g.activatePowerUpForTest((int)Game::PU_SLOW);
    g.spawnTestPowerUp(0.2f, 0.0f, (int)Game::PU_LIFE);
    g.spawnTestPowerUp(-0.2f, 0.0f, (int)Game::PU_MULTI);
    step(g, 0.6f);
    g.launchForTest();
    step(g, 0.5f);
    cmds.clear(); g.render(cmds);                      // PLAYING (HUD, wall, ball, capsules)
    EXPECT_GT(cmds.size(), 0u) << "PLAYING drew nothing";
    expectInsideTheDisc(cmds, "PLAYING");

    g.setLivesForTest(9);
    g.startLevelForTest(10);                           // the densest wall, nine dots
    cmds.clear(); g.render(cmds);
    expectInsideTheDisc(cmds, "LEVEL 10");

    g.setScoreForTest(123456);                         // six digits on the end screen
    g.clearWallForTest();
    g.update(0.016f);                                  // WIN
    ASSERT_TRUE(g.isWinForTest());
    step(g, 1.0f);
    cmds.clear(); g.render(cmds);
    EXPECT_GT(cmds.size(), 0u) << "WIN drew nothing";
    expectInsideTheDisc(cmds, "WIN");

    Game h;
    startPlaying(h);
    h.clearWallForTest();
    h.update(0.016f);                                  // LEVEL_CLEAR
    ASSERT_TRUE(h.isLevelClearForTest());
    cmds.clear(); h.render(cmds);
    EXPECT_GT(cmds.size(), 0u) << "LEVEL_CLEAR drew nothing";
    expectInsideTheDisc(cmds, "LEVEL_CLEAR");
    step(h, 2.0f);
    h.setLivesForTest(1);
    loseABall(h);                                      // GAME_OVER
    ASSERT_TRUE(h.isGameOverForTest());
    step(h, 1.0f);
    cmds.clear(); h.render(cmds);
    EXPECT_GT(cmds.size(), 0u) << "GAME_OVER drew nothing";
    expectInsideTheDisc(cmds, "GAME_OVER");
}
