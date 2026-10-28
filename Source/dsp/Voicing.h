#pragma once

/**
    Every constant that shapes the transient shaper, in one place.

    Provenance labels (see docs/dsp.md):
      Source   - published coefficients (the Niemitalo all-pass Hilbert pair).
      Custom   - chosen for this plug-in.

    Times are in milliseconds, levels in dB.
*/
namespace transient::voicing
{
    //==========================================================================
    // Analytic-signal envelope

    /** Source: Olli Niemitalo, "Hilbert transformer" (8 all-pass sections, two paths of 4).
        Coefficients a_k of the first-order-in-z^-2 sections (b - z^-2) / (1 - b z^-2) with b = a_k^2.
        Path I (coefficients 0.6923878 ...) is delayed by one sample. 90 degrees +/- 0.7 from
        20 Hz to 23 kHz at 48 kHz; valid up to 96 kHz (see kMaxDetectorRate). */
    inline constexpr int kHilbertSections = 4;
    inline constexpr double kHilbertA_I[kHilbertSections] = { 0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737 };
    inline constexpr double kHilbertA_Q[kHilbertSections] = { 0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278 };

    /** Custom: the envelope falls through a 0.5 ms one-pole (linear amplitude, instant rise). */
    inline constexpr double kEnvelopeReleaseMs = 0.5;

    /** Custom: the lowest level the detector knows, dBFS. Followers start here. */
    inline constexpr double kFloorDb = -130.0;

    /** Custom: above this host rate the detector runs on a 2-sample mean at half the rate,
        because the 8-section Hilbert pair ripples at low frequencies when fs is large. */
    inline constexpr double kMaxDetectorRate = 100000.0;

    //==========================================================================
    // Attack path: fast follower minus slow follower, in dB

    /** Custom: the onset is "how far the level has just jumped above its recent past".
        Fast follower: instant-ish rise (0.3 ms) and 5 ms fall; slow follower: 30 ms rise, 5 ms fall. */
    inline constexpr double kAttackFastRiseMs = 0.3;
    inline constexpr double kAttackSlowRiseMs = 30.0;
    inline constexpr double kAttackFallMs = 5.0;
    /** Custom: the slow follower never trails the fast one by more than this (Es >= Ef - 12 dB). */
    inline constexpr double kAttackSpanDb = 12.0;
    /** Custom: the smallest dead zone. Differences below the dead zone do nothing; the depth is 0..1 between the
        dead zone and the span (the span above the dead zone, 9 dB, is fixed). */
    inline constexpr double kAttackDeadZoneDb = 3.0;
    /** Custom: shape of the 0..1 depth before it multiplies the knob. */
    inline constexpr double kAttackExponent = 1.5;

    /** Custom: the dead zone adapts to the local fluctuation of the Attack difference, so dense material (room wash,
        the noise under a decaying tail) is not taken for onsets. The fluctuation is a 100 ms one-pole mean of the
        positive difference, clipped at 4 dB so that a single hit cannot inflate it. The dead zone is
        max(kAttackDeadZoneDb, 1.25 x fluctuation). Silence and clean decays keep the 3 dB dead zone. */
    inline constexpr double kAttackFluctuationMs = 100.0;
    inline constexpr double kAttackFluctuationClipDb = 4.0;
    inline constexpr double kAttackFluctuationFactor = 1.25;

    /** Custom: the Attack depth is peak-held. It rises at once and falls with a 4 ms time constant, so a dip of a few
        milliseconds between a click and its body does not produce a second onset; the Attack gain decays smoothly
        after a hit. The Sustain body suppression uses the held depth; its 0.9 restart uses the raw depth. */
    inline constexpr double kAttackPeakHoldMs = 4.0;

    /** Custom: relative floor. Attack gain is weighted by the detector level relative to its own peak memory:
        the memory rises at once, holds 1 s, then falls 6 dB/s. The weight is 0 at 36 dB or more below the memory
        and 1 from 30 dB below it up (smoothstep over 6 dB), so tails and noise far below the track's own peaks
        are not taken for hits. Level independent: it follows the material's own level. */
    inline constexpr double kRelativeFloorDb = 36.0;
    inline constexpr double kRelativeFloorWidthDb = 6.0;
    inline constexpr double kRelativeHoldMs = 1000.0;
    inline constexpr double kRelativeFallDbPerSec = 6.0;

    //==========================================================================
    // Sustain path: slow-release follower minus fast-release follower, in dB

    /** Custom: a decay is "how far the level has fallen below its recent peak".
        Both followers rise in 6.6 ms; the fast one falls in 25 ms, the slow one in 600 ms. */
    inline constexpr double kSustainRiseMs = 6.6;
    inline constexpr double kSustainFastFallMs = 25.0;
    inline constexpr double kSustainSlowFallMs = 600.0;
    /** Custom: dead zone 3 dB, full depth at 30 dB below the recent peak, linear. */
    inline constexpr double kSustainDeadZoneDb = 3.0;
    inline constexpr double kSustainSpanDb = 30.0;
    /** Custom: an onset more than 8 dB above the fast Sustain follower snaps both followers to it. */
    inline constexpr double kSustainResetDb = 8.0;

    /** Custom: held material (no decay, no onset) carries this fraction of the Sustain knob,
        reached smoothly 30 ms after the last onset. It restarts when the Attack depth exceeds
        0.9 (raw Attack depth) or the reset fires, and is scaled by (1 - held Attack depth) so that
        hits on continuous material (a room mic) are not cut along with the wash. Makes a bass or
        palm-muted DI respond to Sustain. */
    inline constexpr double kBodyGain = 0.12;
    inline constexpr double kBodyRampMs = 30.0;
    inline constexpr double kBodyRestartDepth = 0.9;

    //==========================================================================
    // Silence floor and output stage

    /** Custom: each section fades to 0 dB gain between these follower levels (smoothstep),
        so room noise and the digital silence between hits are never lifted. */
    inline constexpr double kFloorFadeLowDb = -84.0;
    inline constexpr double kFloorFadeHighDb = -72.0;

    /** Custom: look-ahead. The audio is delayed this long and the gain is the mean of the raw
        gains over the same span, so it is fully in place when the transient arrives. */
    inline constexpr double kLookAheadMs = 1.0;

    /** Custom: knob changes ramp linearly over this long, then land exactly on the target. */
    inline constexpr double kKnobSmoothMs = 20.0;
    /** Custom: bypass crossfade (gain in dB scaled 1 -> 0). */
    inline constexpr double kBypassMs = 10.0;

    //==========================================================================
    // Controls

    /** Custom: the knobs are the largest gain in dB the section can apply. */
    inline constexpr float kKnobRangeDb = 24.0f;
    inline constexpr float kKnobStepDb = 0.1f;
    /** Custom: symmetric skew about 0 dB, so +-6 dB sits at half of each side's travel. */
    inline constexpr float kKnobSkew = 0.5f;

    //==========================================================================
    // Telemetry

    inline constexpr double kTelemetryHz = 400.0;
}
