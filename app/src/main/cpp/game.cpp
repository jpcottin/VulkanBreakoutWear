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

// ---- wall geometry (world units) ----
// Bricks are big: five columns across the lane, so they read on a watch.
static const float kBrickHW   = 0.110f;   // half-width
static const float kBrickHH   = 0.045f;   // half-height
static const float kBrickGapX = 0.025f;
static const float kBrickGapY = 0.025f;
static const float kWallTopY  = -0.50f;   // centre of row 0
static const float kSideMargin = 0.020f;  // smallest gap between the wall and the lane's side

// Paddle line and the ball.
static const float kPaddleY   = 0.60f;
static const float kPaddleHW  = 0.160f;   // half-width (normal)
static const float kPaddleHH  = 0.025f;
static const float kBallR     = 0.030f;
// One crown detent moves the paddle this far.
static const float kCrownStep = 0.10f;

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

// 7-segment masks for digits 0..9 (bit a=1,b=2,c=4,d=8,e=16,f=32,g=64).
static const int kDigitSeg[10] = {63, 6, 91, 79, 102, 109, 125, 7, 127, 111};

// Gear icon world position — the bottom of the disc, on the title and end
// screens. Single source of truth for rendering and hit-testing.
static constexpr float kGearWX = 0.0f;
static constexpr float kGearWY = 0.82f;

// Settings row y-positions — shared between drawSettingsScreen() and the tap handler.
static constexpr float kSettingSoundY = -0.22f;
static constexpr float kSettingBackY  =  0.56f;

static const uint32_t kSettingsMagic = 0x53455454u;  // "SETT"


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

// A detent of the crown: step the paddle position and clamp it to the lane.
void Game::onRotary(float delta) {
    crownX_ += delta * kCrownStep;
    float lim = kFieldHW - kPaddleHW;
    if (crownX_ > lim) crownX_ = lim;
    if (crownX_ < -lim) crownX_ = -lim;
    crownRecent_ = 0.0f;
}

// ── Persistence ───────────────────────────────────────────────────────────────

// Write `buf` to `path` atomically: a crash between truncate and write can't
// leave a half-written file behind.
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
    loadSettings();
}

// The single rule for the music flag: it plays from the start of a round
// through the end screen (and under a Settings overlay opened from those),
// when sound is on.
void Game::syncMusic() {
    if (!audio_) return;
    State s = (state_ == SETTINGS) ? prevState_ : state_;
    audio_->setMusicEnabled(soundEnabled_ && s != TITLE);
}

// Resume a run after process death. Bounds-checked because the saved blob
// comes back from the OS, not from us. Gameplay arrives with the next step of
// the project; until then there is no run to resume.
void Game::restoreSession(int level, long score, int lives) {
    if (level < 1 || level > 10 || lives < 1 || lives > 9 || score < 0) return;
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

// The title screen's wall: three full rows, one colour per row.
void Game::buildDemoWall() {
    wallCols_ = wallCols();
    bricks_.clear();
    bricks_.reserve((size_t)3 * wallCols_);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < wallCols_; c++)
            bricks_.push_back({r, c, BRICK_NORMAL, 1, true});
    demoBallX_ = 0.0f; demoBallY_ = 0.20f;
    demoBallVx_ = 0.55f; demoBallVy_ = 0.80f;
    demoPaddleX_ = 0.0f;
}

// ── Update ────────────────────────────────────────────────────────────────────

void Game::update(float dt) {
    if (dt > 0.05f) dt = 0.05f;   // clamp huge hitches
    animTime_ += dt;
    if (crownRecent_ < 10.0f) crownRecent_ += dt;

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
            float target = crownRecent_ < 2.0f ? crownX_ : demoBallX_;
            float dx = target - demoPaddleX_;
            float stepX = 2.0f * dt;
            if (fabsf(dx) <= stepX) demoPaddleX_ = target;
            else demoPaddleX_ += dx > 0.0f ? stepX : -stepX;
            break;
        }
        case PLAYING:
        case LEVEL_CLEAR:
        case GAME_OVER:
        case WIN:
            break;
        case SETTINGS: {
            if (tapped) {
                float wy = 2.0f * tapY_ / (float)vh_ - 1.0f;
                float wx = (2.0f * tapX_ / (float)vw_ - 1.0f) * asp_;
                const float kHitH = 0.13f;
                if (fabsf(wy - kSettingSoundY) < kHitH) {
                    soundEnabled_ = !soundEnabled_;
                    saveSettings();
                    syncMusic();
                } else if (fabsf(wy - kSettingBackY) < kHitH && fabsf(wx) < 0.25f) {
                    onPointersCancel();
                    state_ = prevState_;
                }
            }
            break;
        }
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
    d.tx = wx / asp_;
    d.ty = wy;
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

    // Sound row
    label("SOUND", kSettingSoundY);
    emit(out, SHAPE_QUAD, toggleX, kSettingSoundY, 0.110f, 0.050f, 0.0f,
         soundEnabled_ ? 0.08f : 0.20f,
         soundEnabled_ ? 0.48f : 0.18f,
         soundEnabled_ ? 0.08f : 0.18f, 0.75f);
    drawText(out, soundEnabled_ ? "ON" : "OFF", toggleX, kSettingSoundY, 0.055f,
             soundEnabled_ ? 0.35f : 0.65f,
             soundEnabled_ ? 1.00f : 0.45f,
             soundEnabled_ ? 0.35f : 0.45f, 1.0f);

    // Back button
    emit(out, SHAPE_QUAD, 0.0f, kSettingBackY, 0.16f, 0.062f, 0.0f, 0.22f, 0.22f, 0.25f, 0.82f);
    drawText(out, "BACK", 0.0f, kSettingBackY, 0.065f, 0.85f, 0.85f, 0.92f, 1.0f);
}

void Game::render(std::vector<DrawCmd>& out) {
    float pulse = 0.5f + 0.5f * sinf(animTime_ * 4.0f);
    // The Settings overlay is drawn over whatever screen it was opened from.
    const State scene = (state_ == SETTINGS) ? prevState_ : state_;

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
        drawText(out, "VULKAN",   0.0f, -0.80f, 0.070f, 0.35f, 0.78f, 1.00f, 1.0f);
        drawText(out, "BREAKOUT", 0.0f, -0.06f, 0.105f, 1.00f, 0.80f, 0.30f, 1.0f);
        drawText(out, "TAP TO PLAY", 0.0f, 0.42f, 0.050f, 0.65f, 0.80f, 1.00f, 0.4f + 0.5f * pulse);
    }

    // ── Gear button — the bottom of the disc, on the title and end screens ──
    if (state_ == TITLE || state_ == GAME_OVER || state_ == WIN) {
        float gearA = 0.55f + 0.12f * sinf(animTime_ * 2.0f);
        drawGearIcon(out, kGearWX, kGearWY, 0.055f, 0.55f, 0.62f, 0.78f, gearA);
    }

    // ── Settings overlay — drawn last so it covers everything ────────────────
    if (state_ == SETTINGS) drawSettingsScreen(out);
}

void Game::clearColor(float out[3]) const {
    out[0] = 0.03f; out[1] = 0.04f; out[2] = 0.09f;
}
