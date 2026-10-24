#include "TransientShaper.h"

#include <algorithm>
#include <cmath>

namespace transient
{

void TransientShaper::Boxcar::push (double g) noexcept
{
    const double old = ring[(size_t) position];
    sum += g - old;
    nonZero += (g != 0.0) - (old != 0.0);
    ring[(size_t) position] = g;
    if (++position == (int) ring.size())
    {
        position = 0;
        // Exact re-sum once per revolution, so rounding never accumulates.
        double exact = 0.0;
        for (const double v : ring)
            exact += v;
        sum = exact;
    }
}

void TransientShaper::prepare (double newSampleRate, int, int numChannels)
{
    sampleRate = newSampleRate;
    channelCount = std::clamp (numChannels, 1, kMaxChannels);
    decimation = sampleRate > voicing::kMaxDetectorRate ? 2 : 1;
    detectorSampleRate = sampleRate / decimation;
    lookAhead = std::max (1, (int) std::lround (voicing::kLookAheadMs * 1.0e-3 * sampleRate));
    boxScale = 1.0 / (double) (lookAhead + 1);

    envelope.prepare (detectorSampleRate);
    attackPath.prepare (detectorSampleRate);
    sustainPath.prepare (detectorSampleRate);
    attackKnob.prepare (sampleRate, voicing::kKnobSmoothMs);
    sustainKnob.prepare (sampleRate, voicing::kKnobSmoothMs);
    for (auto& d : delay)
        d.assign ((size_t) lookAhead + 1, 0.0f);
    attackBox.resize (lookAhead + 1);
    sustainBox.resize (lookAhead + 1);
    bypassStep = 1.0 / std::max (1.0, voicing::kBypassMs * 1.0e-3 * sampleRate);
    telemetryInterval = std::max (1, (int) std::lround (sampleRate / voicing::kTelemetryHz));
    reset();
}

void TransientShaper::reset() noexcept
{
    envelope.reset();
    attackPath.reset();
    sustainPath.reset();
    attackKnob.snap();
    sustainKnob.snap();
    attackShape = sustainShape = 0.0;
    for (auto& s : pairSum)
        s = 0.0;
    pairPhase = 0;
    for (auto& d : delay)
        std::fill (d.begin(), d.end(), 0.0f);
    delayPos = 0;
    attackBox.resize (lookAhead + 1);
    sustainBox.resize (lookAhead + 1);
    bypassMix = bypassTarget ? 0.0 : 1.0;
    telemetryCounter = 0;
    framePeak = 0.0f;
    frameGain = frameAttack = frameSustain = 0.0;
}

void TransientShaper::setParameters (double attackDb, double sustainDb) noexcept
{
    attackKnob.setTarget (attackDb);
    sustainKnob.setTarget (sustainDb);
}

void TransientShaper::detectorStep (double e) noexcept
{
    const double level = levelDb (e);
    const double attackDepth = attackPath.process (level);
    const double sustainDepth = sustainPath.process (level, attackPath.depthBeforeExponent());
    attackShape = attackDepth * floorFade (attackPath.level());
    sustainShape = sustainDepth * floorFade (sustainPath.level());
}

void TransientShaper::process (float* const* channels, int numChannels, int numSamples, bool bypassed,
                                 double* gainDbTrace) noexcept
{
    const int nch = std::min (numChannels, channelCount);
    bypassTarget = bypassed;
    const int ringSize = lookAhead + 1;
    constexpr double kDbToNeper = 0.11512925464970229;   // ln(10) / 20

    for (int i = 0; i < numSamples; ++i)
    {
        // 1. Delay line: write the new sample, read the one written lookAhead samples ago.
        double x[kMaxChannels] {};
        float delayed[kMaxChannels] {};
        const int readPos = delayPos + 1 == ringSize ? 0 : delayPos + 1;
        for (int c = 0; c < nch; ++c)
        {
            const float in = channels[c][i];
            x[c] = (double) in;
            delay[c][(size_t) delayPos] = in;
            delayed[c] = delay[c][(size_t) readPos];
        }
        delayPos = readPos;

        // 2. Detector, at the host rate or on pair means at half of it.
        if (decimation == 1)
        {
            detectorStep (envelope.process (x, nch));
        }
        else
        {
            for (int c = 0; c < nch; ++c)
                pairSum[c] += x[c];
            if (++pairPhase == decimation)
            {
                pairPhase = 0;
                double mean[kMaxChannels] {};
                for (int c = 0; c < nch; ++c)
                {
                    mean[c] = pairSum[c] / (double) decimation;
                    pairSum[c] = 0.0;
                }
                detectorStep (envelope.process (mean, nch));
            }
        }

        // 3. Raw gains, look-ahead means, bypass fade.
        attackBox.push (attackKnob.next() * attackShape);
        sustainBox.push (sustainKnob.next() * sustainShape);
        if (bypassTarget)
            bypassMix = std::max (0.0, bypassMix - bypassStep);
        else
            bypassMix = std::min (1.0, bypassMix + bypassStep);
        const double attackDb = attackBox.mean (boxScale) * bypassMix;
        const double sustainDb = sustainBox.mean (boxScale) * bypassMix;
        const double gainDb = attackDb + sustainDb;
        const float gain = gainDb == 0.0 ? 1.0f : (float) std::exp (gainDb * kDbToNeper);
        if (gainDbTrace != nullptr)
            gainDbTrace[i] = gainDb;

        // 4. Output and telemetry.
        float peak = 0.0f;
        for (int c = 0; c < nch; ++c)
        {
            channels[c][i] = gain == 1.0f ? delayed[c] : delayed[c] * gain;
            peak = std::max (peak, std::abs (delayed[c]));
        }
        framePeak = std::max (framePeak, peak);
        if (std::abs (gainDb) >= std::abs (frameGain))
        {
            frameGain = gainDb;
            frameAttack = attackDb;
            frameSustain = sustainDb;
        }
        if (++telemetryCounter >= telemetryInterval)
        {
            telemetryCounter = 0;
            TelemetryFrame frame;
            frame.inputPeakDb = framePeak > 1.0e-6f ? 20.0f * std::log10 (framePeak) : -120.0f;
            frame.gainDb = (float) frameGain;
            frame.attackGainDb = (float) frameAttack;
            frame.sustainGainDb = (float) frameSustain;
            telemetryFifo.push (frame);
            framePeak = 0.0f;
            frameGain = frameAttack = frameSustain = 0.0;
        }
    }
}

}
