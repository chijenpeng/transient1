#pragma once

#include "Voicing.h"

#include <cmath>

namespace transient
{

inline double onePoleCoefficient (double milliseconds, double sampleRate) noexcept
{
    return 1.0 - std::exp (-1.0 / (milliseconds * 1.0e-3 * sampleRate));
}

/**
    Ripple-free envelope: the magnitude of the analytic signal made by an 8-section all-pass
    Hilbert pair (Niemitalo), per channel, in double. The channels are combined with max (the
    stereo link), then a 0.5 ms release smoother (instant rise) is applied.
*/
class AnalyticEnvelope
{
public:
    static constexpr int kMaxChannels = 2;

    void prepare (double detectorRate) noexcept
    {
        releaseCoefficient = onePoleCoefficient (voicing::kEnvelopeReleaseMs, detectorRate);
        for (int k = 0; k < voicing::kHilbertSections; ++k)
        {
            bI[k] = voicing::kHilbertA_I[k] * voicing::kHilbertA_I[k];
            bQ[k] = voicing::kHilbertA_Q[k] * voicing::kHilbertA_Q[k];
        }
        reset();
    }

    void reset() noexcept
    {
        for (auto& channel : state)
            channel = {};
        smoothed = 0.0;
    }

    /** One input sample per channel in, linear envelope out. */
    double process (const double* x, int numChannels) noexcept
    {
        double e = 0.0;
        for (int c = 0; c < numChannels; ++c)
        {
            auto& s = state[c];
            double v = s.previousInput;   // path I sees the input one sample late
            s.previousInput = x[c];
            for (int k = 0; k < voicing::kHilbertSections; ++k)
                v = section (s.i[k], bI[k], v);
            const double inPhase = v;
            v = x[c];
            for (int k = 0; k < voicing::kHilbertSections; ++k)
                v = section (s.q[k], bQ[k], v);
            const double magnitude = std::sqrt (inPhase * inPhase + v * v);
            if (magnitude > e)
                e = magnitude;
        }
        if (e > smoothed)
            smoothed = e;
        else
            smoothed += releaseCoefficient * (e - smoothed);
        return smoothed;
    }

private:
    struct Section
    {
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    };

    static double section (Section& s, double b, double x) noexcept
    {
        const double y = b * (x + s.y2) - s.x2;
        s.x2 = s.x1;
        s.x1 = x;
        s.y2 = s.y1;
        s.y1 = y;
        return y;
    }

    struct ChannelState
    {
        Section i[voicing::kHilbertSections], q[voicing::kHilbertSections];
        double previousInput = 0.0;
    };

    ChannelState state[kMaxChannels];
    double bI[voicing::kHilbertSections] {}, bQ[voicing::kHilbertSections] {};
    double releaseCoefficient = 1.0;
    double smoothed = 0.0;
};

}
