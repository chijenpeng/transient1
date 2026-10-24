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
    The slow follower never trails the fast one by more than the span.
*/
class AttackPath
{
public:
    void prepare (double rate) noexcept
    {
        fastRise = onePoleCoefficient (voicing::kAttackFastRiseMs, rate);
        slowRise = onePoleCoefficient (voicing::kAttackSlowRiseMs, rate);
        fall = onePoleCoefficient (voicing::kAttackFallMs, rate);
        reset();
    }
    void reset() noexcept { fast.reset(); slow.reset(); }

    /** @return the depth after the exponent; depthBeforeExponent() holds it before. */
    double process (double levelDb) noexcept
    {
        fast.step (levelDb, fastRise, fall);
        slow.step (levelDb, slowRise, fall);
        if (slow.value < fast.value - voicing::kAttackSpanDb)
            slow.value = fast.value - voicing::kAttackSpanDb;
        depth = std::clamp ((fast.value - slow.value - voicing::kAttackDeadZoneDb)
                                / (voicing::kAttackSpanDb - voicing::kAttackDeadZoneDb), 0.0, 1.0);
        static_assert (voicing::kAttackExponent == 1.5, "depth * sqrt (depth) below is the exponent");
        return depth * std::sqrt (depth);
    }

    double depthBeforeExponent() const noexcept { return depth; }
    /** The fast follower in dB: the level this section reasons about (floor fade input). */
    double level() const noexcept { return fast.value; }

private:
    DbFollower fast, slow;
    double fastRise = 1.0, slowRise = 1.0, fall = 1.0;
    double depth = 0.0;
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

    /** @param attackDepth the Attack path's depth before its exponent (restarts the body ramp and suppresses the body term in proportion). */
    double process (double levelDb, double attackDepth) noexcept
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
        if (resetFired || attackDepth > voicing::kBodyRestartDepth)
            body = 0.0;
        else
            body = std::min (1.0, body + bodyStep);
        depth = std::min (1.0, depth + voicing::kBodyGain * smoothstep (body) * (1.0 - attackDepth));
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
