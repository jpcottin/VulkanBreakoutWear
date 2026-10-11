#pragma once
#include <memory>

// Pimpl facade — callers need no Oboe headers. Every sound is synthesised in
// real time; there are no audio files.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    bool init();
    void shutdown();
    // Activity lost / regained focus: silence the stream without closing it.
    void pause();
    void resume();

    void triggerPaddle();              // ball off the paddle
    void triggerWall();                // ball off a side or top wall
    void triggerBrickHit();            // brick damaged but still standing
    void triggerBrickBreak(float hz);  // brick destroyed; pitched by the caller (row)
    void triggerBallLost();
    void triggerLevelClear();
    void triggerPowerUp();
    void triggerExtraLife();
    void setMusicEnabled(bool enabled);

private:
    struct Impl;
    // Shared, not owned outright: Oboe's error thread holds a reference while
    // it delivers onErrorAfterClose, so the Impl outlives a teardown that
    // races a route change.
    std::shared_ptr<Impl> impl_;
};
