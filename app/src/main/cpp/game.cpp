#include "game.h"
#include "audio.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unistd.h>

// ---- the round screen ----
// The screen is a disc of radius 1 in world units. The playfield is a lane
// whose corners clear the rim: half-width 0.66, top wall at -0.60, paddle
// line at 0.60 (the corner (0.66, 0.60) sits at radius 0.89).
static const float kFieldHW   = 0.66f;
static const float kTopWallY  = -0.60f;

// ---- level tuning (1..10, easy -> hard) ----
static int clampLevel(int L) { return L < 1 ? 1 : (L > 10 ? 10 : L); }
// Rows of bricks per level: 3 at level 1, up to 6 at level 10 — a watch
// round is short.
static int rowsForLevel(int L) {
    static const int kRows[10] = {3, 3, 4, 4, 4, 5, 5, 5, 6, 6};
    return kRows[clampLevel(L) - 1];
}
// Base ball speed per level, in world units per second (the disc is 2 across).
static float levelBallSpeed(int L) { return 0.85f + 0.06f * (float)(clampLevel(L) - 1); }
// The ball gets up to this much faster as the wall thins out (classic).
static const float kSpeedRamp  = 0.20f;
static const float kSlowFactor = 0.60f;   // slow-ball bonus

// ---- wall geometry (world units) ----
// Bricks are big: five columns across the lane, so they read on a watch.
static const float kBrickHW   = 0.110f;   // half-width
static const float kBrickHH   = 0.045f;   // half-height
static const float kBrickGapX = 0.025f;
static const float kBrickGapY = 0.025f;
static const float kWallTopY  = -0.50f;   // centre of row 0
static const float kSideMargin = 0.020f;  // smallest gap between the wall and the lane's side

// Paddle line, the ball and the pit below the paddle.
static const float kPaddleY      = 0.60f;
static const float kPaddleHW     = 0.160f;   // half-width (normal)
static const float kWidePaddleHW = 0.250f;   // half-width with the wide bonus
static const float kPaddleHH     = 0.025f;
static const float kPaddleSpeed  = 2.6f;
static const float kBallR        = 0.030f;
static const float kPitY         = 0.70f;    // ball centre past this: lost
// A served ball: taps work after the grace, and it goes by itself after the
// delay — a watch should not wait on a tap.
static const float kServeGrace   = 0.5f;
static const float kServeDelay   = 1.5f;
static const float kMaxBounceAngle = 65.0f * 3.14159265f / 180.0f;   // off the paddle's tip
static const float kMinBounceAngle =  8.0f * 3.14159265f / 180.0f;   // never straight up
static const float kMinDy    = 0.25f;    // after a wall/brick bounce: no near-horizontal shuttling
static const float kBallStep = 0.015f;   // collision substep length (half the ball radius)
static const int   kMaxBalls = 6;
// One crown detent moves the paddle this far.
static const float kCrownStep = 0.10f;

// Bonuses.
static const float kPowerUpSpeed = 0.35f;
static const float kPowerUpR     = 0.055f;
static const float kDropChance   = 0.12f;   // per broken brick

// Points per broken brick by kind (x level): normal, tough, armour.
static const int kBrickScore[3] = {10, 20, 30};
static const int kPowerUpScore    = 25;
static const int kLevelClearScore = 100;

// Brick colours by row: a bright rainbow so every row reads at a glance.
static const float kRowCol[9][3] = {
    {1.00f, 0.32f, 0.30f},   // red
    {1.00f, 0.60f, 0.20f},   // orange
    {1.00f, 0.90f, 0.25f},   // yellow
    {0.40f, 0.95f, 0.40f},   // green
    {0.30f, 0.85f, 1.00f},   // cyan
    {0.40f, 0.52f, 1.00f},   // blue
    {0.80f, 0.45f, 1.00f},   // purple
    {1.00f, 0.45f, 0.80f},   // pink
    {0.92f, 0.92f, 0.95f},   // white
};
static const float kSteelCol[3] = {0.55f, 0.58f, 0.66f};
// Bonus capsule colours: wide, slow, multi, life.
static const float kPowerUpCol[4][3] = {
    {0.30f, 0.85f, 1.00f},
    {0.80f, 0.45f, 1.00f},
    {1.00f, 0.65f, 0.20f},
    {0.40f, 0.95f, 0.45f},
};

// High-score podium colours (gold/silver/bronze), shared by the title table
// and the rank digit on the game-over/win screens.
static const float kPodiumCol[3][3] = {
    {1.00f, 0.85f, 0.20f},   // gold
    {0.78f, 0.82f, 0.92f},   // silver
    {0.72f, 0.47f, 0.22f},   // bronze
};

// 7-segment masks for digits 0..9 (bit a=1,b=2,c=4,d=8,e=16,f=32,g=64).
static const int kDigitSeg[10] = {63, 6, 91, 79, 102, 109, 125, 7, 127, 111};

// Gear icon world position — the bottom of the disc, on the title and end
// screens. Single source of truth for rendering and hit-testing.
static constexpr float kGearWX = 0.0f;
static constexpr float kGearWY = 0.82f;

// Settings row y-positions — shared between drawSettingsScreen() and the tap handler.
static constexpr float kSettingSoundY    = -0.22f;
static constexpr float kSettingAutoPlayY =  0.08f;
static constexpr float kSettingBackY     =  0.56f;

static const uint32_t kSettingsMagic = 0x53455454u;  // "SETT"
static const uint32_t kHsMagic       = 0x4F4B5257u;  // "WRKO"

// On-disk layout of highscores.bin (little-endian, fixed size).
struct HighScoreFile {
    uint32_t magic, count;
    struct { int64_t score; int32_t level; } e[Game::kMaxScores];
};

// Zero the screen-shake offset for the lifetime of the guard so HUD and
// overlay elements are drawn shake-free, then restore it.
struct ShakeOff {
    float &x, &y, sx, sy;
    ShakeOff(float& x_, float& y_) : x(x_), y(y_), sx(x_), sy(y_) { x = 0.0f; y = 0.0f; }
    ~ShakeOff() { x = sx; y = sy; }
};

// The wall of a level, slot by slot, for a five-column lane. `present` is
// false for a hole.
static Game::BrickType brickTypeAt(int L, int row, int col, int rows, bool& present) {
    present = true;
    const bool oddCol = (col % 2) == 1;
    switch (clampLevel(L)) {
        case 1:
        case 2:  return Game::BRICK_NORMAL;
        case 3:  return row == 0 ? Game::BRICK_TOUGH : Game::BRICK_NORMAL;
        case 4:  if (row == rows - 1 && oddCol) present = false;
                 return row == 0 ? Game::BRICK_TOUGH : Game::BRICK_NORMAL;
        case 5:  if (row == 2 && col == 2) return Game::BRICK_STEEL;
                 return row == 0 ? Game::BRICK_TOUGH : Game::BRICK_NORMAL;
        case 6:  if (row == 3 && (col == 0 || col == 4)) return Game::BRICK_STEEL;
                 return row <= 1 ? Game::BRICK_TOUGH : Game::BRICK_NORMAL;
        case 7:  if (row == 3 && col == 2) return Game::BRICK_STEEL;
                 if (row == rows - 1 && oddCol) present = false;
                 return row == 0 ? Game::BRICK_ARMOR
                      : (row == 1 ? Game::BRICK_TOUGH : Game::BRICK_NORMAL);
        case 8:  if (row == 4 && oddCol) return Game::BRICK_STEEL;
                 return row == 0 ? Game::BRICK_ARMOR
                      : (row <= 2 ? Game::BRICK_TOUGH : Game::BRICK_NORMAL);
        case 9:  if (row == 4 && !oddCol) return Game::BRICK_STEEL;
                 return row <= 1 ? Game::BRICK_ARMOR
                      : (row == 2 ? Game::BRICK_TOUGH : Game::BRICK_NORMAL);
        default: if (row == 4 && oddCol) return Game::BRICK_STEEL;
                 return row <= 1 ? Game::BRICK_ARMOR
                      : (row <= 3 ? Game::BRICK_TOUGH : Game::BRICK_NORMAL);
    }
}

static int hitsFor(Game::BrickType t) {
    switch (t) {
        case Game::BRICK_NORMAL: return 1;
        case Game::BRICK_TOUGH:  return 2;
        case Game::BRICK_ARMOR:  return 3;
        default:                 return 1;   // steel: never decremented
    }
}


Game::Game() { buildDemoWall(); }   // title screen shows a demo wall

float Game::frand() {
    uint32_t x = rng_;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_ = x;
    return float(x & 0xFFFFFF) / float(0x1000000);
}
float Game::frange(float a, float b) { return a + (b - a) * frand(); }

void Game::setViewport(int w, int h) {
    if (w <= 0 || h <= 0) return;
    vw_ = w; vh_ = h;
    asp_ = (float)w / (float)h;
}

// --- input ---

void Game::onPointerDown(int id, float x, float y) {
    if (id >= 0 && id < kMaxPointers) {
        pointers_[id].active = true;
        pointers_[id].x = x;
        pointers_[id].y = y;
    }
    if (!tapPending_) { tapX_ = x; tapY_ = y; }  // first finger wins
    tapPending_ = true;
}
void Game::onPointerMove(int id, float x, float y) {
    if (id >= 0 && id < kMaxPointers && pointers_[id].active) {
        pointers_[id].x = x;
        pointers_[id].y = y;
    }
}
void Game::onPointerUp(int id) {
    if (id >= 0 && id < kMaxPointers) pointers_[id].active = false;
}
void Game::onPointersCancel() {
    for (auto& p : pointers_) p.active = false;
}

// A detent of the crown: step the paddle target and clamp it to the lane.
void Game::onRotary(float delta) {
    setPaddleTarget(paddleTarget_ + delta * kCrownStep);
    crownRecent_ = 0.0f;
}

void Game::setPaddleTarget(float x) {
    float lim = kFieldHW - paddleHalfW();
    if (x > lim) x = lim;
    if (x < -lim) x = -lim;
    paddleTarget_ = x;
}

// ── Persistence ───────────────────────────────────────────────────────────────

// Write `buf` to `path` atomically: a crash between truncate and write can't
// leave a half-written file behind, and the loader would otherwise discard
// the short file and the next save would wipe the table.
static void writeFileAtomic(const char* path, const void* buf, size_t len) {
    char tmp[600];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return;
    FILE* f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(buf, len, 1, f) == 1;
    // fsync before the rename: on f2fs a rename can otherwise reach disk
    // before the data blocks, and a power loss leaves a short file that the
    // loader discards (and the next save then overwrites with a fresh table).
    ok = (fflush(f) == 0) && ok;
    ok = (fsync(fileno(f)) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) remove(tmp);
}

void Game::setDataPath(const char* path) {
    if (!path || path[0] == '\0') return;
    snprintf(dataPath_,     sizeof(dataPath_),     "%s/highscores.bin", path);
    snprintf(settingsPath_, sizeof(settingsPath_), "%s/settings.bin",   path);
    loadHighScores();
    loadSettings();
}

void Game::loadHighScores() {
    if (dataPath_[0] == '\0') return;
    FILE* f = fopen(dataPath_, "rb");
    if (!f) return;
    HighScoreFile buf{};
    if (fread(&buf, sizeof(buf), 1, f) == 1 && buf.magic == kHsMagic) {
        int n = (int)buf.count < kMaxScores ? (int)buf.count : kMaxScores;
        for (int i = 0; i < n; i++) {
            highScores_[i].score = (long)buf.e[i].score;
            highScores_[i].level = buf.e[i].level;
        }
    }
    fclose(f);
}

// Insert a score at its rank unless the identical entry is already present.
int Game::mergeHighScore(long score, int level) {
    if (score <= 0) return -1;
    for (const auto& hs : highScores_)
        if (hs.score == score && hs.level == level) return -1;
    int pos = kMaxScores;
    for (int i = 0; i < kMaxScores; i++)
        if (score > highScores_[i].score) { pos = i; break; }
    if (pos == kMaxScores) return -1;
    for (int i = kMaxScores - 1; i > pos; i--) highScores_[i] = highScores_[i - 1];
    highScores_[pos] = {score, level};
    return pos;
}

// Fold the on-disk table into ours before writing, so a stale in-memory copy
// never clobbers a saved score.
void Game::mergeDiskHighScores() {
    if (dataPath_[0] == '\0') return;
    FILE* rf = fopen(dataPath_, "rb");
    if (!rf) return;
    HighScoreFile in{};
    if (fread(&in, sizeof(in), 1, rf) == 1 && in.magic == kHsMagic) {
        int n = (int)in.count < kMaxScores ? (int)in.count : kMaxScores;
        for (int i = 0; i < n; i++)
            mergeHighScore((long)in.e[i].score, in.e[i].level);
    }
    fclose(rf);
}

void Game::saveHighScores() {
    mergeDiskHighScores();
    writeHighScores();
}

void Game::writeHighScores() {
    if (dataPath_[0] == '\0') return;
    HighScoreFile buf{};
    buf.magic = kHsMagic;
    buf.count = kMaxScores;
    for (int i = 0; i < kMaxScores; i++) {
        buf.e[i].score = (int64_t)highScores_[i].score;
        buf.e[i].level = highScores_[i].level;
    }
    writeFileAtomic(dataPath_, &buf, sizeof(buf));
}

// The single rule for the music flag: it plays from the start of a round
// through the end screen (and under a Settings overlay opened from those),
// when sound is on.
void Game::syncMusic() {
    if (!audio_) return;
    State s = (state_ == SETTINGS) ? prevState_ : state_;
    audio_->setMusicEnabled(soundEnabled_ && s != TITLE);
}

// Resume a run after process death: same level, score, and lives, but a
// fresh wall — entity positions aren't persisted. Bounds-checked because the
// saved blob comes back from the OS, not from us.
void Game::restoreSession(int level, long score, int lives) {
    if (level < 1 || level > 10 || lives < 1 || lives > kMaxLives || score < 0) return;
    startGame();
    if (level > 1) startLevel(level);
    score_ = score;
    lives_ = lives;
}

void Game::loadSettings() {
    if (settingsPath_[0] == '\0') return;
    FILE* f = fopen(settingsPath_, "rb");
    if (!f) return;
    struct { uint32_t magic; int32_t soundOn; int32_t autoPlay; } buf = {};
    size_t n = fread(&buf, 1, sizeof(buf), f);
    fclose(f);
    if (n >= 8 && buf.magic == kSettingsMagic) {
        soundEnabled_ = (buf.soundOn != 0);
        if (n >= 12) autoPlayActive_ = (buf.autoPlay != 0);
    }
}

void Game::saveSettings() {
    if (settingsPath_[0] == '\0') return;
    struct { uint32_t magic; int32_t soundOn; int32_t autoPlay; } buf = {
        kSettingsMagic, soundEnabled_ ? 1 : 0, autoPlayActive_ ? 1 : 0
    };
    writeFileAtomic(settingsPath_, &buf, sizeof(buf));
}

bool Game::isGearTap(float px, float py) const {
    float wy = 2.0f * py / (float)vh_ - 1.0f;
    float wx = (2.0f * px / (float)vw_ - 1.0f) * asp_;
    float dx = wx - kGearWX, dy = wy - kGearWY;
    return dx*dx + dy*dy < 0.0144f;  // 0.12 world units radius: a fingertip on a watch
}

void Game::checkHighScore() {
    if (score_ <= 0) return;
    // Same dedupe rule as the disk merge; an exact tie with an existing row
    // is not a new record.
    mergeDiskHighScores();
    int pos = mergeHighScore(score_, level_);
    if (pos < 0) return;
    newHighScore_     = true;
    newHighScoreRank_ = pos;
    writeHighScores();
}

// ── The wall ──────────────────────────────────────────────────────────────────

// Columns that fit across the lane.
int Game::wallCols() const {
    float usable = 2.0f * (kFieldHW - kSideMargin) + kBrickGapX;
    int cols = (int)(usable / (2.0f * kBrickHW + kBrickGapX));
    return cols < 1 ? 1 : cols;
}

float Game::brickX(const Brick& b) const {
    return ((float)b.col - (float)(wallCols_ - 1) * 0.5f) * (2.0f * kBrickHW + kBrickGapX);
}
float Game::brickY(const Brick& b) const {
    return kWallTopY + (float)b.row * (2.0f * kBrickHH + kBrickGapY);
}

int Game::breakableLeft() const {
    int n = 0;
    for (auto& b : bricks_) if (b.alive && b.type != BRICK_STEEL) n++;
    return n;
}

void Game::buildWall(int level) {
    wallCols_ = wallCols();
    rows_ = rowsForLevel(level);
    bricks_.clear();
    bricks_.reserve((size_t)rows_ * wallCols_);
    for (int r = 0; r < rows_; r++)
        for (int c = 0; c < wallCols_; c++) {
            bool present;
            BrickType t = brickTypeAt(level, r, c, rows_, present);
            bricks_.push_back({r, c, t, hitsFor(t), present});
        }
    wallTotal_ = breakableLeft();
}

// The title screen's wall: three full rows, one colour per row.
void Game::buildDemoWall() {
    wallCols_ = wallCols();
    rows_ = 3;
    bricks_.clear();
    bricks_.reserve((size_t)rows_ * wallCols_);
    for (int r = 0; r < rows_; r++)
        for (int c = 0; c < wallCols_; c++)
            bricks_.push_back({r, c, BRICK_NORMAL, 1, true});
    wallTotal_ = (int)bricks_.size();
    demoBallX_ = 0.0f; demoBallY_ = 0.20f;
    demoBallVx_ = 0.55f; demoBallVy_ = 0.80f;
    demoPaddleX_ = 0.0f;
}

// ── Rounds ────────────────────────────────────────────────────────────────────

void Game::startGame() {
    score_ = 0;
    lives_ = 5;
    paddleHits_ = 0; bricksBroken_ = 0; ballsLost_ = 0;
    newHighScore_     = false;
    newHighScoreRank_ = -1;
    bonusFlashTimer_  = 0.0f;
    level_ = 1;
    startLevel(1);
}

void Game::resetBattlefield() {
    balls_.clear();
    powerUps_.clear();
    particles_.clear();
    explosions_.clear();
    ballHeld_ = false;
    wideTimer_ = 0.0f;
    slowTimer_ = 0.0f;
}

void Game::startLevel(int level) {
    level_ = clampLevel(level);
    buildWall(level_);
    resetBattlefield();
    paddleX_ = 0.0f;
    paddleTarget_ = 0.0f;
    speedBoost_ = 0.0f;
    shakeAmt_ = 0.0f; shakeX_ = 0.0f; shakeY_ = 0.0f;
    serveBall();
    state_ = PLAYING;
    syncMusic();
}

// A new ball rests on the paddle: a tap launches it once the grace is over,
// and it launches itself when the delay runs out.
void Game::serveBall() {
    balls_.clear();
    Ball b;
    b.x = paddleX_;
    b.y = kPaddleY - kPaddleHH - kBallR;
    b.dx = 0.0f; b.dy = -1.0f;
    b.alive = true;
    balls_.push_back(b);
    ballHeld_   = true;
    serveTimer_ = kServeDelay;
    aiPlans_.clear();
}

// Up and to one side, 14-31 degrees off vertical.
void Game::launchBall() {
    ballHeld_ = false;
    float a = (frand() < 0.5f ? -1.0f : 1.0f) * frange(0.25f, 0.55f);
    for (auto& b : balls_)
        if (b.alive) { b.dx = sinf(a); b.dy = -cosf(a); }
}

float Game::ballSpeed() const {
    float s = levelBallSpeed(level_) * (1.0f + kSpeedRamp * speedBoost_);
    if (slowTimer_ > 0.0f) s *= kSlowFactor;
    return s;
}

float Game::paddleHalfW() const {
    return wideTimer_ > 0.0f ? kWidePaddleHW : kPaddleHW;
}

// After a bounce off a wall or a brick: keep the ball from shuttling almost
// horizontally, which is a slow way to play nothing.
static void ensureMinDy(float& dx, float& dy) {
    if (fabsf(dy) >= kMinDy) return;
    float sdy = dy < 0.0f ? -1.0f : 1.0f;
    float sdx = dx < 0.0f ? -1.0f : 1.0f;
    dy = sdy * kMinDy;
    dx = sdx * sqrtf(1.0f - kMinDy * kMinDy);
}

// The bounce angle follows where the ball met the paddle: the centre sends
// it (almost) straight up, the tips 65 degrees out. That is the player's
// aim.
void Game::bounceOffPaddle(Ball& b) {
    float u = (b.x - paddleX_) / paddleHalfW();
    if (u >  1.0f) u =  1.0f;
    if (u < -1.0f) u = -1.0f;
    float a = u * kMaxBounceAngle;
    if (fabsf(a) < kMinBounceAngle)
        a = (u < 0.0f || (u == 0.0f && b.dx < 0.0f)) ? -kMinBounceAngle : kMinBounceAngle;
    b.dx =  sinf(a);
    b.dy = -cosf(a);
    paddleHits_++;
    if (audio_ && soundEnabled_) audio_->triggerPaddle();
}

bool Game::hitBrick(Brick& br) {
    if (br.type == BRICK_STEEL) {
        if (audio_ && soundEnabled_) audio_->triggerBrickHit();
        return false;
    }
    br.hp--;
    if (br.hp > 0) {
        if (audio_ && soundEnabled_) audio_->triggerBrickHit();
        return false;
    }
    br.alive = false;
    bricksBroken_++;
    float x = brickX(br), y = brickY(br);
    const float* c = kRowCol[br.row % 9];
    spawnDebris(x, y, kBrickHW, c[0], c[1], c[2]);
    score_ += (long)kBrickScore[br.type] * level_;
    // The ball speeds up as the wall thins.
    speedBoost_ = wallTotal_ > 0 ? 1.0f - (float)breakableLeft() / (float)wallTotal_ : 0.0f;
    if (audio_ && soundEnabled_) {
        // One semitone per row, the top row highest: the wall is a keyboard.
        float hz = 523.25f * powf(2.0f, (float)(rows_ - 1 - br.row) / 12.0f);
        audio_->triggerBrickBreak(hz);
    }
    if (frand() < kDropChance) spawnPowerUp(x, y);
    return true;
}

void Game::spawnPowerUp(float x, float y) {
    PowerUp p;
    p.x  = x;
    p.y  = y;
    p.vy = kPowerUpSpeed;
    // Weighted: wide 35%, slow 30%, multi 25%, extra life 10%.
    float roll = frand();
    p.type = roll < 0.35f ? PU_WIDE : (roll < 0.65f ? PU_SLOW : (roll < 0.90f ? PU_MULTI : PU_LIFE));
    p.alive = true;
    powerUps_.push_back(p);
}

// Grant a bonus. Shared by the pickup path and the test hook so tests
// exercise the production timers, not a copy of them.
void Game::applyPowerUp(PowerUpType t) {
    switch (t) {
        case PU_WIDE: wideTimer_ = kWideDuration; setPaddleTarget(paddleTarget_); break;
        case PU_SLOW: slowTimer_ = kSlowDuration; break;
        case PU_MULTI: {
            // Every ball in play splits into three (fanned 23 degrees apart),
            // up to the cap. A ball still resting on the paddle launches first.
            if (ballHeld_) launchBall();
            int n0 = (int)balls_.size();
            for (int i = 0; i < n0 && (int)balls_.size() < kMaxBalls; i++) {
                if (!balls_[i].alive) continue;
                for (int s = -1; s <= 1 && (int)balls_.size() < kMaxBalls; s += 2) {
                    const float th = 0.40f * (float)s;
                    Ball b = balls_[i];
                    float dx = balls_[i].dx, dy = balls_[i].dy;
                    b.dx = dx * cosf(th) - dy * sinf(th);
                    b.dy = dx * sinf(th) + dy * cosf(th);
                    ensureMinDy(b.dx, b.dy);
                    balls_.push_back(b);
                }
            }
            break;
        }
        case PU_LIFE:
            if (lives_ < kMaxLives) lives_++;
            break;
    }
}

void Game::spawnDebris(float ax, float ay, float ar, float cr, float cg, float cb) {
    // Expanding flash ring
    Explosion e;
    e.x = ax; e.y = ay; e.radius = ar * 0.5f;
    e.t = 0.0f; e.maxLife = 0.20f;
    e.cr = cr; e.cg = cg; e.cb = cb;
    e.alive = true;
    explosions_.push_back(e);

    // 8 fragments flying outward
    for (int i = 0; i < 8; i++) {
        float angle = frange(0.0f, 6.2832f);
        float speed = frange(0.4f, 1.3f);
        Particle p;
        p.x = ax + cosf(angle) * ar * 0.4f;
        p.y = ay + sinf(angle) * ar * 0.4f;
        p.vx = cosf(angle) * speed;
        p.vy = sinf(angle) * speed;
        p.rot  = frange(0.0f, 6.2832f);
        p.spin = frange(-5.0f, 5.0f);
        p.t       = 0.0f;
        p.maxLife = frange(0.25f, 0.50f);
        p.size    = ar * frange(0.12f, 0.28f);
        p.r = cr + frange(-0.08f, 0.08f);
        p.g = cg + frange(-0.08f, 0.08f);
        p.b = cb + frange(-0.08f, 0.08f);
        p.alive = true;
        particles_.push_back(p);
    }
}

// The last ball went into the pit: a life, the bonuses and the shake. The
// next ball is served at once; the game ends on the last life.
void Game::loseBall() {
    ballsLost_++;
    lives_--;
    wideTimer_ = 0.0f;
    slowTimer_ = 0.0f;
    shakeAmt_ = 1.0f;
    if (audio_ && soundEnabled_) audio_->triggerBallLost();
    if (haptic_) haptic_();
    Explosion flash;
    flash.x = paddleX_; flash.y = kPaddleY; flash.radius = kPaddleHW * 0.6f;
    flash.t = 0.0f; flash.maxLife = 0.25f;
    flash.cr = 1.0f; flash.cg = 0.35f; flash.cb = 0.30f;
    flash.alive = true;
    explosions_.push_back(flash);
    if (lives_ <= 0) {
        balls_.clear();
        state_ = GAME_OVER; stateTimer_ = 0.0f;
        checkHighScore();
        return;
    }
    setPaddleTarget(paddleTarget_);
    serveBall();
}

// ── Update ────────────────────────────────────────────────────────────────────

void Game::update(float dt) {
    if (dt > 0.05f) dt = 0.05f;   // clamp huge hitches
    animTime_ += dt;
    if (crownRecent_ < 10.0f) crownRecent_ += dt;

    // Particles and explosions animate in all states.
    // Drag is exponential decay, normalized to a 60 Hz reference so debris
    // behaves the same at any refresh rate.
    float drag = powf(0.88f, dt * 60.0f);
    for (auto& p : particles_) {
        if (!p.alive) continue;
        p.x  += p.vx * dt; p.y  += p.vy * dt;
        p.vx *= drag;      p.vy *= drag;
        p.rot += p.spin * dt;
        p.t   += dt;
        if (p.t >= p.maxLife) p.alive = false;
    }
    particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
                                    [](const Particle& p) { return !p.alive; }),
                     particles_.end());

    for (auto& e : explosions_) {
        if (!e.alive) continue;
        e.t += dt;
        if (e.t >= e.maxLife) e.alive = false;
    }
    explosions_.erase(std::remove_if(explosions_.begin(), explosions_.end(),
                                     [](const Explosion& e) { return !e.alive; }),
                      explosions_.end());

    // Screen shake decay, 60 Hz-normalized like the particle drag above.
    if (shakeAmt_ > 0.0f) {
        shakeX_ = frange(-1.0f, 1.0f) * shakeAmt_ * 0.030f;
        shakeY_ = frange(-1.0f, 1.0f) * shakeAmt_ * 0.030f;
        shakeAmt_ *= powf(0.78f, dt * 60.0f);
        if (shakeAmt_ < 0.01f) { shakeAmt_ = 0.0f; shakeX_ = 0.0f; shakeY_ = 0.0f; }
    }

    if (bonusFlashTimer_ > 0.0f) bonusFlashTimer_ -= dt;

    bool tapped = tapPending_;
    tapPending_ = false;

    // Gear tap opens Settings from the title and end screens.
    if (tapped && (state_ == TITLE || state_ == GAME_OVER || state_ == WIN) &&
        isGearTap(tapX_, tapY_)) {
        prevState_ = state_;
        state_ = SETTINGS;
        tapped = false;
    }

    switch (state_) {
        case TITLE: {
            if (tapped) { startGame(); break; }
            // Demo under the title: the ball bounces between the lane's
            // walls, the bottom of the wall and the paddle line. The paddle
            // follows the crown when it has just been turned (so the first
            // thing a new player sees is the crown working), and otherwise
            // slides to meet the ball.
            demoBallX_ += demoBallVx_ * dt;
            demoBallY_ += demoBallVy_ * dt;
            float lim = kFieldHW - kBallR;
            if (demoBallX_ >  lim) { demoBallX_ =  lim; demoBallVx_ = -fabsf(demoBallVx_); }
            if (demoBallX_ < -lim) { demoBallX_ = -lim; demoBallVx_ =  fabsf(demoBallVx_); }
            float ceiling = kWallTopY + 3.0f * (2.0f * kBrickHH + kBrickGapY) + 0.14f;
            if (demoBallY_ < ceiling) { demoBallY_ = ceiling; demoBallVy_ = fabsf(demoBallVy_); }
            float floorY = kPaddleY - kPaddleHH - kBallR;
            if (demoBallY_ > floorY) { demoBallY_ = floorY; demoBallVy_ = -fabsf(demoBallVy_); }
            float target = crownRecent_ < 2.0f ? paddleTarget_ : demoBallX_;
            float dx = target - demoPaddleX_;
            float stepX = 2.0f * dt;
            if (fabsf(dx) <= stepX) demoPaddleX_ = target;
            else demoPaddleX_ += dx > 0.0f ? stepX : -stepX;
            break;
        }
        case PLAYING: {
            // Reset AI intents; updateAutoPlay sets them when autopilot is on.
            aiMove_ = false; aiLaunch_ = false;
            if (autoPlayActive_) updateAutoPlay(dt);
            if (aiMove_) setPaddleTarget(aiTargetX_);
            // A finger on the screen drags the paddle target; the crown moves
            // it in steps (onRotary). The paddle glides to it either way.
            for (auto& p : pointers_)
                if (p.active) { setPaddleTarget((2.0f * p.x / (float)vw_ - 1.0f) * asp_); break; }
            // One frame of gameplay, in order. State may flip to GAME_OVER
            // (last ball lost) or LEVEL_CLEAR / WIN mid-sequence; once it
            // does, the remaining steps are skipped — checkHighScore() has
            // already saved, so nothing may score or collect after the end.
            updatePaddle(dt);
            if (ballHeld_) {
                bool launchNow = serveTimer_ <= 0.0f;
                bool pastGrace = serveTimer_ <= kServeDelay - kServeGrace;
                if (pastGrace && tapped) launchNow = true;
                if (pastGrace && autoPlayActive_ && aiLaunch_) launchNow = true;
                if (launchNow) launchBall();
            }
            updateBalls(dt);
            if (state_ == PLAYING) updatePowerUps(dt);
            if (state_ == PLAYING) checkLevelClear();
            break;
        }
        case LEVEL_CLEAR: {
            stateTimer_ += dt;
            if (stateTimer_ >= 1.8f) startLevel(level_ + 1);
            break;
        }
        case GAME_OVER:
        case WIN: {
            stateTimer_ += dt;
            if (tapped && stateTimer_ > 0.6f) {
                state_ = TITLE;
                level_ = 1;
                // Clear every leftover, or it renders frozen on the title
                // screen forever (nothing updates it there).
                resetBattlefield();
                buildDemoWall();
                shakeAmt_ = 0.0f; shakeX_ = 0.0f; shakeY_ = 0.0f;
                syncMusic();
            }
            break;
        }
        case SETTINGS: {
            if (tapped) {
                float wy = 2.0f * tapY_ / (float)vh_ - 1.0f;
                float wx = (2.0f * tapX_ / (float)vw_ - 1.0f) * asp_;
                const float kHitH = 0.13f;
                if (fabsf(wy - kSettingSoundY) < kHitH) {
                    soundEnabled_ = !soundEnabled_;
                    saveSettings();
                    syncMusic();
                } else if (fabsf(wy - kSettingAutoPlayY) < kHitH) {
                    autoPlayActive_ = !autoPlayActive_;
                    saveSettings();
                } else if (fabsf(wy - kSettingBackY) < kHitH && fabsf(wx) < 0.25f) {
                    onPointersCancel();
                    state_ = prevState_;
                }
            }
            break;
        }
    }
}

// ---- PLAYING-state update steps ----

void Game::updatePaddle(float dt) {
    float dx = paddleTarget_ - paddleX_;
    const float deadband = 0.004f;
    if (fabsf(dx) > deadband) {
        float step = kPaddleSpeed * dt;
        if (step > fabsf(dx)) step = fabsf(dx);   // land exactly on the target
        paddleX_ += dx > 0.0f ? step : -step;
    }
    float lim = kFieldHW - paddleHalfW();
    if (paddleX_ > lim) paddleX_ = lim;
    if (paddleX_ < -lim) paddleX_ = -lim;

    if (ballHeld_) {
        serveTimer_ -= dt;
        for (auto& b : balls_) {
            if (!b.alive) continue;
            b.x = paddleX_;
            b.y = kPaddleY - kPaddleHH - kBallR;
        }
    }
}

void Game::updateBalls(float dt) {
    if (ballHeld_) return;
    const float speed = ballSpeed();
    // Substep the move so the ball can't tunnel through a brick or the
    // paddle: a step is at most half the ball's radius.
    int steps = (int)(speed * dt / kBallStep) + 1;
    float sdt = dt / (float)steps;
    const float phw = paddleHalfW();
    const float lim = kFieldHW - kBallR;
    const float top = kTopWallY + kBallR;
    const float paddleTop = kPaddleY - kPaddleHH;

    for (auto& b : balls_) {
        if (!b.alive) continue;
        for (int s = 0; s < steps && b.alive; s++) {
            b.x += b.dx * speed * sdt;
            b.y += b.dy * speed * sdt;

            // Side walls and the ceiling.
            if (b.x > lim && b.dx > 0.0f) {
                b.x = lim; b.dx = -b.dx;
                if (audio_ && soundEnabled_) audio_->triggerWall();
            } else if (b.x < -lim && b.dx < 0.0f) {
                b.x = -lim; b.dx = -b.dx;
                if (audio_ && soundEnabled_) audio_->triggerWall();
            }
            if (b.y < top && b.dy < 0.0f) {
                b.y = top; b.dy = -b.dy;
                ensureMinDy(b.dx, b.dy);
                if (audio_ && soundEnabled_) audio_->triggerWall();
            }

            // The paddle: only its top face, only a falling ball.
            if (b.dy > 0.0f && b.y + kBallR >= paddleTop &&
                b.y - kBallR <= kPaddleY + kPaddleHH &&
                fabsf(b.x - paddleX_) <= phw + kBallR) {
                b.y = paddleTop - kBallR;
                bounceOffPaddle(b);
            }

            // Bricks: reflect along the axis of least penetration and push the
            // ball out. One brick per substep; the next substep sees the rest.
            for (auto& br : bricks_) {
                if (!br.alive) continue;
                float dx = b.x - brickX(br), dy = b.y - brickY(br);
                float px = kBrickHW + kBallR - fabsf(dx);
                float py = kBrickHH + kBallR - fabsf(dy);
                if (px <= 0.0f || py <= 0.0f) continue;
                if (px < py) {
                    b.dx = dx > 0.0f ? fabsf(b.dx) : -fabsf(b.dx);
                    b.x += dx > 0.0f ? px : -px;
                } else {
                    b.dy = dy > 0.0f ? fabsf(b.dy) : -fabsf(b.dy);
                    b.y += dy > 0.0f ? py : -py;
                }
                ensureMinDy(b.dx, b.dy);
                hitBrick(br);
                break;
            }

            // The pit.
            if (b.y - kBallR > kPitY) b.alive = false;
        }
    }

    bool any = false;
    for (auto& b : balls_) if (b.alive) any = true;
    balls_.erase(std::remove_if(balls_.begin(), balls_.end(),
                                [](const Ball& b) { return !b.alive; }),
                 balls_.end());
    if (!any) loseBall();
}

void Game::updatePowerUps(float dt) {
    if (wideTimer_ > 0.0f) {
        wideTimer_ -= dt;
        if (wideTimer_ <= 0.0f) {
            wideTimer_ = 0.0f;
            setPaddleTarget(paddleTarget_);
        }
    }
    if (slowTimer_ > 0.0f) {
        slowTimer_ -= dt;
        if (slowTimer_ <= 0.0f) slowTimer_ = 0.0f;
    }

    const float phw = paddleHalfW();
    for (auto& pu : powerUps_) {
        if (!pu.alive) continue;
        pu.y += pu.vy * dt;
        if (pu.y > 1.1f) { pu.alive = false; continue; }
        if (fabsf(pu.x - paddleX_) < phw + kPowerUpR &&
            fabsf(pu.y - kPaddleY) < kPaddleHH + kPowerUpR) {
            pu.alive = false;
            long bonus = (long)kPowerUpScore * level_;
            score_ += bonus;
            bonusFlash_ = bonus;
            bonusFlashTimer_ = 0.9f;
            bonusFlashX_ = pu.x; bonusFlashY_ = pu.y - 0.09f;
            applyPowerUp(pu.type);
            if (audio_ && soundEnabled_) {
                if (pu.type == PU_LIFE) audio_->triggerExtraLife();
                else                    audio_->triggerPowerUp();
            }
        }
    }
    powerUps_.erase(std::remove_if(powerUps_.begin(), powerUps_.end(),
                                   [](const PowerUp& pu) { return !pu.alive; }),
                    powerUps_.end());
}

void Game::checkLevelClear() {
    if (state_ != PLAYING || breakableLeft() > 0) return;
    score_ += (long)kLevelClearScore * level_;
    if (audio_ && soundEnabled_) audio_->triggerLevelClear();
    balls_.clear();
    powerUps_.clear();
    ballHeld_ = false;
    if (level_ >= 10) {
        state_ = WIN; stateTimer_ = 0.0f;
        checkHighScore();
        return;
    }
    state_ = LEVEL_CLEAR;
    stateTimer_ = 0.0f;
}

// ── Auto-play AI ─────────────────────────────────────────────────────────────
//
// The autopilot plays the same way a good player does: it works out where the
// ball will come down, gets the paddle there, and picks *where on the paddle*
// to take it so the rebound does the most damage — the hit offset sets the
// angle, so the choice of offset is the aim. Every candidate rebound is
// simulated against the wall with the same collision rules as the real
// update; the one that hits the most bricks per second of flight and still
// comes down somewhere the paddle can reach wins. Predictions are cached
// per ball and only redone when the ball changes direction (a bounce), so a
// trip costs a handful of simulations, not one per frame.
//
// Bonus capsules are collected when the paddle can get there and still be
// back under the ball in time; a ball resting on the paddle is launched as
// soon as the serve delay is over.

namespace {

constexpr float kAiHorizon    = 8.0f;    // seconds of flight worth simulating
constexpr int   kAiMaxSteps   = 4000;    // hard cap on a simulation
constexpr float kAiCatchSlack = 0.85f;   // fraction of the half-width that still catches
// Candidate hit offsets across the paddle: -1 is the left tip, +1 the right.
// Nothing inside the dead zone (|u| < kAiMinOffset): every hit there leaves
// at the 8-degree minimum, left or right by the sign of a hair's width of
// paddle placement, so a plan there is a coin toss the simulation cannot
// call.
constexpr float kAiOffsets[] = {-0.85f, -0.60f, -0.35f, -0.16f, 0.16f, 0.35f, 0.60f, 0.85f};
constexpr float kAiMinOffset = 0.16f;
constexpr int   kAiOffsetCount = (int)(sizeof(kAiOffsets) / sizeof(kAiOffsets[0]));

// The rebound direction for a hit at offset u — the same rule as
// bounceOffPaddle(), including the never-vertical minimum.
void reboundDir(float u, float& dx, float& dy) {
    if (u >  1.0f) u =  1.0f;
    if (u < -1.0f) u = -1.0f;
    float a = u * kMaxBounceAngle;
    if (fabsf(a) < kMinBounceAngle) a = u < 0.0f ? -kMinBounceAngle : kMinBounceAngle;
    dx =  sinf(a);
    dy = -cosf(a);
}

}  // namespace

// Fly a ball from (x, y) along (dx, dy) until it comes down to the paddle
// line. `hp` is a scratch copy of the wall (hit points per brick, 0 dead,
// -1 steel) that the flight damages as it goes, so a second flight from the
// landing point sees the bricks the first one broke.
Game::Flight Game::simulateFlight(float x, float y, float dx, float dy, float speed,
                                  std::vector<int>& hp, float tMax) const {
    Flight f;
    const float lim = kFieldHW - kBallR;
    const float top = kTopWallY + kBallR;
    const float paddleTop = kPaddleY - kPaddleHH;
    // Below this line there is nothing to hit: skip the brick loop.
    float wallBottom = kTopWallY;
    if (rows_ > 0) wallBottom = kWallTopY + (float)(rows_ - 1) * (2.0f * kBrickHH + kBrickGapY) + kBrickHH;
    const float sdt = kBallStep / speed;
    float t = 0.0f;
    for (int step = 0; step < kAiMaxSteps && t < tMax; step++) {
        x += dx * kBallStep;
        y += dy * kBallStep;
        t += sdt;
        if (x > lim && dx > 0.0f) { x = lim; dx = -dx; }
        else if (x < -lim && dx < 0.0f) { x = -lim; dx = -dx; }
        if (y < top && dy < 0.0f) { y = top; dy = -dy; ensureMinDy(dx, dy); }
        if (y < f.minY) f.minY = y;
        if (y - kBallR <= wallBottom) {
            for (size_t i = 0; i < bricks_.size(); i++) {
                if (hp[i] == 0) continue;
                const Brick& br = bricks_[i];
                float bx = x - brickX(br), by = y - brickY(br);
                float px = kBrickHW + kBallR - fabsf(bx);
                float py = kBrickHH + kBallR - fabsf(by);
                if (px <= 0.0f || py <= 0.0f) continue;
                if (px < py) {
                    dx = bx > 0.0f ? fabsf(dx) : -fabsf(dx);
                    x += bx > 0.0f ? px : -px;
                } else {
                    dy = by > 0.0f ? fabsf(dy) : -fabsf(dy);
                    y += by > 0.0f ? py : -py;
                }
                ensureMinDy(dx, dy);
                if (hp[i] > 0) {
                    hp[i]--;
                    f.hits += hp[i] == 0 ? 2 : 1;
                }
                break;
            }
        }
        if (dy > 0.0f && y + kBallR >= paddleTop) {
            f.lands = true;
            f.x = x;
            f.t = t;
            return f;
        }
    }
    return f;   // never came down within the horizon
}

// Predict where ball `b` lands and choose the rebound to give it there.
void Game::makePlan(Plan& plan, const Ball& b, int ballIndex) const {
    plan.valid = true;
    plan.dx = b.dx; plan.dy = b.dy;
    plan.age = 0.0f;
    plan.u = 0.0f;
    const float speed = ballSpeed();
    std::vector<int> hp(bricks_.size());
    auto resetWall = [&]() {
        for (size_t i = 0; i < bricks_.size(); i++)
            hp[i] = !bricks_[i].alive ? 0 : (bricks_[i].type == BRICK_STEEL ? -1 : bricks_[i].hp);
    };
    resetWall();
    plan.land = simulateFlight(b.x, b.y, b.dx, b.dy, speed, hp, kAiHorizon);
    if (!plan.land.lands) return;
    // The wall as it will be when the ball arrives (this flight's damage).
    std::vector<int> wallAtLanding = hp;

    const float hw   = paddleHalfW();
    const float lim  = kFieldHW - hw;
    const float speedP = kPaddleSpeed;
    const float ballY = kPaddleY - kPaddleHH - kBallR;
    // The other balls' landings, to keep reachable after this hit.
    float bestScore = -1e9f;
    for (int k = 0; k < kAiOffsetCount; k++) {
        float u = kAiOffsets[k];
        float px = plan.land.x - u * hw;         // where the paddle must be
        if (px > lim) px = lim;
        if (px < -lim) px = -lim;
        float uReal = (plan.land.x - px) / hw;   // the offset the clamp allows
        if (fabsf(uReal) < kAiMinOffset) {       // pushed into the dead zone by the clamp
            uReal = uReal < 0.0f ? -kAiMinOffset : kAiMinOffset;
            px = plan.land.x - uReal * hw;
            if (px > lim || px < -lim) continue;
        }
        // Reachable in time? The paddle slides at kPaddleSpeed.
        float need = fabsf(px - paddleX_) - speedP * plan.land.t;
        if (need > 0.0f) continue;
        float rdx, rdy;
        reboundDir(uReal, rdx, rdy);
        hp = wallAtLanding;
        Flight next = simulateFlight(plan.land.x, ballY, rdx, rdy, speed, hp, kAiHorizon);
        // Damage per second of flight, the wall's height reached as a tie
        // breaker (a trip that climbs high is a trip that will hit something
        // next time), and a small preference for the middle of the paddle.
        float score;
        if (next.lands) {
            score = (float)next.hits / (next.t + 0.4f) + (kPaddleY - next.minY) * 0.15f;
            // The next landing must be reachable from this paddle position;
            // otherwise the hit is a slow way to lose the ball.
            float gap = fabsf(next.x - px) - hw * kAiCatchSlack - speedP * next.t;
            if (gap > 0.0f) score -= 10.0f + gap;
            // Other balls in play land too: a hit that strands them is bad.
            for (size_t j = 0; j < aiPlans_.size(); j++) {
                if ((int)j == ballIndex || !aiPlans_[j].valid || !aiPlans_[j].land.lands) continue;
                const Flight& o = aiPlans_[j].land;
                float after = o.t - aiPlans_[j].age - plan.land.t;
                if (after <= 0.0f) continue;
                float g = fabsf(o.x - px) - hw * kAiCatchSlack - speedP * after;
                if (g > 0.0f) score -= 3.0f;
            }
        } else {
            // Still up there after the horizon: busy breaking bricks, or
            // stuck. Reward the damage, mildly.
            score = (float)next.hits / kAiHorizon + (kPaddleY - next.minY) * 0.15f;
        }
        if (next.hits == 0) {
            // A trip that hits nothing. When the last bricks are off in a
            // corner, no single rebound reaches them: look one trip further
            // — from where this rebound comes down, which second rebound
            // hits something, and how soon. Taking a dead trip without
            // looking ahead sent the ball up the same empty lane for
            // minutes.
            float best2 = 0.0f;
            if (next.lands) {
                float px2 = next.x;   // the paddle will be under it; offsets from there
                for (int k2 = 0; k2 < kAiOffsetCount; k2++) {
                    float u2 = kAiOffsets[k2];
                    float p2 = next.x - u2 * hw;
                    if (p2 > lim || p2 < -lim) continue;
                    if (fabsf(p2 - px2) > speedP * next.t) continue;   // not reachable in time
                    float d2x, d2y;
                    reboundDir(u2, d2x, d2y);
                    std::vector<int> hp2 = hp;
                    Flight two = simulateFlight(next.x, ballY, d2x, d2y, speed, hp2, kAiHorizon);
                    if (two.hits == 0) continue;
                    float s2 = (float)two.hits / (next.t + (two.lands ? two.t : kAiHorizon) + 0.4f);
                    if (s2 > best2) best2 = s2;
                }
            }
            score += 0.8f * best2 + 0.10f * fabsf(next.x - plan.land.x) + 0.05f * fabsf(uReal);
        } else {
            score -= 0.02f * fabsf(uReal);
        }
        if (score > bestScore) { bestScore = score; plan.u = uReal; }
    }
    if (bestScore <= -1e8f) {
        // Nothing reachable in time: head for the landing anyway, the
        // nearest tip first.
        plan.u = plan.land.x > paddleX_ ? -kAiCatchSlack : kAiCatchSlack;
    }
}

void Game::updateAutoPlay(float dt) {
    // A ball on the paddle: launch it the moment the serve delay is over.
    if (ballHeld_) {
        aiLaunch_ = true;
        aiPlans_.clear();
    }

    // Keep one plan per ball; redo a plan when its ball changed direction.
    if (aiPlans_.size() != balls_.size()) aiPlans_.assign(balls_.size(), Plan());
    for (size_t i = 0; i < balls_.size(); i++) {
        const Ball& b = balls_[i];
        Plan& p = aiPlans_[i];
        if (!b.alive || ballHeld_) { p.valid = false; continue; }
        if (p.valid && p.dx == b.dx && p.dy == b.dy) { p.age += dt; continue; }
        makePlan(p, b, (int)i);
    }

    // The primary ball: the one that comes down first and can still be
    // caught. A ball that is already out of reach is written off.
    const float hw  = paddleHalfW();
    const float lim = kFieldHW - hw;
    const float speedP = kPaddleSpeed;
    int primary = -1;
    float primaryT = 1e9f;
    for (size_t i = 0; i < aiPlans_.size(); i++) {
        const Plan& p = aiPlans_[i];
        if (!p.valid || !p.land.lands) continue;
        float t = p.land.t - p.age;
        if (t < 0.0f) t = 0.0f;
        float px = p.land.x - p.u * hw;
        if (px > lim) px = lim;
        if (px < -lim) px = -lim;
        bool catchable = fabsf(px - paddleX_) - speedP * t <= hw * (1.0f - kAiCatchSlack) + 0.02f;
        if (!catchable) continue;
        if (t < primaryT) { primaryT = t; primary = (int)i; }
    }
    if (primary < 0) {
        // Nothing catchable: chase the soonest landing regardless.
        for (size_t i = 0; i < aiPlans_.size(); i++) {
            const Plan& p = aiPlans_[i];
            if (!p.valid || !p.land.lands) continue;
            float t = p.land.t - p.age;
            if (t < primaryT) { primaryT = t; primary = (int)i; }
        }
    }

    float desiredX = paddleX_;
    bool  haveTarget = false;
    if (primary >= 0) {
        const Plan& p = aiPlans_[primary];
        aiLanding_ = p.land;
        aiLanding_.t = primaryT;
        aiPlanU_ = p.u;
        desiredX = p.land.x - p.u * hw;
        haveTarget = true;
    } else {
        aiLanding_ = Flight();
        aiPlanU_ = 0.0f;
    }

    // Bonus capsules: worth a detour when the paddle can be under the
    // capsule as it crosses the paddle line and still be back under the
    // ball in time. The most valuable reachable one wins; a bonus already
    // running with plenty of time left is not worth the walk.
    const PowerUp* collect = nullptr;
    int collectValue = -1;
    for (auto& pu : powerUps_) {
        if (!pu.alive) continue;
        float tp = (kPaddleY - pu.y) / pu.vy;        // centre reaches the paddle line
        if (tp < -0.25f) continue;                   // already past
        if (tp < 0.0f) tp = 0.0f;
        int value;
        switch (pu.type) {
            case PU_LIFE:  value = 4; break;
            case PU_MULTI: value = 3; break;
            case PU_SLOW:  value = slowTimer_ > 3.0f ? 0 : 2; break;
            default:       value = wideTimer_ > 3.0f ? 0 : 1; break;
        }
        if (value == 0) continue;
        float px = pu.x;
        if (px > lim) px = lim;
        if (px < -lim) px = -lim;
        // Get there before the capsule leaves the paddle band...
        float leave = tp + (kPaddleHH + kPowerUpR) / pu.vy;
        if (fabsf(px - paddleX_) > speedP * leave) continue;
        // ...and back under the ball in time for its landing.
        if (haveTarget) {
            float back = primaryT - tp;
            if (back < 0.0f) continue;               // the ball comes first
            if (fabsf(desiredX - px) - hw * (1.0f - kAiCatchSlack) > speedP * back) continue;
        }
        if (value > collectValue) { collectValue = value; collect = &pu; }
    }

    if (collect) {
        aiTargetX_ = collect->x;
        aiMove_ = true;
    } else if (haveTarget) {
        aiTargetX_ = desiredX;
        aiMove_ = true;
    } else if (ballHeld_) {
        aiTargetX_ = paddleX_;
        aiMove_ = true;
    }
}


// ── Drawing ───────────────────────────────────────────────────────────────────

void Game::emit(std::vector<DrawCmd>& out, int shape, float wx, float wy,
                float sx, float sy, float rot, float r, float g, float b, float a,
                float style) const {
    float c = cosf(rot), s = sinf(rot);
    DrawCmd d;
    d.mtx[0] = sx * c / asp_;
    d.mtx[1] = -sy * s / asp_;
    d.mtx[2] = sx * s;
    d.mtx[3] = sy * c;
    d.tx = wx / asp_ + shakeX_;
    d.ty = wy + shakeY_;
    d.color[0] = r; d.color[1] = g; d.color[2] = b; d.color[3] = a;
    d.style = style;
    d.seed = 0.0f;
    d.shape = shape;
    out.push_back(d);
}

void Game::drawDigit(std::vector<DrawCmd>& out, int dgt, float cx, float cy,
                     float h, float r, float g, float b, float a) const {
    if (dgt < 0 || dgt > 9) return;
    int mask = kDigitSeg[dgt];
    float w = h * 0.60f;
    float t = h * 0.16f;
    float ht = t * 0.5f, hw = w * 0.5f, q = h * 0.25f;
    float hSegX = hw - ht, hSegY = ht;     // horizontal segment half-extents
    float vSegX = ht, vSegY = q - ht;      // vertical segment half-extents
    auto seg = [&](float x, float y, float ex, float ey) {
        emit(out, SHAPE_QUAD, x, y, ex, ey, 0.0f, r, g, b, a);
    };
    if (mask & 1)  seg(cx, cy - h * 0.5f + ht, hSegX, hSegY);   // a top
    if (mask & 2)  seg(cx + hw - ht, cy - q, vSegX, vSegY);     // b top-right
    if (mask & 4)  seg(cx + hw - ht, cy + q, vSegX, vSegY);     // c bottom-right
    if (mask & 8)  seg(cx, cy + h * 0.5f - ht, hSegX, hSegY);   // d bottom
    if (mask & 16) seg(cx - hw + ht, cy + q, vSegX, vSegY);     // e bottom-left
    if (mask & 32) seg(cx - hw + ht, cy - q, vSegX, vSegY);     // f top-left
    if (mask & 64) seg(cx, cy, hSegX, hSegY);                   // g middle
}

int Game::numDigits(long v) {
    if (v <= 0) return 1;
    int n = 0;
    while (v > 0) { n++; v /= 10; }
    return n;
}

void Game::drawNumber(std::vector<DrawCmd>& out, long value, float firstCx, float cy,
                      float h, float r, float g, float b, float a) const {
    if (value < 0) value = 0;
    int n = numDigits(value);
    float w = h * 0.60f;
    float step = w * 1.45f;
    int digits[20];   // enough for any 64-bit value
    long tmp = value;
    int count = 0;
    if (value == 0) { digits[count++] = 0; }
    while (tmp > 0 && count < 20) { digits[count++] = (int)(tmp % 10); tmp /= 10; }
    // digits[] is reversed; draw most-significant first.
    for (int i = 0; i < n; i++) {
        int dgt = digits[n - 1 - i];
        drawDigit(out, dgt, firstCx + (float)i * step, cy, h, r, g, b, a);
    }
}

void Game::drawLetter(std::vector<DrawCmd>& out, char ch, float cx, float cy,
                      float h, float r, float g, float b, float a) const {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch >= '0' && ch <= '9') { drawDigit(out, ch - '0', cx, cy, h, r, g, b, a); return; }
    if (ch < 'A' || ch > 'Z') return;

    // Stroke font: each letter built from line segments.
    // Coords are in letter-local space: x in [-1,1], y in [-1,1] (top=-1, bot=+1).
    float hw = h * 0.38f;  // half-width
    float hh = h * 0.50f;  // half-height
    float th = h * 0.07f;  // stroke thickness

    auto stroke = [&](float x1, float y1, float x2, float y2) {
        float wx1 = cx + x1 * hw,  wy1 = cy + y1 * hh;
        float wx2 = cx + x2 * hw,  wy2 = cy + y2 * hh;
        float dx = wx2 - wx1, dy = wy2 - wy1;
        float len = sqrtf(dx*dx + dy*dy);
        if (len < 0.001f) return;
        float rot = atan2f(wx1 - wx2, wy2 - wy1);
        emit(out, SHAPE_QUAD, (wx1+wx2)*0.5f, (wy1+wy2)*0.5f, th, len*0.5f, rot, r, g, b, a);
    };

    switch (ch) {
        case 'A': stroke(-1,1,0,-1); stroke(1,1,0,-1); stroke(-0.5f,0.1f,0.5f,0.1f); break;
        case 'B': stroke(-1,-1,-1,1); stroke(-1,-1,0.6f,-1); stroke(0.6f,-1,1,-0.5f); stroke(1,-0.5f,0.6f,0); stroke(0.6f,0,-1,0); stroke(-1,0,0.8f,0); stroke(0.8f,0,1,0.5f); stroke(1,0.5f,0.8f,1); stroke(0.8f,1,-1,1); break;
        case 'C': stroke(1,-0.7f,0,-1); stroke(0,-1,-1,-0.3f); stroke(-1,-0.3f,-1,0.3f); stroke(-1,0.3f,0,1); stroke(0,1,1,0.7f); break;
        case 'D': stroke(-1,-1,-1,1); stroke(-1,-1,0.3f,-1); stroke(0.3f,-1,1,-0.4f); stroke(1,-0.4f,1,0.4f); stroke(1,0.4f,0.3f,1); stroke(0.3f,1,-1,1); break;
        case 'E': stroke(-1,-1,-1,1); stroke(-1,-1,1,-1); stroke(-1,0,0.6f,0); stroke(-1,1,1,1); break;
        case 'F': stroke(-1,-1,-1,1); stroke(-1,-1,1,-1); stroke(-1,0,0.6f,0); break;
        case 'G': stroke(1,-0.7f,0,-1); stroke(0,-1,-1,-0.3f); stroke(-1,-0.3f,-1,0.3f); stroke(-1,0.3f,0,1); stroke(0,1,1,0.7f); stroke(1,0.7f,1,0); stroke(1,0,0.2f,0); break;
        case 'H': stroke(-1,-1,-1,1); stroke(1,-1,1,1); stroke(-1,0,1,0); break;
        case 'I': stroke(-0.5f,-1,0.5f,-1); stroke(0,-1,0,1); stroke(-0.5f,1,0.5f,1); break;
        case 'J': stroke(0.5f,-1,0.5f,0.6f); stroke(0.5f,0.6f,0,1); stroke(0,1,-0.5f,0.7f); break;
        case 'K': stroke(-1,-1,-1,1); stroke(-1,0,1,-1); stroke(-1,0,1,1); break;
        case 'L': stroke(-1,-1,-1,1); stroke(-1,1,1,1); break;
        case 'M': stroke(-1,1,-1,-1); stroke(-1,-1,0,0.3f); stroke(0,0.3f,1,-1); stroke(1,-1,1,1); break;
        case 'N': stroke(-1,1,-1,-1); stroke(-1,-1,1,1); stroke(1,1,1,-1); break;
        case 'O': stroke(-1,-1,1,-1); stroke(1,-1,1,1); stroke(1,1,-1,1); stroke(-1,1,-1,-1); break;
        case 'P': stroke(-1,-1,-1,1); stroke(-1,-1,0.7f,-1); stroke(0.7f,-1,1,-0.5f); stroke(1,-0.5f,0.7f,0); stroke(0.7f,0,-1,0); break;
        case 'Q': stroke(-1,-1,1,-1); stroke(1,-1,1,1); stroke(1,1,-1,1); stroke(-1,1,-1,-1); stroke(0.2f,0.4f,1,1); break;
        case 'R': stroke(-1,-1,-1,1); stroke(-1,-1,0.7f,-1); stroke(0.7f,-1,1,-0.5f); stroke(1,-0.5f,0.7f,0); stroke(0.7f,0,-1,0); stroke(-0.1f,0,1,1); break;
        case 'S': stroke(1,-0.8f,-1,-1); stroke(-1,-1,-1,0); stroke(-1,0,1,0); stroke(1,0,1,1); stroke(1,1,-1,0.8f); break;
        case 'T': stroke(-1,-1,1,-1); stroke(0,-1,0,1); break;
        case 'U': stroke(-1,-1,-1,0.7f); stroke(-1,0.7f,0,1); stroke(0,1,1,0.7f); stroke(1,0.7f,1,-1); break;
        case 'V': stroke(-1,-1,0,1); stroke(1,-1,0,1); break;
        case 'W': stroke(-1,-1,-0.5f,1); stroke(-0.5f,1,0,0); stroke(0,0,0.5f,1); stroke(0.5f,1,1,-1); break;
        case 'X': stroke(-1,-1,1,1); stroke(1,-1,-1,1); break;
        case 'Y': stroke(-1,-1,0,0); stroke(1,-1,0,0); stroke(0,0,0,1); break;
        case 'Z': stroke(-1,-1,1,-1); stroke(1,-1,-1,1); stroke(-1,1,1,1); break;
        default: break;
    }
}

void Game::drawText(std::vector<DrawCmd>& out, const char* text, float cx, float cy,
                    float h, float r, float g, float b, float a) const {
    int total = 0;
    for (const char* p = text; *p; p++) total++;
    if (total == 0) return;
    float step = h * 0.95f;  // stroke font: wider spacing than 7-seg digits
    float startX = cx - (float)(total - 1) * step * 0.5f;
    for (int i = 0; text[i]; i++) {
        if (text[i] != ' ')
            drawLetter(out, text[i], startX + (float)i * step, cy, h, r, g, b, a);
    }
}

// A brick: a flat body in its row colour, a lighter top bevel and a darker
// bottom edge so the wall reads as solid blocks. Steel is grey with two
// rivets; tougher bricks carry a lighter inner core per hit left so their
// state is visible at a glance.
void Game::drawBrick(std::vector<DrawCmd>& out, const Brick& b, float alpha) const {
    float x = brickX(b), y = brickY(b);
    const float* c = (b.type == BRICK_STEEL) ? kSteelCol : kRowCol[b.row % 9];
    emit(out, SHAPE_QUAD, x, y, kBrickHW, kBrickHH, 0.0f, c[0], c[1], c[2], alpha);
    // top bevel (lighter) and bottom edge (darker)
    emit(out, SHAPE_QUAD, x, y - kBrickHH * 0.72f, kBrickHW, kBrickHH * 0.28f, 0.0f,
         c[0] * 0.6f + 0.4f, c[1] * 0.6f + 0.4f, c[2] * 0.6f + 0.4f, alpha * 0.9f);
    emit(out, SHAPE_QUAD, x, y + kBrickHH * 0.78f, kBrickHW, kBrickHH * 0.22f, 0.0f,
         c[0] * 0.45f, c[1] * 0.45f, c[2] * 0.45f, alpha);
    if (b.type == BRICK_STEEL) {
        for (int s = -1; s <= 1; s += 2)
            emit(out, SHAPE_DISC, x + (float)s * kBrickHW * 0.6f, y, 0.012f, 0.012f, 0.0f,
                 0.25f, 0.27f, 0.32f, alpha);
    } else if (b.hp > 1) {
        // One inner core per extra hit the brick can still take.
        for (int i = 1; i < b.hp; i++) {
            float k = 1.0f - 0.30f * (float)i;
            emit(out, SHAPE_QUAD, x, y, kBrickHW * k, kBrickHH * k, 0.0f,
                 c[0] * 0.5f + 0.5f, c[1] * 0.5f + 0.5f, c[2] * 0.5f + 0.5f, alpha * 0.55f);
        }
    }
}

// The paddle: a rounded bar (quad + end caps) with a bright top line.
void Game::drawPaddle(std::vector<DrawCmd>& out, float cx, float cy, float halfW,
                      float alpha) const {
    float hh = kPaddleHH;
    emit(out, SHAPE_QUAD, cx, cy, halfW - hh, hh, 0.0f, 0.30f, 0.72f, 0.95f, alpha);
    for (int s = -1; s <= 1; s += 2)
        emit(out, SHAPE_DISC, cx + (float)s * (halfW - hh), cy, hh, hh, 0.0f,
             0.30f, 0.72f, 0.95f, alpha);
    emit(out, SHAPE_QUAD, cx, cy - hh * 0.55f, halfW - hh, hh * 0.25f, 0.0f,
         0.80f, 0.95f, 1.00f, alpha * 0.9f);
}

// The ball: a soft halo under a bright disc.
void Game::drawBall(std::vector<DrawCmd>& out, float cx, float cy, float alpha) const {
    emit(out, SHAPE_DISC, cx, cy, kBallR * 2.4f, kBallR * 2.4f, 0.0f,
         1.00f, 0.90f, 0.55f, alpha * 0.35f, (float)STYLE_GLOW);
    emit(out, SHAPE_DISC, cx, cy, kBallR, kBallR, 0.0f, 1.00f, 0.97f, 0.85f, alpha);
}

// A bonus capsule: a coloured disc with a glow and a small glyph for its
// kind — a bar (wide), a dial (slow), three dots (multi), a plus (life).
void Game::drawPowerUp(std::vector<DrawCmd>& out, const PowerUp& pu) const {
    const float* c = kPowerUpCol[pu.type];
    float glow = kPowerUpR * (1.6f + 0.25f * sinf(animTime_ * 5.0f));
    emit(out, SHAPE_DISC, pu.x, pu.y, glow, glow, 0.0f, c[0], c[1], c[2], 0.35f, (float)STYLE_GLOW);
    emit(out, SHAPE_DISC, pu.x, pu.y, kPowerUpR, kPowerUpR, 0.0f, c[0], c[1], c[2], 1.0f);
    const float ir = 0.04f, ig = 0.06f, ib = 0.14f;   // glyph colour: the background
    const float k = kPowerUpR / 0.035f;              // glyphs scale with the capsule
    switch (pu.type) {
        case PU_WIDE:
            emit(out, SHAPE_QUAD, pu.x, pu.y, 0.022f * k, 0.005f * k, 0.0f, ir, ig, ib, 1.0f);
            break;
        case PU_SLOW:
            emit(out, SHAPE_DISC, pu.x, pu.y, 0.016f * k, 0.016f * k, 0.0f, ir, ig, ib, 1.0f);
            emit(out, SHAPE_DISC, pu.x, pu.y, 0.011f * k, 0.011f * k, 0.0f, c[0], c[1], c[2], 1.0f);
            emit(out, SHAPE_QUAD, pu.x, pu.y - 0.005f * k, 0.0025f * k, 0.006f * k, 0.0f, ir, ig, ib, 1.0f);
            break;
        case PU_MULTI:
            emit(out, SHAPE_DISC, pu.x, pu.y - 0.010f * k, 0.006f * k, 0.006f * k, 0.0f, ir, ig, ib, 1.0f);
            emit(out, SHAPE_DISC, pu.x - 0.011f * k, pu.y + 0.007f * k, 0.006f * k, 0.006f * k, 0.0f, ir, ig, ib, 1.0f);
            emit(out, SHAPE_DISC, pu.x + 0.011f * k, pu.y + 0.007f * k, 0.006f * k, 0.006f * k, 0.0f, ir, ig, ib, 1.0f);
            break;
        case PU_LIFE:
            emit(out, SHAPE_QUAD, pu.x, pu.y, 0.018f * k, 0.005f * k, 0.0f, ir, ig, ib, 1.0f);
            emit(out, SHAPE_QUAD, pu.x, pu.y, 0.005f * k, 0.018f * k, 0.0f, ir, ig, ib, 1.0f);
            break;
    }
}

void Game::drawPowerUpHUD(std::vector<DrawCmd>& out) const {
    // The timed bonuses with their remaining time as a bar, between the
    // paddle line and the balls-left dots at the bottom of the disc.
    const float y = 0.71f;
    int row = 0;
    auto timedRow = [&](float timer, float duration, const float* c) {
        float x0 = -0.30f + (float)row * 0.34f;
        float progress = timer / duration;
        float pulse = (timer < 2.0f) ? (0.5f + 0.5f * sinf(animTime_ * 18.0f)) : 1.0f;
        emit(out, SHAPE_DISC, x0, y, 0.018f, 0.018f, 0.0f, c[0], c[1], c[2], pulse);
        const float barX0 = x0 + 0.04f, barMaxW = 0.22f;
        emit(out, SHAPE_QUAD, barX0 + barMaxW * 0.5f, y, barMaxW * 0.5f, 0.006f, 0.0f,
             c[0] * 0.3f, c[1] * 0.3f, c[2] * 0.3f, 0.5f);
        float fillW = barMaxW * progress;
        if (fillW > 0.002f)
            emit(out, SHAPE_QUAD, barX0 + fillW * 0.5f, y, fillW * 0.5f, 0.006f, 0.0f,
                 c[0], c[1], c[2], 0.85f);
        row++;
    };
    if (wideTimer_ > 0.0f) timedRow(wideTimer_, kWideDuration, kPowerUpCol[PU_WIDE]);
    if (slowTimer_ > 0.0f) timedRow(slowTimer_, kSlowDuration, kPowerUpCol[PU_SLOW]);
}

void Game::drawGearIcon(std::vector<DrawCmd>& out, float cx, float cy, float size,
                         float r, float g, float b, float a) const {
    // Circular body
    const float bodyR = size * 0.68f;
    emit(out, SHAPE_DISC, cx, cy, bodyR, bodyR, 0.0f, r, g, b, a);

    // 6 rectangular teeth protruding clearly beyond the body
    const float toothCR = size * 0.94f;   // center of tooth from gear center
    const float toothHW = size * 0.20f;   // tooth half-width  (tangential)
    const float toothHH = size * 0.30f;   // tooth half-height (radial)
    for (int i = 0; i < 6; i++) {
        float ang = (float)i * 1.0472f;  // 60° apart
        emit(out, SHAPE_QUAD,
             cx + cosf(ang) * toothCR,
             cy + sinf(ang) * toothCR,
             toothHW, toothHH, ang, r, g, b, a);
    }

    // Centre hole punched through in background colour
    emit(out, SHAPE_DISC, cx, cy, size * 0.30f, size * 0.30f, 0.0f,
         0.03f, 0.04f, 0.09f, a);
}

void Game::drawSettingsScreen(std::vector<DrawCmd>& out) const {
    // Full-screen dark overlay
    emit(out, SHAPE_QUAD, 0.0f, 0.0f, asp_, 1.0f, 0.0f, 0.04f, 0.06f, 0.14f, 0.93f);

    drawText(out, "SETTINGS", 0.0f, -0.60f, 0.085f, 0.55f, 0.78f, 1.00f, 1.0f);

    // Row labels are left-aligned at one margin inside the disc; the chips
    // sit at the right. At these heights the disc is still nearly full width.
    const float labelH    = 0.060f;
    const float labelLeft = -0.72f;
    const float toggleX   =  0.48f;
    auto labelWidth = [&](const char* text) {
        int n = 0;
        for (const char* p = text; *p; p++) n++;
        return (float)(n - 1) * labelH * 0.95f + labelH * 0.76f;   // glyph pitch + one glyph
    };
    auto label = [&](const char* text, float y) {
        drawText(out, text, labelLeft + labelWidth(text) * 0.5f, y, labelH,
                 0.80f, 0.85f, 0.90f, 0.9f);
    };
    auto chip = [&](bool on, float y) {
        emit(out, SHAPE_QUAD, toggleX, y, 0.110f, 0.050f, 0.0f,
             on ? 0.08f : 0.20f, on ? 0.48f : 0.18f, on ? 0.08f : 0.18f, 0.75f);
        drawText(out, on ? "ON" : "OFF", toggleX, y, 0.055f,
                 on ? 0.35f : 0.65f, on ? 1.00f : 0.45f, on ? 0.35f : 0.45f, 1.0f);
    };

    label("SOUND", kSettingSoundY);
    chip(soundEnabled_, kSettingSoundY);
    label("AUTO PLAY", kSettingAutoPlayY);
    chip(autoPlayActive_, kSettingAutoPlayY);

    // Back button
    emit(out, SHAPE_QUAD, 0.0f, kSettingBackY, 0.16f, 0.062f, 0.0f, 0.22f, 0.22f, 0.25f, 0.82f);
    drawText(out, "BACK", 0.0f, kSettingBackY, 0.065f, 0.85f, 0.85f, 0.92f, 1.0f);
}

// The big end-screen score keeps this height until the digit count would
// clip at the sides of the disc, then shrinks to fit. drawNumber spaces
// digit centres 0.87h apart (0.6h glyph width x 1.45) and a glyph spans
// 0.6h, so n digits span (0.87(n-1) + 0.6)h.
static float endScoreHeight(int digits) {
    const float kMaxH = 0.26f;
    float span = 0.87f * (float)(digits - 1) + 0.60f;
    float h = 1.50f / span;   // fill at most 1.5 world units of the disc's width
    return h < kMaxH ? h : kMaxH;
}

void Game::render(std::vector<DrawCmd>& out) {
    float pulse = 0.5f + 0.5f * sinf(animTime_ * 4.0f);
    // The Settings overlay is drawn over whatever screen it was opened from.
    const State scene = (state_ == SETTINGS) ? prevState_ : state_;
    const bool inRound = scene == PLAYING || scene == LEVEL_CLEAR ||
                         scene == GAME_OVER || scene == WIN;

    // The wall: dimmed under the title, full strength in a round.
    float wallAlpha = (scene == TITLE) ? 0.85f : 1.0f;
    for (auto& b : bricks_)
        if (b.alive) drawBrick(out, b, wallAlpha);

    // The lane: its walls and ceiling, faint lines the ball bounces off.
    emit(out, SHAPE_QUAD, 0.0f, kTopWallY, kFieldHW, 0.004f, 0.0f, 0.40f, 0.65f, 0.95f, 0.30f);
    for (int sgn = -1; sgn <= 1; sgn += 2)
        emit(out, SHAPE_QUAD, (float)sgn * kFieldHW, 0.0f, 0.004f, 0.60f, 0.0f,
             0.40f, 0.65f, 0.95f, 0.30f);

    if (scene == TITLE) {
        drawBall(out, demoBallX_, demoBallY_, 1.0f);
        drawPaddle(out, demoPaddleX_, kPaddleY, kPaddleHW, 0.5f + 0.5f * pulse);
    }

    if (inRound) {
        for (auto& pu : powerUps_)
            if (pu.alive) drawPowerUp(out, pu);
        for (auto& b : balls_)
            if (b.alive) drawBall(out, b.x, b.y, 1.0f);
        float paddleAlpha = (scene == GAME_OVER) ? 0.35f : 1.0f;
        drawPaddle(out, paddleX_, kPaddleY, paddleHalfW(), paddleAlpha);
    }

    // explosion flash rings (expanding, fading, glow falloff)
    for (auto& e : explosions_) {
        float t = e.t / e.maxLife;                // 0→1
        float s = e.radius * (1.0f + t * 3.0f);   // expand outward
        float a = (1.0f - t) * 0.85f;
        emit(out, SHAPE_DISC, e.x, e.y, s, s, 0.0f, e.cr, e.cg, e.cb, a, (float)STYLE_GLOW);
    }

    // debris fragments
    for (auto& p : particles_) {
        float life = 1.0f - p.t / p.maxLife;     // 1→0
        float sz   = p.size * life;
        emit(out, SHAPE_QUAD, p.x, p.y, sz, sz, p.rot, p.r, p.g, p.b, life);
    }

    // Bonus flash: the points just earned, fading where they were earned.
    if (bonusFlashTimer_ > 0.0f) {
        float alpha = bonusFlashTimer_ / 0.9f;
        int n = numDigits(bonusFlash_);
        float h = 0.07f, step = h * 0.60f * 1.45f;
        drawNumber(out, bonusFlash_, bonusFlashX_ - (float)(n - 1) * step * 0.5f,
                   bonusFlashY_, h, 1.0f, 0.85f, 0.10f, alpha);
    }

    // HUD during gameplay — drawn without screen shake so it stays readable.
    // Score at the top of the disc, the level under it, the balls left as
    // dots at the bottom.
    if (scene == PLAYING || scene == LEVEL_CLEAR) {
        ShakeOff noShake(shakeX_, shakeY_);

        float h = 0.085f;
        float step = h * 0.60f * 1.45f;
        int n = numDigits(score_);
        drawNumber(out, score_, -(float)(n - 1) * step * 0.5f, -0.84f, h,
                   1.0f, 1.0f, 1.0f, 1.0f);
        int nd = numDigits(level_);
        float lh = 0.055f, lstep = lh * 0.60f * 1.45f;
        drawNumber(out, level_, -(float)(nd - 1) * lstep * 0.5f, -0.71f, lh,
                   1.0f, 0.85f, 0.2f, 1.0f);
        // balls left as dots
        float gap = 0.07f;
        float startX = -(float)(lives_ - 1) * gap * 0.5f;
        for (int i = 0; i < lives_; i++)
            emit(out, SHAPE_DISC, startX + (float)i * gap, 0.80f, 0.018f, 0.018f, 0.0f,
                 0.30f, 0.72f, 0.95f, 1.0f);

        drawPowerUpHUD(out);

        // A served ball waits for a tap (or launches itself).
        if (scene == PLAYING && ballHeld_ && serveTimer_ <= kServeDelay - kServeGrace)
            drawText(out, "TAP", 0.0f, 0.42f, 0.055f, 0.65f, 0.80f, 1.00f, 0.4f + 0.5f * pulse);
    }

    if (scene == TITLE) {
        drawText(out, "VULKAN",   0.0f, -0.80f, 0.070f, 0.35f, 0.78f, 1.00f, 1.0f);
        drawText(out, "BREAKOUT", 0.0f, -0.06f, 0.105f, 1.00f, 0.80f, 0.30f, 1.0f);

        // High score podium (top 3, gold/silver/bronze)
        const float hh = 0.045f, rowY[3] = {0.08f, 0.18f, 0.28f};
        for (int i = 0; i < 3; i++) {
            if (highScores_[i].score <= 0) break;
            float pr = kPodiumCol[i][0], pg = kPodiumCol[i][1], pb = kPodiumCol[i][2];
            emit(out, SHAPE_DISC, -0.50f, rowY[i], 0.012f, 0.012f, 0.0f, pr, pg, pb, 1.0f);
            drawNumber(out, highScores_[i].score, -0.42f, rowY[i], hh, pr, pg, pb, 1.0f);
            drawNumber(out, highScores_[i].level, 0.40f, rowY[i], hh, pr, pg, pb, 0.80f);
        }

        drawText(out, "TAP TO PLAY", 0.0f, 0.42f, 0.050f, 0.65f, 0.80f, 1.00f, 0.4f + 0.5f * pulse);
    } else if (scene == LEVEL_CLEAR) {
        // big green level number just cleared
        drawText(out, "LEVEL", 0.0f, -0.28f, 0.080f, 0.4f, 1.0f, 0.5f, 1.0f);
        int nd = numDigits(level_);
        float lh = 0.40f, lstep = lh * 0.60f * 1.45f;
        drawNumber(out, level_, -(float)(nd - 1) * lstep * 0.5f, 0.0f, lh, 0.4f, 1.0f, 0.5f, 1.0f);
    } else if (scene == GAME_OVER) {
        const float tint[4] = {0.6f, 0.05f, 0.08f, 0.32f + 0.10f * pulse};
        drawEndScreen(out, "GAME OVER", pulse, tint, 0.75f + 0.20f * pulse, 0.10f, 1.0f, 1.0f);
    } else if (scene == WIN) {
        const float tint[4] = {0.1f, 0.5f, 0.15f, 0.30f + 0.10f * pulse};
        drawEndScreen(out, "YOU WIN", pulse, tint, 0.80f + 0.15f * pulse, 0.10f, 0.9f, 0.3f);
    }

    // ── Gear button — the bottom of the disc, on the title and end screens ──
    if (state_ == TITLE || state_ == GAME_OVER || state_ == WIN) {
        ShakeOff noShake(shakeX_, shakeY_);
        float gearA = 0.55f + 0.12f * sinf(animTime_ * 2.0f);
        float gr = autoPlayActive_ ? 0.25f : 0.55f;
        float gg = autoPlayActive_ ? 1.00f : 0.62f;
        float gb = autoPlayActive_ ? 0.35f : 0.78f;
        drawGearIcon(out, kGearWX, kGearWY, 0.055f, gr, gg, gb, gearA);
    }
    // While the autopilot drives, a small pulsing AUTO badge sits at the top.
    if (autoPlayActive_ && (scene == PLAYING || scene == LEVEL_CLEAR)) {
        ShakeOff noShake(shakeX_, shakeY_);
        float autoA = 0.38f + 0.28f * sinf(animTime_ * 3.5f);
        drawText(out, "AUTO", 0.0f, -0.935f, 0.030f, 0.25f, 1.00f, 0.38f, autoA);
    }

    // ── Settings overlay — drawn last so it covers everything ────────────────
    if (state_ == SETTINGS) {
        ShakeOff noShake(shakeX_, shakeY_);
        drawSettingsScreen(out);
    }
}

void Game::drawEndScreen(std::vector<DrawCmd>& out, const char* title, float pulse,
                         const float tint[4], float scoreG, float scoreB,
                         float plainG, float plainB) const {
    emit(out, SHAPE_QUAD, 0.0f, 0.0f, asp_, 1.0f, 0.0f, tint[0], tint[1], tint[2], tint[3]);
    int n = numDigits(score_);
    float fh = endScoreHeight(n), fw = fh * 0.6f * 1.45f;
    float firstCx = -(float)(n - 1) * fw * 0.5f;
    // Gold pulsing score if new high score, plain otherwise
    float sg = newHighScore_ ? scoreG : plainG, sb = newHighScore_ ? scoreB : plainB;
    drawNumber(out, score_, firstCx, -0.05f, fh, 1.0f, sg, sb, 1.0f);
    // Rank medal under the score when it's a new high score (above it is
    // the wall).
    if (newHighScore_ && newHighScoreRank_ >= 0 && newHighScoreRank_ < 3) {
        int ri = newHighScoreRank_;
        drawDigit(out, ri + 1, 0.0f, 0.17f, 0.10f,
                  kPodiumCol[ri][0], kPodiumCol[ri][1], kPodiumCol[ri][2],
                  0.65f + 0.35f * pulse);
    }
    drawText(out, title, 0.0f, 0.33f, 0.070f, 1.0f, 1.0f, 1.0f, 0.5f + 0.5f * pulse);
}

void Game::clearColor(float out[3]) const {
    out[0] = 0.03f; out[1] = 0.04f; out[2] = 0.09f;
}
