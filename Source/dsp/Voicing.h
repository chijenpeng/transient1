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
    /** Custom: differences below this do nothing; the difference is 0..1 between dead zone and span. */
    inline constexpr double kAttackDeadZoneDb = 3.0;
    /** Custom: shape of the 0..1 depth before it multiplies the knob. */
    inline constexpr double kAttackExponent = 1.5;

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
        0.9 or the reset fires, and is scaled by (1 - Attack depth before its exponent) so that
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
