# Transient1 DSP

Source: `Source/dsp/` (JUCE-free). Constants: `Source/dsp/Voicing.h`. Provenance labels: **Source** (published
coefficients) and **Custom** (chosen for this plug-in). The numbers in the Measurements section come from
`tests/transient1_test.cpp`, run with and without the optional recordings.

## Signal flow

```
input --> delay line (L samples) ----------------------------------------> x gain --> output
   |                                                                          ^
   +--> analytic envelope --> dB --> Attack path  --> depth_A * fade_A --+    |
                                 \-> Sustain path --> depth_S * fade_S --+--> g = A*sA + S*sS (dB)
                                                                              --> mean of last L+1 raw g (boxcar) --+
```

Audio is `float`; the detector, followers and boxcar are `double`. Stereo is linked: the envelope is the max
over the channels, so there is one gain for both. All numbers below are for 48 kHz; times are in seconds
and scale with the rate.

## Envelope (`Envelope.h`)

- **Source:** Niemitalo's 8-section all-pass Hilbert pair, 4 sections per path, coefficients a_k squared
  (b = a_k^2) in sections `(b - z^-2) / (1 - b z^-2)`. Path I (0.6923878, 0.9360654, 0.9882295, 0.9987488) sees the input one sample late; path Q
  (0.4021921, 0.8561711, 0.9722910, 0.9952885). Envelope = sqrt(I^2 + Q^2), 90 degrees +-0.7 from 20 Hz to 23 kHz.
- **Custom:** channels combined by max; a 0.5 ms release one-pole in linear amplitude (instant rise).
- Level in dB with a floor of -130 dBFS (Custom); all followers start at the floor.

## Attack path (`Followers.h`)

dB-domain asymmetric one-poles. Fast follower Ef: rise 0.3 ms, fall 5 ms. Slow follower Es: rise 30 ms, fall 5 ms. Es is kept
at or above Ef - 12 dB (**Custom**). Times and structure **Custom**.

Dead zone (**Custom**, adaptive): `DZ = max(3, 1.25 F)`, where `F` is a 100 ms one-pole mean of `clamp(Ef - Es, 0, 4 dB)`. The 4 dB clip keeps
a single hit from inflating `F`. Silence and clean decays keep the 3 dB dead zone; dense material (a room wash, the noise under a decaying tail)
raises `F`, so only larger rises pass.
Depth `dA = clamp((Ef - Es - DZ) / 9, 0, 1)` (the 9 dB span above the dead zone is fixed, so full scale is `DZ + 9`).

Peak hold (**Custom**): the depth is held, `dAh = max(dA, dAh * exp(-1 / (4 ms * fs)))` per detector sample, so it rises at once and falls over
about 4 ms. The hold keeps a second bump (a dip between a click and its body, then the body rising again) inside the first hit's gain.
Shape `sA = dAh^1.5` (**Custom**).

Relative floor (**Custom**): `wR = smoothstep((Ef - (M - 36)) / 6)`, where `M` is the peak memory of the detector level: it rises at once, holds
1 s, then falls 6 dB/s. The Attack gain is weighted by `wR`, so tails and noise 36 dB and more below the track's own peaks are not taken for hits.
It is relative to the material's own level, so level independence holds.

## Sustain path

Both followers rise in 6.6 ms (**Custom**); the fast one Ef2 falls in 25 ms and the slow one Es2 in 600 ms (**Custom**).
If the level exceeds Ef2 + 8 dB the onset reset sets both to the level (**Custom**).
`dS = clamp((Es2 - Ef2 - 3) / 27, 0, 1)` (dead zone 3, full at 30 dB, **Custom**).
Body term (**Custom**): a counter ramps 0 -> 1 over 30 ms and restarts at 0 when the reset fires or the Attack depth (before its exponent) exceeds 0.9;
`sS = min(1, dS + 0.12 * smoothstep(body) * (1 - dAh))`, with `dAh` the held Attack depth before its exponent. Held, steady material (no onsets) therefore carries
a constant 0.12 of the Sustain knob. The `(1 - dAh)` factor (**Custom**) suppresses the body term in proportion while the Attack path sees an onset,
without resetting the ramp: on continuous material such as a room mic the hits would otherwise sit in the held gain and Sustain -24 would cut them
almost as much as the wash. The 0.9 restart uses the raw depth before the hold, and the 8 dB reset is unchanged.

## Gain

`g_dB = Attack * sA * wR * wA + Sustain * sS * wS`, where `wR` is the relative floor and `wA = smoothstep((Ef - (-84)) / 12)` and `wS` the same with Ef2: each section fades to 0 dB
between -84 and -72 dB of its follower level (**Custom**; the silence floor, so noise and digital silence are never lifted;
very quiet tails are therefore level dependent by design). The knob values are smoothed (linear, 20 ms, **Custom**) before the multiplication and
land exactly on the target. The mean of the last L+1 raw gains (look-ahead boxcar) is applied to the input delayed by L samples,
`L = round(1 ms * fs)` (**Custom**). The boxcar sums are kept exact (a count of non-zero entries and an exact re-sum once per revolution),
so with both knobs at zero the gain is exactly 1.0f and the output is the delayed input bit for bit.
Output can exceed 0 dBFS; there is no limiter.

The knobs are the largest gain each section can apply; the gain stays inside them (tests 2, 6).

## Sample rates

At 44.1-96 kHz the detector runs at the host rate. The 8-coefficient Hilbert pair ripples at low frequencies when the rate is high (30 Hz envelope ripple
0.1/0.8/1.05 dB p-p at 48/88.2/96 kHz, but 4.8 dB at 192 kHz, THD of a 20 Hz sine -25 dB), so **above 100 kHz (176.4 and 192 kHz) the detector
runs at half the rate**: the input is averaged in pairs (a zero-latency 2-tap low-pass), the envelope and every follower coefficient use fs/2,
and the shape values `sA*wA`, `sS*wS` are held for two samples before the knob multiplication, boxcar and delay run at the full rate.
Latency is unchanged (1 ms). Held 20 and 40 Hz sines at 176.4 / 192 kHz with Sustain +-24 are tested to THD <= -60 dB (test 4).

## Latency

1 ms = `round(0.001 fs)` samples (44 / 48 / 88 / 96 / 176 / 192), constant for every knob value, reported with `setLatencySamples` in `prepareToPlay`.
Host bypass (`processBlockBypassed`) keeps the delay line and detector running and fades the gain (in dB) to zero over 10 ms.

## Parameters

| ID | Range | Default | Notes |
|---|---|---|---|
| `attack` | -24..+24 dB, step 0.1 | 0 | `NormalisableRange` with symmetric skew 0.5 (**Custom**): +-6 dB at half of each side's travel. Text `+6.0 dB`. |
| `sustain` | same | 0 | same |

State: the APVTS tree plus a `version` property (1). A state with a newer version loads the defaults. New parameters are appended, never inserted.

## Telemetry interface (for the editor)

Declared in `Source/dsp/TransientShaper.h`, namespace `transient`:

```cpp
struct TelemetryFrame {
    float inputPeakDb  = -120.0f;  // peak of the delayed input over the frame, dBFS, floor -120
    float gainDb       = 0.0f;     // applied gain, dB: the largest-magnitude value in the frame
    float attackGainDb = 0.0f;     // Attack section's part of gainDb
    float sustainGainDb = 0.0f;    // Sustain section's part; gainDb = attackGainDb + sustainGainDb
};
class TelemetryFifo { bool push (const TelemetryFrame&) noexcept; bool pop (TelemetryFrame&) noexcept; }; // SPSC, 2048 frames, drops when full
```

The audio thread pushes one frame every `round(fs / 400)` samples (about 400 per second, i.e. 5.1 s of buffer). All values are for the audio the listener is hearing now
(input delayed by the latency, gain applied to it). On the processor: `bool Transient1Processor::popTelemetry (transient::TelemetryFrame&) noexcept;`
call it from the message thread (timer) until it returns false. Frames are produced only while audio runs; bypassed, the gain values fade to zero.
The range for a bipolar display is +-48 dB at most (both sections at their knobs); typical single sections stay within +-24.

## Tests

`tests/transient1_test.cpp` (tests 1-12, 14, 15); the README has the commands. Test 13 is a comparison of the C++ with an offline C model of the same algorithm
that is not part of this repository. The checks on recordings (the drum mix in 2 and 3, the real kick in 5, the bass DI in 15) need the optional
recordings (`TRANSIENT1_TEST_AUDIO`) and are skipped without them. Test 5b checks the onset rule: a steady wash gives no Attack, hits on a wash
are kept, and a kick with a click and a body gives one Attack peak per hit.

## Measurements

- Latency: 1 ms (44 / 48 / 88 / 96 / 176 / 192 samples), identical at neutral and at Attack +24 / Sustain -24, equal to the reported value.
- Neutral: with both knobs at 0 the output equals the delayed input bit for bit (72 combinations of channels, rates 44.1 / 48 / 96 kHz and block sizes 1 to 4096).
- Honest dB: an onset from silence reaches +-23.91 dB of its 24 dB Attack knob, a decay reaches exactly +-24.0000 dB of Sustain; on noise the Attack gain stays inside [0, 24] and the Sustain gain inside +-2.88 dB.
- Onset detection: a steady wash at -20 dBFS with Attack +24 gives an Attack gain above 1 dB for 0.000 % of its duration. Hits 12 dB above a wash at -30 dBFS, one every 200 ms, keep 11.8 to 11.9 dB of peak gain at Attack +12. A kick followed 68 ms later by a click and a body (the body peaking 8 ms after the click) gives one Attack peak for each hit.
- Level independence: on a drum recording scaled by +6, -6, -12, -24 and -36 dB the applied gain stays within 0.012 dB of the unscaled one over the samples compared. Below about -72 dBFS the sections fade out by design.
- Held material: Attack alone applies 0.000 dB to steady sines from 40 Hz to 5 kHz at -12, -40 and -70 dBFS; Sustain +-24 applies +-2.88 dB (ripple 0.0000 dB); THD of held 40 and 100 Hz sines at Sustain +-24 is -160.7 dB (20 and 40 Hz at 176.4 and 192 kHz: -169.1 dB).
- Fast repeats: with Attack +24 a synthetic kick train keeps 100 % of an isolated hit's gain at 8 Hz and 75 % at 16 Hz in steady state. A real kick recording (the hit and the 150 ms after it) keeps 99 % at 8 Hz and 92 % at 16 Hz.
- Hits on a wash: with 6 dB hits over a steady wash and Sustain -24, the hits' peak gain is -0.93 dB against -4.37 dB on the wash; with Sustain +24 the hits keep up with the wash (3.96 dB against 4.37 dB).
- Smoothing: a 1 Hz +-24 dB sweep of both knobs moves the gain at most 0.00234 dB per sample; a jump from -24 to +24 at most 0.006 dB per sample. After an Attack cut the energy above 5 kHz is 33.8 dB below that of an unsmoothed, zero-latency gain.
- Stereo: one gain for both channels (worst relative difference 1e-7); a steady right channel is not affected by hits on the left.
- Realtime: no allocations in `process`, `processBlock` or `processBlockBypassed`; 10 s of stereo at 48 kHz runs about 430 to 440 times faster than real time in the test (best of 5 runs).
