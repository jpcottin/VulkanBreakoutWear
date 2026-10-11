#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>
#include "common.h"

class AudioEngine;

// A brick-breaking game for a round watch: a paddle, a ball and a wall of
// coloured bricks, driven by the crown. Ten short levels from easy to hard,
// bonuses dropping from broken bricks.
// World coordinates: x in [-asp, asp], y in [-1, 1], y pointing DOWN (the
// bricks sit at the top, the paddle near the bottom). asp = width/height,
// 1 on a watch. The screen is a disc of radius 1: everything the game draws
// stays inside it, the playfield is a lane of half-width 0.66 whose corners
// clear the rim.
//
// Controls: turning the crown slides the paddle (one detent, one step);
// dragging a finger anywhere does the same; any tap launches a ball that is
// resting on the paddle, and a ball nobody launches goes by itself.
class Game {
public:
    Game();
    void setViewport(int w, int h);

    // Input (pixel coordinates from the touch screen).
    void onPointerDown(int id, float x, float y);
    void onPointerMove(int id, float x, float y);
    void onPointerUp(int id);
    void onPointersCancel();
    // The crown: a signed number of detents (positive clockwise).
    void onRotary(float delta);

    void update(float dt);
    void render(std::vector<DrawCmd>& out);
    void clearColor(float out[3]) const;

    // True on static screens (title / game over / win) where the main loop may
    // throttle the frame rate to save the battery.
    bool isIdleScreen() const {
        return state_ == TITLE || state_ == GAME_OVER || state_ == WIN;
    }

public:
    static const int kMaxScores = 5;
    static const int kMaxLives  = 9;

    // Brick kinds. Hits to break: 1, 2, 3; steel never breaks and does not
    // count toward clearing the level.
    enum BrickType { BRICK_NORMAL = 0, BRICK_TOUGH = 1, BRICK_ARMOR = 2, BRICK_STEEL = 3 };
    // Bonuses dropped by broken bricks.
    enum PowerUpType { PU_WIDE = 0, PU_SLOW = 1, PU_MULTI = 2, PU_LIFE = 3 };

private:
    enum State { TITLE, PLAYING, LEVEL_CLEAR, GAME_OVER, WIN, SETTINGS };

    // One brick of the wall. Its world position derives from its (row, col)
    // slot; the wall is centred horizontally.
    struct Brick {
        int  row = 0, col = 0;
        BrickType type = BRICK_NORMAL;
        int  hp = 1;
        bool alive = false;
    };
    // (dx, dy) is a unit direction; the speed is a property of the level
    // (see ballSpeed()), so a slow-ball bonus scales every ball at once.
    struct Ball { float x = 0.0f, y = 0.0f, dx = 0.0f, dy = -1.0f; bool alive = false; };
    struct PowerUp {
        float x = 0.0f, y = 0.0f, vy = 0.0f;
        PowerUpType type = PU_WIDE;
        bool alive = false;
    };
    struct Particle {
        float x = 0.0f, y = 0.0f, vx = 0.0f, vy = 0.0f;
        float rot = 0.0f, spin = 0.0f;
        float t = 0.0f, maxLife = 0.0f, size = 0.0f;
        float r = 0.0f, g = 0.0f, b = 0.0f;
        bool alive = false;
    };
    struct Explosion {
        float x = 0.0f, y = 0.0f, radius = 0.0f;
        float t = 0.0f, maxLife = 0.0f;
        float cr = 0.0f, cg = 0.0f, cb = 0.0f;
        bool alive = false;
    };
    struct Pointer { bool active = false; float x = 0.0f, y = 0.0f; };
    struct HighScore { long score = 0; int level = 0; };

    // --- helpers ---
    float frand();
    float frange(float a, float b);
    void  startGame();
    void  startLevel(int level);
    void  buildWall(int level);
    void  buildDemoWall();
    int   wallCols() const;
    float brickX(const Brick& b) const;
    float brickY(const Brick& b) const;
    int   breakableLeft() const;
    // PLAYING-state update steps, called in this order from update().
    void  updatePaddle(float dt);
    void  updateBalls(float dt);
    void  updatePowerUps(float dt);
    void  checkLevelClear();
    void  serveBall();
    void  launchBall();
    void  loseBall();
    void  bounceOffPaddle(Ball& b);
    // Damage a brick the ball just hit. Returns true if it broke.
    bool  hitBrick(Brick& br);
    void  spawnPowerUp(float x, float y);
    void  applyPowerUp(PowerUpType t);
    void  spawnDebris(float x, float y, float r, float cr, float cg, float cb);
    float ballSpeed() const;
    float paddleHalfW() const;
    void  setPaddleTarget(float x);
    bool  isGearTap(float px, float py) const;
    void  loadHighScores();
    void  saveHighScores();
    // Returns the rank the entry was inserted at, or -1 when it was not
    // ranked or an identical row already exists.
    int   mergeHighScore(long score, int level);
    void  writeHighScores();
    void  mergeDiskHighScores();
    void  checkHighScore();
    // Clear every in-play entity; shared by startLevel() and the return to
    // the title so nothing renders frozen there.
    void  resetBattlefield();
    void  loadSettings();
    void  saveSettings();
    // Push soundEnabled_ + the current state into the audio engine's music flag.
    void  syncMusic();
    static int numDigits(long v);


    // emit one shape: world centre (wx,wy), world half-extents (sx,sy), rotation, rgba.
    // style selects the fragment-shader fill (STYLE_FLAT default, STYLE_GLOW).
    void emit(std::vector<DrawCmd>& out, int shape, float wx, float wy,
              float sx, float sy, float rot, float r, float g, float b, float a,
              float style = (float)STYLE_FLAT) const;
    void drawDigit(std::vector<DrawCmd>& out, int d, float cx, float cy,
                   float h, float r, float g, float b, float a) const;
    void drawNumber(std::vector<DrawCmd>& out, long value, float leftX, float cy,
                    float h, float r, float g, float b, float a) const;
    void drawLetter(std::vector<DrawCmd>& out, char ch, float cx, float cy,
                    float h, float r, float g, float b, float a) const;
    void drawText(std::vector<DrawCmd>& out, const char* text, float cx, float cy,
                  float h, float r, float g, float b, float a) const;
    void drawBrick(std::vector<DrawCmd>& out, const Brick& b, float alpha) const;
    void drawPaddle(std::vector<DrawCmd>& out, float cx, float cy, float halfW,
                    float alpha) const;
    void drawBall(std::vector<DrawCmd>& out, float cx, float cy, float alpha) const;
    void drawPowerUp(std::vector<DrawCmd>& out, const PowerUp& pu) const;
    void drawPowerUpHUD(std::vector<DrawCmd>& out) const;
    void drawGearIcon(std::vector<DrawCmd>& out, float cx, float cy, float size,
                      float r, float g, float b, float a) const;
    void drawSettingsScreen(std::vector<DrawCmd>& out) const;
    // GAME_OVER / WIN share one layout: tinted overlay, big score, rank digit.
    void drawEndScreen(std::vector<DrawCmd>& out, const char* title, float pulse,
                       const float tint[4], float scoreG, float scoreB,
                       float plainG, float plainB) const;

    // --- audio ---
    AudioEngine* audio_ = nullptr;

    // --- haptic ---
    std::function<void()> haptic_;

    // --- persistence ---
    HighScore highScores_[kMaxScores] = {};
    bool newHighScore_     = false;
    int  newHighScoreRank_ = -1;
    char dataPath_[512]     = {};
    char settingsPath_[512] = {};

public:
    void setAudioEngine(AudioEngine* a) { audio_ = a; syncMusic(); }
    void setDataPath(const char* path);
    // Randomize the xorshift state (production only — tests rely on the fixed
    // default seed for deterministic choreography). Zero would lock xorshift
    // at zero forever, so it is mapped away.
    void seedRng(uint32_t seed) { rng_ = seed ? seed : 0x1234567u; }
    // Process-death save/restore (see SavedGame in main.cpp).
    bool isMidGame() const { return state_ == PLAYING || state_ == LEVEL_CLEAR; }
    void restoreSession(int level, long score, int lives);
    void setHapticCallback(std::function<void()> fn) { haptic_ = std::move(fn); }

    // --- test / debug accessors ---
public:
    float paddleX() const { return paddleX_; }
    int   ballCount() const {
        int n = 0;
        for (auto& b : balls_) if (b.alive) n++;
        return n;
    }
    long  score() const { return score_; }
    int   lives() const { return lives_; }
    int   level() const { return level_; }

#ifndef NDEBUG
    // Test-only helpers — excluded from release builds (NDEBUG defined).
    void  triggerNewGameForTest()        { startGame(); }
    void  startLevelForTest(int level)   { startLevel(level); }
    bool  inTitleForTest()        const { return state_ == TITLE; }
    bool  inSettingsForTest()     const { return state_ == SETTINGS; }
    bool  isPlayingForTest()      const { return state_ == PLAYING; }
    bool  isLevelClearForTest()   const { return state_ == LEVEL_CLEAR; }
    bool  isGameOverForTest()     const { return state_ == GAME_OVER; }
    bool  isWinForTest()          const { return state_ == WIN; }
    bool  soundEnabledForTest()   const { return soundEnabled_; }
    int   wallColsForTest()       const { return wallCols(); }
    int   wallRowsForTest()       const { return rows_; }
    int   brickTotalForTest()     const { return (int)bricks_.size(); }
    bool  brickAliveForTest(int i) const { return bricks_[i].alive; }
    int   brickHpForTest(int i)   const { return bricks_[i].hp; }
    int   brickTypeForTest(int i) const { return (int)bricks_[i].type; }
    float brickXForTest(int i)    const { return brickX(bricks_[i]); }
    float brickYForTest(int i)    const { return brickY(bricks_[i]); }
    int   breakableLeftForTest()  const { return breakableLeft(); }
    void  killBrickForTest(int i)       { bricks_[i].alive = false; }
    // Break every breakable brick: the level is one update from clear. Steel stays.
    void  clearWallForTest() {
        for (auto& b : bricks_) if (b.type != BRICK_STEEL) b.alive = false;
    }
    bool  ballHeldForTest()       const { return ballHeld_; }
    float ballXForTest(int i)     const { return balls_[i].x; }
    float ballYForTest(int i)     const { return balls_[i].y; }
    float ballDxForTest(int i)    const { return balls_[i].dx; }
    float ballDyForTest(int i)    const { return balls_[i].dy; }
    // Replace every ball with one free ball at (x, y) heading along (dx, dy).
    void  setBallForTest(float x, float y, float dx, float dy) {
        float n = sqrtf(dx * dx + dy * dy);
        if (n < 1e-6f) { dx = 0.0f; dy = -1.0f; n = 1.0f; }
        balls_.clear();
        balls_.push_back({x, y, dx / n, dy / n, true});
        ballHeld_ = false;
    }
    // Add a free ball to the ones in play.
    void  addBallForTest(float x, float y, float dx, float dy) {
        float n = sqrtf(dx * dx + dy * dy);
        if (n < 1e-6f) { dx = 0.0f; dy = -1.0f; n = 1.0f; }
        balls_.push_back({x, y, dx / n, dy / n, true});
        ballHeld_ = false;
    }
    void  launchForTest()               { if (ballHeld_) launchBall(); }
    void  setPaddleXForTest(float x)    { paddleX_ = x; paddleTarget_ = x; }
    float paddleTargetForTest()   const { return paddleTarget_; }
    float paddleHalfWForTest()    const { return paddleHalfW(); }
    float ballSpeedForTest()      const { return ballSpeed(); }
    void  spawnTestPowerUp(float x, float y, int type) {
        powerUps_.push_back({x, y, 0.35f, (PowerUpType)type, true});
    }
    int   powerUpCountForTest()   const {
        int n = 0;
        for (auto& p : powerUps_) if (p.alive) n++;
        return n;
    }
    void  activatePowerUpForTest(int type) { applyPowerUp((PowerUpType)type); }
    bool  wideActiveForTest()     const { return wideTimer_ > 0.0f; }
    bool  slowActiveForTest()     const { return slowTimer_ > 0.0f; }
    long  highScoreForTest(int i) const { return highScores_[i].score; }
    bool  newHighScoreForTest()   const { return newHighScore_; }
    void  setScoreForTest(long s)       { score_ = s; }
    void  setLivesForTest(int n)        { lives_ = n; }
    int   paddleHitsForTest()     const { return paddleHits_; }
    int   bricksBrokenForTest()   const { return bricksBroken_; }
    int   ballsLostForTest()      const { return ballsLost_; }
    float demoBallXForTest()      const { return demoBallX_; }
    float demoBallYForTest()      const { return demoBallY_; }
    float demoPaddleXForTest()    const { return demoPaddleX_; }
#endif

private:
    // --- viewport ---
    int vw_ = 1, vh_ = 1;
    float asp_ = 1.0f;

    // --- input ---
    static const int kMaxPointers = 16;
    Pointer pointers_[kMaxPointers] = {};
    bool  tapPending_ = false;
    float tapX_       = 0.0f;
    float tapY_       = 0.0f;
    // Where the paddle is going: the crown's position, a finger's, or the
    // autopilot's. The paddle glides there at its own speed.
    float paddleTarget_ = 0.0f;
    float crownRecent_  = 10.0f;   // seconds since the last detent (capped)

    // --- state ---
    State state_     = TITLE;
    State prevState_ = TITLE;
    float animTime_  = 0.0f;
    float stateTimer_ = 0.0f;

    // --- the wall ---
    std::vector<Brick> bricks_;
    int rows_      = 0;
    int wallCols_  = 1;      // columns the current wall was built with
    int wallTotal_ = 0;      // breakable bricks at level start (speed ramp)

    // --- paddle (fixed altitude, slides horizontally) ---
    float paddleX_ = 0.0f;

    // --- balls ---
    std::vector<Ball> balls_;
    bool  ballHeld_   = false;  // a ball rests on the paddle, waiting to go
    float serveTimer_ = 0.0f;   // counts down to the self-launch; taps work once past the grace
    float speedBoost_ = 0.0f;   // extra speed earned by breaking bricks (0..1)

    // --- bonuses ---
    std::vector<PowerUp> powerUps_;
    float wideTimer_ = 0.0f;
    float slowTimer_ = 0.0f;
    static constexpr float kWideDuration = 10.0f;
    static constexpr float kSlowDuration = 8.0f;

    // --- effects ---
    std::vector<Particle>  particles_;
    std::vector<Explosion> explosions_;
    float shakeAmt_ = 0.0f;
    float shakeX_   = 0.0f;
    float shakeY_   = 0.0f;
    long  bonusFlash_ = 0;        // last bonus value, shown briefly where earned
    float bonusFlashTimer_ = 0.0f;
    float bonusFlashX_ = 0.0f, bonusFlashY_ = 0.0f;

    // --- title demo: a ball bouncing under the title, a paddle chasing it ---
    float demoBallX_ = 0.0f, demoBallY_ = 0.0f;
    float demoBallVx_ = 0.55f, demoBallVy_ = 0.80f;
    float demoPaddleX_ = 0.0f;

    // --- progression / scoring ---
    int level_ = 1;
    int lives_ = 5;
    long score_ = 0;
    int  paddleHits_   = 0;   // bench statistics
    int  bricksBroken_ = 0;
    int  ballsLost_    = 0;

    // --- settings (persisted) ---
    bool soundEnabled_   = true;
    bool autoPlayActive_ = false;


    uint32_t rng_ = 0x1234567u;
};
