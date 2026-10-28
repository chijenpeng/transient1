#pragma once

#include "Envelope.h"
#include "Voicing.h"

#include <algorithm>
#include <cmath>

namespace transient
{

inline double smoothstep (double t) noexcept
{
    if (t <= 0.0)
        return 0.0;
    if (t >= 1.0)
        return 1.0;
    return t * t * (3.0 - 2.0 * t);
}

/** Level in dB with the detector floor applied. */
inline double levelDb (double linear) noexcept
{
    static const double floorLinear = std::pow (10.0, voicing::kFloorDb / 20.0);
    return 20.0 * std::log10 (linear > floorLinear ? linear : floorLinear);
}

/** One-pole in the dB domain with separate rise and fall coefficients. */
struct DbFollower
{
    double value = voicing::kFloorDb;

    void reset() noexcept { value = voicing::kFloorDb; }
    void step (double levelDb, double riseCoefficient, double fallCoefficient) noexcept
    {
        value += (levelDb > value ? riseCoefficient : fallCoefficient) * (levelDb - value);
    }
};

/**
    Attack: fast follower minus slow follower. Returns the depth 0..1 (dead zone, span, exponent).
    The slow follower never trails the fast one by more than the span. The dead zone adapts to the
    local fluctuation of the difference, the depth is soft-limited and peak-held, and the gain is weighted
    by a relative floor below the level's own peak memory (all constants in voicing.h).
*/
class AttackPath
{
public:
    void prepare (double rate) noexcept
    {
        fastRise = onePoleCoefficient (voicing::kAttackFastRiseMs, rate);
        slowRise = onePoleCoefficient (voicing::kAttackSlowRiseMs, rate);
        fall = onePoleCoefficient (voicing::kAttackFallMs, rate);
        fluctuation = onePoleCoefficient (voicing::kAttackFluctuationMs, rate);
        peakDecay = std::exp (-1.0 / (voicing::kAttackPeakHoldMs * 1.0e-3 * rate));
        softNormalise = 1.0 / std::tanh (voicing::kAttackSoftness);
        memoryHoldSamples = (int) std::lround (voicing::kRelativeHoldMs * 1.0e-3 * rate);
        memoryFall = voicing::kRelativeFallDbPerSec / rate;
        reset();
    }
    void reset() noexcept
    {
        fast.reset();
        slow.reset();
        meanPositive = 0.0;
        raw = 0.0;
        held = 0.0;
        memory = voicing::kFloorDb;
        memoryHold = 0;
    }

    /** @return the Attack shape: held depth ^ exponent times the relative-floor weight. */
    double process (double levelDb) noexcept
    {
        fast.step (levelDb, fastRise, fall);
        slow.step (levelDb, slowRise, fall);
        if (slow.value < fast.value - voicing::kAttackSpanDb)
            slow.value = fast.value - voicing::kAttackSpanDb;

        const double difference = fast.value - slow.value;
        const double clipped = std::clamp (difference, 0.0, voicing::kAttackFluctuationClipDb);
        meanPositive += fluctuation * (clipped - meanPositive);
        const double deadZone = std::max (voicing::kAttackDeadZoneDb,
                                          voicing::kAttackFluctuationFactor * meanPositive);
        const double span = std::max (0.0, (difference - deadZone)
                                              / (voicing::kAttackSpanDb - voicing::kAttackDeadZoneDb));
        raw = std::min (1.0, std::tanh (voicing::kAttackSoftness * span) * softNormalise);
        held = raw > held ? raw : held * peakDecay;

        // Relative floor: the peak memory of the level rises at once, holds, then falls.
        if (levelDb > memory)
        {
            memory = levelDb;
            memoryHold = memoryHoldSamples;
        }
        else if (memoryHold > 0)
        {
            --memoryHold;
        }
        else
        {
            memory -= memoryFall;
        }
        const double relative = smoothstep ((fast.value - (memory - voicing::kRelativeFloorDb))
                                            / voicing::kRelativeFloorWidthDb);

        static_assert (voicing::kAttackExponent == 1.5, "held * sqrt (held) below is the exponent");
        return held * std::sqrt (held) * relative;
    }

    /** The depth before the exponent, from this sample (restarts the Sustain body ramp). */
    double rawDepth() const noexcept { return raw; }
    /** The peak-held depth before the exponent (suppresses the Sustain body term). */
    double heldDepth() const noexcept { return held; }
    /** The fast follower in dB: the level this section reasons about (floor fade input). */
    double level() const noexcept { return fast.value; }

private:
    DbFollower fast, slow;
    double fastRise = 1.0, slowRise = 1.0, fall = 1.0;
    double fluctuation = 1.0;
    double meanPositive = 0.0;
    double peakDecay = 1.0;
    double softNormalise = 1.0;
    double raw = 0.0, held = 0.0;
    double memory = voicing::kFloorDb;
    double memoryFall = 0.0;
    int memoryHoldSamples = 0, memoryHold = 0;
};

/**
    Sustain: slow-release follower minus fast-release follower. Returns the depth 0..1 including
    the body term for held material.
*/
class SustainPath
{
public:
    void prepare (double rate) noexcept
    {
        rise = onePoleCoefficient (voicing::kSustainRiseMs, rate);
        fastFall = onePoleCoefficient (voicing::kSustainFastFallMs, rate);
        slowFall = onePoleCoefficient (voicing::kSustainSlowFallMs, rate);
        bodyStep = 1.0 / (voicing::kBodyRampMs * 1.0e-3 * rate);
        reset();
    }
    void reset() noexcept { fast.reset(); slow.reset(); body = 0.0; }

    /**
        @param attackRaw the Attack path's raw depth (before its exponent): above the restart depth it restarts the body ramp.
        @param attackHeld the Attack path's peak-held depth: the body term is suppressed in proportion to it.
    */
    double process (double levelDb, double attackRaw, double attackHeld) noexcept
    {
        bool resetFired = false;
        if (levelDb > fast.value + voicing::kSustainResetDb)
        {
            fast.value = levelDb;
            slow.value = levelDb;
            resetFired = true;
        }
        fast.step (levelDb, rise, fastFall);
        slow.step (levelDb, rise, slowFall);
        double depth = std::clamp ((slow.value - fast.value - voicing::kSustainDeadZoneDb)
                                       / (voicing::kSustainSpanDb - voicing::kSustainDeadZoneDb), 0.0, 1.0);
        if (resetFired || attackRaw > voicing::kBodyRestartDepth)
            body = 0.0;
        else
            body = std::min (1.0, body + bodyStep);
        depth = std::min (1.0, depth + voicing::kBodyGain * smoothstep (body) * (1.0 - attackHeld));
        return depth;
    }

    double level() const noexcept { return fast.value; }

private:
    DbFollower fast, slow;
    double rise = 1.0, fastFall = 1.0, slowFall = 1.0;
    double bodyStep = 1.0, body = 0.0;
};

/** Floor fade: 0 below the low level, 1 above the high level, smoothstep between. */
inline double floorFade (double followerDb) noexcept
{
    return smoothstep ((followerDb - voicing::kFloorFadeLowDb) / (voicing::kFloorFadeHighDb - voicing::kFloorFadeLowDb));
}

}
