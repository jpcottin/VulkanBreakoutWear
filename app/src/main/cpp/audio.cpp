#include "audio.h"
#include "common.h"
#include <oboe/Oboe.h>
#include <atomic>
#include <array>
#include <cmath>
#include <mutex>

// ── Impl ──────────────────────────────────────────────────────────────────────

struct AudioEngine::Impl : public oboe::AudioStreamDataCallback,
                           public oboe::AudioStreamErrorCallback,
                           public std::enable_shared_from_this<AudioEngine::Impl> {

    static constexpr int   kVoices = 8;
    static constexpr float kTau    = 6.28318530f;

    enum class ST : uint8_t { PADDLE, WALL, HIT, BREAK, LOST, CLEAR, POWERUP, LIFE };

    struct Voice {
        ST    type   = ST::PADDLE;
        float t      = 0.0f;
        float phase  = 0.0f;
        float freq   = 0.0f;   // BREAK: pitch chosen at trigger time
        bool  active = false;
    };

    // `stream` is touched by the game thread (open/close/pause/resume) and by
    // Oboe's error thread (onErrorAfterClose); `streamMutex` guards it. Oboe
    // calls are made outside the lock so lifecycle calls never wait on
    // audioserver.
    std::mutex                         streamMutex;
    std::shared_ptr<oboe::AudioStream> stream;
    bool                               closing = false;
    bool                               paused  = false;
    std::array<Voice, kVoices>         voices{};

    // The stream opens at the device's native rate (no resampler in the
    // path); the synth steps every oscillator by this. Published together
    // with the stream it describes, before that stream starts; the audio
    // thread reads it per sample.
    std::atomic<float> sampleDt{1.0f / 48000.0f};

    // Bits: 0=PADDLE 1=WALL 2=HIT 3=LOST 4=CLEAR 5=POWERUP 6=LIFE – game
    // thread writes, the audio callback drains.
    std::atomic<uint32_t> pending{0};
    // > 0: play a brick-break voice at that pitch (Hz). Game thread writes.
    std::atomic<float>    breakPending{0.0f};
    std::atomic<bool>     musicEnabled{false};

    uint32_t rng = 0xDEADBEEFu;

    // ── Background music state (audio thread only) ────────────────────────────
    // 92 BPM ambient track in C major: pad + bass + arpeggio.
    static constexpr float kTempo       = 92.0f / 60.0f;          // beats/s
    static constexpr float kArpInterval = 60.0f / 92.0f / 2.0f;   // 8th note
    static constexpr float kPadFreqs[3] = {130.8f, 164.8f, 196.0f}; // C3 E3 G3
    static constexpr float kArpFreqs[4] = {261.6f, 329.6f, 392.0f, 523.3f}; // C4 E4 G4 C5

    // Wrapping phases instead of an ever-growing time accumulator: a float
    // that keeps counting seconds loses the per-sample step after ~8 minutes.
    float tremPhase_   = 0.0f;   // [0,1) at 0.25 Hz
    float barPhase_    = 0.0f;   // [0,4) beats
    float padPhase_[3] = {};
    float bassPhase_   = 0.0f;
    float bassEnv_     = 0.0f;
    float arpPhase_    = 0.0f;
    float arpEnv_      = 0.55f;  // start audible on step 0 immediately
    float arpTimer_    = 0.0f;
    int   arpStep_     = 0;
    int   lastBeat_    = -1;

    // ── helpers ──────────────────────────────────────────────────────────────

    float white() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return static_cast<float>(rng & 0xFFFFu) / 32768.0f - 1.0f;
    }

    void activate(ST type, float freq = 0.0f) {
        for (auto& v : voices) {
            if (!v.active) { v = {type, 0.0f, 0.0f, freq, true}; return; }
        }
        voices[0] = {type, 0.0f, 0.0f, freq, true};
    }

    // Advance a wrapping oscillator phase and return the sine at it.
    static float osc(float& phase, float freq, float dt) {
        phase += dt * freq;
        if (phase >= 1.0f) phase -= 1.0f;
        return sinf(phase * kTau);
    }

    float genVoice(Voice& v) {
        const float dt = sampleDt.load(std::memory_order_relaxed);
        float s = 0.0f;

        switch (v.type) {
            case ST::PADDLE: {
                // Short bright blip, pitch dipping as it decays.
                constexpr float dur = 0.07f;
                if (v.t > dur) { v.active = false; break; }
                float prog = v.t / dur;
                s = osc(v.phase, 560.0f - 140.0f * prog, dt) * (1.0f - prog) * 0.38f;
                break;
            }
            case ST::WALL: {
                // Dull low tick: a square wave with a very fast decay.
                constexpr float dur = 0.045f;
                if (v.t > dur) { v.active = false; break; }
                v.phase += dt * 240.0f;
                if (v.phase >= 1.0f) v.phase -= 1.0f;
                float sq = v.phase < 0.5f ? 1.0f : -1.0f;
                s = sq * expf(-v.t / 0.012f) * 0.22f;
                break;
            }
            case ST::HIT: {
                // A brick taking a hit and holding: a hard click with no tone.
                constexpr float dur = 0.06f;
                if (v.t > dur) { v.active = false; break; }
                float click = white() * expf(-v.t / 0.006f) * 0.30f;
                float knock = osc(v.phase, 900.0f, dt) * expf(-v.t / 0.02f) * 0.22f;
                s = click + knock;
                break;
            }
            case ST::BREAK: {
                // A brick shattering: pitched pop (the game picks the pitch by
                // row) over a burst of noise.
                constexpr float dur = 0.16f;
                if (v.t > dur) { v.active = false; break; }
                float pop   = osc(v.phase, v.freq, dt) * expf(-v.t / 0.045f) * 0.36f;
                float burst = white() * expf(-v.t / 0.015f) * 0.22f;
                s = pop + burst;
                break;
            }
            case ST::LOST: {
                // The ball dropping out: a slide down from 320 Hz to 70 Hz.
                constexpr float dur = 0.45f;
                if (v.t > dur) { v.active = false; break; }
                float prog = v.t / dur;
                float freq = 320.0f * powf(70.0f / 320.0f, prog);
                s = osc(v.phase, freq, dt) * (1.0f - prog * prog) * 0.42f;
                break;
            }
            case ST::CLEAR: {
                // Rising four-note arpeggio: C5 E5 G5 C6.
                constexpr float dur     = 0.64f;
                constexpr float segDur  = dur / 4.0f;
                constexpr float freqs[4]= {523.25f, 659.25f, 783.99f, 1046.5f};
                if (v.t > dur) { v.active = false; break; }
                int   ni  = static_cast<int>(v.t / segDur);
                if (ni > 3) ni = 3;
                float nt  = fmodf(v.t, segDur);
                float env = sinf(nt / segDur * 3.14159265f);
                s = osc(v.phase, freqs[ni], dt) * env * 0.45f;
                break;
            }
            case ST::POWERUP: {
                // Rising sparkle: four quick notes over 0.28 s.
                constexpr float dur      = 0.28f;
                constexpr float noteDur  = dur / 4.0f;
                constexpr float noteFreq[4] = {440.0f, 554.4f, 659.3f, 880.0f};
                if (v.t > dur) { v.active = false; break; }
                int   ni  = (int)(v.t / noteDur);
                if (ni > 3) ni = 3;
                float nt  = fmodf(v.t, noteDur);
                float env = sinf(nt / noteDur * 3.14159265f);
                s = osc(v.phase, noteFreq[ni], dt) * env * 0.38f;
                break;
            }
            case ST::LIFE: {
                // Extra life jingle: E5 G5 then a held C6.
                constexpr float dur = 0.55f;
                constexpr float freqs[3] = {659.25f, 783.99f, 1046.5f};
                constexpr float starts[3] = {0.0f, 0.11f, 0.22f};
                if (v.t > dur) { v.active = false; break; }
                int ni = v.t >= starts[2] ? 2 : (v.t >= starts[1] ? 1 : 0);
                float nt = v.t - starts[ni];
                float env = ni == 2 ? expf(-nt / 0.18f) : sinf(nt / 0.11f * 3.14159265f);
                s = osc(v.phase, freqs[ni], dt) * env * 0.40f;
                break;
            }
        }
        v.t += dt;
        return s;
    }

    float genMusic() {
        const float dt = sampleDt.load(std::memory_order_relaxed);
        float s = 0.0f;

        // Slow tremolo LFO at 0.25 Hz
        tremPhase_ += dt * 0.25f;
        if (tremPhase_ >= 1.0f) tremPhase_ -= 1.0f;
        float tremolo = 0.72f + 0.28f * sinf(tremPhase_ * kTau);

        // Pad chord: C3 + E3 + G3 sustained
        for (int i = 0; i < 3; i++)
            s += osc(padPhase_[i], kPadFreqs[i], dt) * 0.022f * tremolo;

        // Bass: C2 (65.4 Hz) on beats 1 and 3 of a 4-beat bar
        barPhase_ += dt * kTempo;
        if (barPhase_ >= 4.0f) barPhase_ -= 4.0f;
        int beat = (int)barPhase_;
        if (beat != lastBeat_) {
            lastBeat_ = beat;
            if (beat == 0 || beat == 2) bassEnv_ = 1.0f;
        }
        bassEnv_ *= 0.9989f;
        s += osc(bassPhase_, 65.4f, dt) * bassEnv_ * 0.065f;

        // Melody arpeggio: C4 E4 G4 C5 cycling on 8th notes
        arpTimer_ += dt;
        if (arpTimer_ >= kArpInterval) {
            arpTimer_ -= kArpInterval;
            arpStep_   = (arpStep_ + 1) & 3;
            arpEnv_    = 0.55f;
        }
        arpEnv_ *= 0.9988f;
        s += osc(arpPhase_, kArpFreqs[arpStep_], dt) * arpEnv_ * 0.018f;

        return s;
    }

    // ── Oboe callback ─────────────────────────────────────────────────────────

    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream*, void* data, int32_t frames) override {

        float* out = static_cast<float*>(data);

        uint32_t mask = pending.exchange(0, std::memory_order_relaxed);
        if (mask &  1u) activate(ST::PADDLE);
        if (mask &  2u) activate(ST::WALL);
        if (mask &  4u) activate(ST::HIT);
        if (mask &  8u) activate(ST::LOST);
        if (mask & 16u) activate(ST::CLEAR);
        if (mask & 32u) activate(ST::POWERUP);
        if (mask & 64u) activate(ST::LIFE);

        float hz = breakPending.exchange(0.0f, std::memory_order_relaxed);
        if (hz > 0.0f) activate(ST::BREAK, hz);

        const bool music = musicEnabled.load(std::memory_order_relaxed);

        for (int i = 0; i < frames; i++) {
            float s = 0.0f;
            for (auto& v : voices)
                if (v.active) s += genVoice(v);
            if (music) s += genMusic();
            // Soft clip: stacked breaks and blips compress instead of turning
            // into a square wave.
            out[i] = tanhf(s);
        }
        return oboe::DataCallbackResult::Continue;
    }

    // ── stream lifecycle ──────────────────────────────────────────────────────

    // Open (but don't start) an output stream at the device's native rate.
    // No lock held: this talks to audioserver.
    std::shared_ptr<oboe::AudioStream> openStream() {
        std::shared_ptr<oboe::AudioStream> s;
        oboe::AudioStreamBuilder builder;
        builder.setDirection(oboe::Direction::Output)
               ->setFormat(oboe::AudioFormat::Float)
               ->setChannelCount(oboe::ChannelCount::Mono)
               // onAudioReady writes mono float unconditionally; let Oboe
               // convert when the device stream differs, instead of the
               // callback misinterpreting the buffer (garbage audio).
               ->setFormatConversionAllowed(true)
               ->setChannelConversionAllowed(true)
               ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
               // Shared: exclusive endpoints are the ones AAudio takes away
               // when another app wants one, and this synth gains nothing
               // from the extra few ms.
               ->setSharingMode(oboe::SharingMode::Shared)
               ->setDataCallback(shared_from_this())
               ->setErrorCallback(shared_from_this());

        oboe::Result r = builder.openStream(s);
        if (r != oboe::Result::OK) {
            LOGW("Oboe openStream: %s", oboe::convertToText(r));
            return nullptr;
        }
        LOGI("Oboe stream: %d Hz, %s, %s", s->getSampleRate(),
             oboe::convertToText(s->getSharingMode()),
             oboe::convertToText(s->getPerformanceMode()));
        return s;
    }

    static void start(const std::shared_ptr<oboe::AudioStream>& s) {
        oboe::Result r = s->requestStart();
        if (r != oboe::Result::OK) LOGW("Oboe requestStart: %s", oboe::convertToText(r));
    }

    bool open() {
        std::shared_ptr<oboe::AudioStream> s = openStream();
        if (!s) return false;
        bool startIt;
        {
            std::lock_guard<std::mutex> lock(streamMutex);
            closing = false;
            stream = s;
            sampleDt.store(1.0f / (float)s->getSampleRate(), std::memory_order_relaxed);
            startIt = !paused;
        }
        if (startIt) start(s);
        return true;
    }

    // Route changes (headphones unplugged, Bluetooth switch, the endpoint
    // taken by another app) disconnect the stream; Oboe has already stopped
    // and closed it. Open a fresh one so audio comes back. Runs on Oboe's
    // error thread; the open itself happens outside the lock.
    void onErrorAfterClose(oboe::AudioStream* errored, oboe::Result error) override {
        LOGW("Audio stream closed (%s), reopening", oboe::convertToText(error));
        {
            std::lock_guard<std::mutex> lock(streamMutex);
            if (closing || stream.get() != errored) return;
            stream.reset();
        }
        std::shared_ptr<oboe::AudioStream> fresh = openStream();
        if (!fresh) return;
        bool install, startIt = false;
        {
            std::lock_guard<std::mutex> lock(streamMutex);
            install = !closing && !stream;   // shutdown() may have raced us
            if (install) {
                stream = fresh;
                sampleDt.store(1.0f / (float)fresh->getSampleRate(), std::memory_order_relaxed);
                startIt = !paused;
            }
        }
        if (!install) { fresh->close(); return; }
        if (startIt) start(fresh);
    }

    void close() {
        std::shared_ptr<oboe::AudioStream> s;
        {
            std::lock_guard<std::mutex> lock(streamMutex);
            closing = true;
            s = std::move(stream);
        }
        if (s) {
            s->requestStop();
            s->close();
        }
    }

    void setPaused(bool p) {
        std::shared_ptr<oboe::AudioStream> s;
        {
            std::lock_guard<std::mutex> lock(streamMutex);
            if (paused == p) return;   // GAINED_FOCUS right after open(): already running
            paused = p;
            s = stream;
        }
        if (!s) return;
        oboe::Result r = p ? s->requestPause() : s->requestStart();
        if (r != oboe::Result::OK)
            LOGW("Oboe %s: %s", p ? "requestPause" : "requestStart", oboe::convertToText(r));
    }
};

// ── AudioEngine public API ────────────────────────────────────────────────────

AudioEngine::AudioEngine()  : impl_(std::make_shared<Impl>()) {}
AudioEngine::~AudioEngine() { shutdown(); }

bool AudioEngine::init()     { return impl_->open(); }
void AudioEngine::shutdown() { impl_->close(); }
void AudioEngine::pause()    { impl_->setPaused(true); }
void AudioEngine::resume()   { impl_->setPaused(false); }

void AudioEngine::triggerPaddle()     { impl_->pending.fetch_or( 1u, std::memory_order_relaxed); }
void AudioEngine::triggerWall()       { impl_->pending.fetch_or( 2u, std::memory_order_relaxed); }
void AudioEngine::triggerBrickHit()   { impl_->pending.fetch_or( 4u, std::memory_order_relaxed); }
void AudioEngine::triggerBallLost()   { impl_->pending.fetch_or( 8u, std::memory_order_relaxed); }
void AudioEngine::triggerLevelClear() { impl_->pending.fetch_or(16u, std::memory_order_relaxed); }
void AudioEngine::triggerPowerUp()    { impl_->pending.fetch_or(32u, std::memory_order_relaxed); }
void AudioEngine::triggerExtraLife()  { impl_->pending.fetch_or(64u, std::memory_order_relaxed); }
void AudioEngine::triggerBrickBreak(float hz) {
    if (hz > 0.0f) impl_->breakPending.store(hz, std::memory_order_relaxed);
}
void AudioEngine::setMusicEnabled(bool e) { impl_->musicEnabled.store(e, std::memory_order_relaxed); }
