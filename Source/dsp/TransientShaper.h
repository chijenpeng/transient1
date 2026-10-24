#pragma once

#include "Envelope.h"
#include "Followers.h"
#include "Voicing.h"

#include <array>
#include <atomic>
#include <vector>

namespace transient
{

/** One display point, about 400 per second. All values are what the (delayed) audio met. */
struct TelemetryFrame
{
    float inputPeakDb = -120.0f;   // peak of the delayed input over the frame, dBFS (floor -120)
    float gainDb = 0.0f;           // applied gain: the largest-magnitude value in the frame, dB
    float attackGainDb = 0.0f;     // the Attack section's share of gainDb (gainDb = attack + sustain)
    float sustainGainDb = 0.0f;    // the Sustain section's share of gainDb
};

/** Single-producer single-consumer ring, audio thread -> UI. Drops frames when full. */
class TelemetryFifo
{
public:
    bool push (const TelemetryFrame& f) noexcept
    {
        const int w = writeIndex.load (std::memory_order_relaxed);
        const int next = (w + 1) % kSize;
        if (next == readIndex.load (std::memory_order_acquire))
            return false;
        frames[(size_t) w] = f;
        writeIndex.store (next, std::memory_order_release);
        return true;
    }

    bool pop (TelemetryFrame& f) noexcept
    {
        const int r = readIndex.load (std::memory_order_relaxed);
        if (r == writeIndex.load (std::memory_order_acquire))
            return false;
        f = frames[(size_t) r];
        readIndex.store ((r + 1) % kSize, std::memory_order_release);
        return true;
    }

private:
    static constexpr int kSize = 2048;
    std::array<TelemetryFrame, kSize> frames;
    std::atomic<int> writeIndex { 0 }, readIndex { 0 };
};

/** Linear ramp to a target that lands exactly on it. */
class KnobSmoother
{
public:
    void prepare (double sampleRate, double milliseconds) noexcept
    {
        rampSamples = std::max (1, (int) std::lround (milliseconds * 1.0e-3 * sampleRate));
    }
    void setTarget (double t) noexcept
    {
        if (! (t < target) && ! (t > target))
            return;
        target = t;
        remaining = rampSamples;
        step = (target - current) / (double) rampSamples;
    }
    void snap() noexcept { current = target; remaining = 0; }
    double next() noexcept
    {
        if (remaining > 0)
        {
            if (--remaining == 0)
                current = target;
            else
                current += step;
        }
        return current;
    }
    double targetValue() const noexcept { return target; }

private:
    double current = 0.0, target = 0.0, step = 0.0;
    int remaining = 0, rampSamples = 1;
};

/**
    Transient shaper: Attack and Sustain, each the largest gain (dB) its section can apply.

    envelope -> dB -> Attack path / Sustain path (depth 0..1) -> gain = knob x depth x floor fade
    -> mean over the look-ahead span -> applied to the input delayed by the same span.
    Stereo is linked (one detector, one gain). JUCE-free; audio is float, detector state double.
*/
class TransientShaper
{
public:
    static constexpr int kMaxChannels = AnalyticEnvelope::kMaxChannels;

    /** Not real-time: allocates for the host rate (any rate up to 192 kHz and beyond). */
    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    /** Clears all state and lands the smoothed knobs on their targets. */
    void reset() noexcept;

    /** Targets in dB; the engine ramps to them in 20 ms and lands exactly on them. */
    void setParameters (double attackDb, double sustainDb) noexcept;

    /** In place. When bypassed the audio is only delayed (the gain fades out over 10 ms) and the
        detector keeps running. gainDbTrace, when not null, receives the applied gain in dB per
        sample (for tools and tests). */
    void process (float* const* channels, int numChannels, int numSamples, bool bypassed = false,
                  double* gainDbTrace = nullptr) noexcept;

    int latencySamples() const noexcept { return lookAhead; }
    /** Rate the detector and followers run at (half the host rate above 100 kHz). */
    double detectorRate() const noexcept { return detectorSampleRate; }

    TelemetryFifo& telemetry() noexcept { return telemetryFifo; }

private:
    void detectorStep (double e) noexcept;

    double sampleRate = 48000.0, detectorSampleRate = 48000.0;
    int decimation = 1, lookAhead = 48, channelCount = 2;

    AnalyticEnvelope envelope;
    AttackPath attackPath;
    SustainPath sustainPath;
    KnobSmoother attackKnob, sustainKnob;
    double attackShape = 0.0, sustainShape = 0.0;   // depth x floor fade, held between detector steps
    double pairSum[kMaxChannels] {};
    int pairPhase = 0;

    // Look-ahead: delay line for the audio and boxcars over the raw gains of both sections.
    std::vector<float> delay[kMaxChannels];
    int delayPos = 0;
    struct Boxcar
    {
        std::vector<double> ring;
        double sum = 0.0;
        int position = 0, nonZero = 0;
        void resize (int n) { ring.assign ((size_t) n, 0.0); sum = 0.0; position = 0; nonZero = 0; }
        void push (double g) noexcept;
        double mean (double scale) const noexcept { return nonZero == 0 ? 0.0 : sum * scale; }
    } attackBox, sustainBox;
    double boxScale = 1.0;

    double bypassMix = 1.0, bypassStep = 0.0;
    bool bypassTarget = false;

    TelemetryFifo telemetryFifo;
    int telemetryInterval = 120, telemetryCounter = 0;
    float framePeak = 0.0f;
    double frameGain = 0.0, frameAttack = 0.0, frameSustain = 0.0;
};

}
