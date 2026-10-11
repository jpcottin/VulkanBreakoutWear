// Stub AudioEngine for the game_tests binary and the host bench — no Oboe
// dependency needed.
#include "audio.h"
AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() = default;   // impl_ stays empty in the stub
// NOLINT below: the stub ignores `this`, but the real audio.cpp impl doesn't,
// so the shared declaration in audio.h cannot be static.
bool AudioEngine::init()                 { return false; }  // NOLINT(readability-convert-member-functions-to-static)
void AudioEngine::shutdown()             {}
void AudioEngine::pause()                {}
void AudioEngine::resume()               {}
void AudioEngine::triggerPaddle()        {}
void AudioEngine::triggerWall()          {}
void AudioEngine::triggerBrickHit()      {}
void AudioEngine::triggerBrickBreak(float) {}
void AudioEngine::triggerBallLost()      {}
void AudioEngine::triggerLevelClear()    {}
void AudioEngine::triggerPowerUp()       {}
void AudioEngine::triggerExtraLife()     {}
void AudioEngine::setMusicEnabled(bool)  {}
