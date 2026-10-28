# Transient1 release notes

## v0.1.1

Attack no longer reacts to the steady wash of a room or overhead track, or to the low tail after a kick hit. Each hit now gives one
smooth Attack bump, also on fast kick patterns such as 1/32 notes at 110 BPM. Sustain, level independence and latency are unchanged.
Parameters, ranges, state and plug-in identity are unchanged, so sessions load as before.

- Onset detection: the dead zone follows the recent level fluctuation, onsets far below the track's own recent peak are ignored, and the
  Attack depth is held for about 4 ms so a dip between a click and its body does not give a second bump.
- Limit: Attack fades out for onsets 30 to 36 dB below the track's own recent peak level (1 s memory), so a very soft hit right after a
  loud one is not shaped.
- Checked before release: CTest 1/1 (transient1_test: 50 checks pass, 3 skipped without the recordings, 0 failures; with the
  recordings in place locally: 60 pass, 0 skipped); editor snapshot rendered with `--synthetic`; AU and VST3 slices arm64 + x86_64;
  ad-hoc signatures, ZIP integrity and SHA-256 verified by the packaging script.
- Windows x64: VST3, built natively with MSVC on windows-2022; CTest 1/1 passed ([run 40639036665](https://github.com/chijenpeng/transient1/actions/runs/40639036665)); the package is a PE32+ x86-64 VST3 exporting `GetPluginFactory`.

## v0.1.0

Transient1 is a two-knob transient shaper: Attack and Sustain, each in dB and each the largest gain its
section can apply. It gives the same result at any input level above the noise floor, gives held material
12 % of the Sustain knob, and has a 1 ms look-ahead reported to the host. It is an independent plug-in
identity (`com.c0dypeng.transient1`, `Cdpg` / `Trn1`).

- Controls: Attack and Sustain, -24..+24 dB in 0.1 dB steps, default 0 (bit-exact bypass of the processing: the output is the input delayed by the latency).
- Latency: 1 ms (48 samples at 48 kHz), constant, reported to the host.
- macOS Universal: AU and VST3, arm64 + x86_64.
- Windows x64: VST3, built natively with MSVC on windows-2022; CTest 1/1 passed on windows-2022 ([run 40087440196](https://github.com/chijenpeng/transient1/actions/runs/40087440196)); the package is a PE32+ x86-64 VST3 exporting `GetPluginFactory`.
  Requires the Microsoft Visual C++ x64 Redistributable.
- macOS bundles are ad-hoc signed, not notarized. Windows builds are unsigned.
- Checked before release: CTest 1/1 (transient1_test: 47 checks pass, 3 skipped without the recordings, 0 failures); editor snapshot rendered; AU and VST3 slices arm64 + x86_64; ad-hoc signatures, ZIP integrity and SHA-256 verified by the packaging script. The real-recording checks ran locally on the maintainer's recordings and are skipped in CI.

Known limits: there is no limiter, so the output can exceed 0 dBFS. Each section fades out between -84 and
-72 dBFS, so very quiet tails are level dependent by design. Above 96 kHz the detector runs at half the
rate. The +-24 dB range and the 0.5 knob skew are choices for this plug-in.

Each ZIP includes installation instructions, dependency notices and build provenance, with a
SHA-256 checksum companion.
