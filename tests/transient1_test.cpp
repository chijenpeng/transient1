// Offline checks of Transient1. Prints PASS/FAIL per check and returns non-zero when anything
// failed. Tests 1-12, 14 and 15 are here; test 13 (port fidelity against an offline C model of the
// same algorithm) is a separate script that is not part of this repository. Real recordings are
// optional: set TRANSIENT1_TEST_AUDIO to a folder holding drums/drums.wav, drums/kick.wav and
// di/bass.wav; without it the checks that need them are skipped with a message.
//
// Portability (MSVC and clang agree): the random generator is a hand-written xorshift (never
// std::*_distribution in an assertion), every draw is its own statement, and lambdas capture
// constants at namespace scope.

#include "PluginProcessor.h"
#include "dsp/TransientShaper.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

using namespace transient;

//==============================================================================
// Allocation counter for the real-time check: counts operator new while armed, on the armed
// thread only (the processor's parameter-tree timer thread allocates on its own schedule).
namespace
{
thread_local bool gCountAllocations = false;
std::atomic<long> gAllocations { 0 };

void* countedAlloc (std::size_t size)
{
    if (gCountAllocations)
        ++gAllocations;
    if (void* p = std::malloc (size > 0 ? size : 1))
        return p;
    throw std::bad_alloc();
}
} // namespace

void* operator new (std::size_t size) { return countedAlloc (size); }
void* operator new[] (std::size_t size) { return countedAlloc (size); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

//==============================================================================
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kBody = voicing::kBodyGain * 24.0;   // 2.88 dB at +-24
int failures = 0;

std::string fmt (const char* f, ...)
{
    char buffer[1024];
    va_list args;
    va_start (args, f);
    std::vsnprintf (buffer, sizeof (buffer), f, args);
    va_end (args);
    return buffer;
}

void check (bool ok, const std::string& what)
{
    std::printf ("  %s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (! ok)
        ++failures;
}

void section (const char* title) { std::printf ("\n%s\n", title); }

struct Rng
{
    uint32_t state = 2463534242u;
    uint32_t next() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
    float bipolar()   // -1..1, from the engine's raw output (portable)
    {
        const uint32_t v = next();
        return (float) ((double) (v >> 8) / 8388608.0 - 1.0);
    }
};

using Channels = std::vector<std::vector<float>>;

struct Audio
{
    Channels ch;
    double fs = 48000.0;
    size_t length() const { return ch.empty() ? 0 : ch[0].size(); }
};

double dbToLin (double db) { return std::pow (10.0, db / 20.0); }
double linToDb (double lin) { return 20.0 * std::log10 (std::max (lin, 1.0e-12)); }

Audio mono (std::vector<float> x, double fs) { Audio a; a.fs = fs; a.ch.push_back (std::move (x)); return a; }

Audio stereoOf (const Audio& m)
{
    Audio a = m;
    a.ch.push_back (m.ch[0]);
    return a;
}

std::vector<float> sine (double fs, double hz, double dbfs, double seconds)
{
    const size_t n = (size_t) std::lround (seconds * fs);
    std::vector<float> x (n);
    const double amp = dbToLin (dbfs);
    for (size_t i = 0; i < n; ++i)
        x[i] = (float) (amp * std::sin (2.0 * kPi * hz * (double) i / fs));
    return x;
}

std::vector<float> noise (double fs, double dbfs, double seconds, uint32_t seed)
{
    Rng rng;
    rng.state = seed;
    const size_t n = (size_t) std::lround (seconds * fs);
    std::vector<float> x (n);
    const double amp = dbToLin (dbfs) * 1.7320508;   // uniform noise: rms = peak / sqrt (3)
    for (size_t i = 0; i < n; ++i)
    {
        const float r = rng.bipolar();
        x[i] = (float) (amp * (double) r);
    }
    return x;
}

/** A kick: 55 Hz sine decaying with tau 40 ms plus a 2 ms click, peak about -6 dBFS. */
void addKick (std::vector<float>& x, double fs, size_t start, double gain = 1.0)
{
    for (size_t i = 0; start + i < x.size(); ++i)
    {
        const double t = (double) i / fs;
        if (t > 0.4)
            break;
        double v = 0.5 * std::exp (-t / 0.040) * std::sin (2.0 * kPi * 55.0 * t);
        v += 0.35 * std::exp (-t / 0.0007) * std::sin (2.0 * kPi * 2500.0 * t);
        x[start + i] += (float) (gain * v);
    }
}

//==============================================================================
// Engine rendering helper

struct Rendered
{
    Channels out;
    std::vector<double> gain;   // applied gain in dB, aligned with the output
    int latency = 0;
};

Rendered renderEngine (const Audio& in, double attackDb, double sustainDb, int block = 512, bool bypass = false)
{
    const int nch = (int) in.ch.size();
    TransientShaper engine;
    engine.prepare (in.fs, block, nch);
    engine.setParameters (attackDb, sustainDb);
    engine.reset();

    Rendered r;
    r.latency = engine.latencySamples();
    const size_t n = in.length();
    r.out = in.ch;
    r.gain.assign (n, 0.0);
    for (size_t pos = 0; pos < n; pos += (size_t) block)
    {
        const int count = (int) std::min<size_t> ((size_t) block, n - pos);
        float* io[2] = { r.out[0].data() + pos, nch > 1 ? r.out[1].data() + pos : nullptr };
        engine.setParameters (attackDb, sustainDb);
        engine.process (io, nch, count, bypass, r.gain.data() + pos);
    }
    return r;
}

/** Min and max of a range of the gain trace. */
void extremes (const std::vector<double>& g, size_t from, size_t to, double& lo, double& hi)
{
    lo = 1.0e9;
    hi = -1.0e9;
    to = std::min (to, g.size());
    for (size_t i = from; i < to; ++i)
    {
        lo = std::min (lo, g[i]);
        hi = std::max (hi, g[i]);
    }
}

double meanOf (const std::vector<double>& g, size_t from, size_t to)
{
    to = std::min (to, g.size());
    double s = 0.0;
    for (size_t i = from; i < to; ++i)
        s += g[i];
    return to > from ? s / (double) (to - from) : 0.0;
}

//==============================================================================
// Test audio

juce::File findTestAudio()
{
    const auto root = juce::SystemStats::getEnvironmentVariable ("TRANSIENT1_TEST_AUDIO", {});
    return root.isEmpty() ? juce::File() : juce::File (root);
}

bool loadWav (const juce::String& relative, Audio& out, int maxChannels = 2)
{
    static const auto root = findTestAudio();
    if (! root.isDirectory())
        return false;
    const auto file = root.getChildFile (relative);
    if (! file.existsAsFile())
        return false;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
        return false;
    const int n = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> buffer ((int) reader->numChannels, n);
    reader->read (&buffer, 0, n, 0, true, true);
    out.fs = reader->sampleRate;
    out.ch.clear();
    for (int c = 0; c < std::min ((int) reader->numChannels, maxChannels); ++c)
        out.ch.emplace_back (buffer.getReadPointer (c), buffer.getReadPointer (c) + n);
    return true;
}

/** Left channel only. */
bool loadMono (const juce::String& relative, Audio& out)
{
    if (! loadWav (relative, out, 1))
        return false;
    return true;
}

Audio slice (const Audio& a, double fromSeconds, double seconds)
{
    Audio r;
    r.fs = a.fs;
    const size_t from = std::min (a.length(), (size_t) std::lround (fromSeconds * a.fs));
    const size_t to = std::min (a.length(), from + (size_t) std::lround (seconds * a.fs));
    for (const auto& c : a.ch)
        r.ch.emplace_back (c.begin() + (long) from, c.begin() + (long) to);
    return r;
}

//==============================================================================
// Measurement helpers

/** THD in dB of y over [from, from + cycles / hz): harmonics 2..12 against the fundamental. */
double thdDb (const std::vector<float>& y, double fs, double hz, size_t from, int cycles)
{
    const size_t n = (size_t) std::lround ((double) cycles * fs / hz);
    const auto amplitude = [&] (int k)
    {
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < n && from + i < y.size(); ++i)
        {
            const double ph = 2.0 * kPi * hz * (double) k * (double) i / fs;
            re += (double) y[from + i] * std::cos (ph);
            im += (double) y[from + i] * std::sin (ph);
        }
        return 2.0 * std::sqrt (re * re + im * im) / (double) n;
    };
    const double fundamental = amplitude (1);
    double sum = 0.0;
    for (int k = 2; k <= 12; ++k)
    {
        const double a = amplitude (k);
        sum += a * a;
    }
    return linToDb (std::sqrt (sum) / fundamental);
}

/** Second-order high-pass (Butterworth), applied four times for 8th order. Returns the filtered copy. */
std::vector<double> highPass (const std::vector<double>& x, double fs, double hz)
{
    const double w = 2.0 * kPi * hz / fs, c = std::cos (w), a = std::sin (w) / (2.0 * 0.70710678);
    const double a0 = 1.0 + a;
    const double b0 = (1.0 + c) / 2.0 / a0, b1 = -(1.0 + c) / a0, b2 = b0, a1 = -2.0 * c / a0, a2 = (1.0 - a) / a0;
    std::vector<double> y = x;
    for (int pass = 0; pass < 4; ++pass)
    {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (auto& v : y)
        {
            const double in = v;
            const double out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = in; y2 = y1; y1 = out;
            v = out;
        }
    }
    return y;
}

double energy (const std::vector<double>& x, size_t from, size_t to)
{
    double e = 0.0;
    for (size_t i = from; i < std::min(to, x.size()); ++i)
        e += x[i] * x[i];
    return e;
}

bool same (float a, float b) { return std::memcmp (&a, &b, sizeof (float)) == 0 || (a == 0.0f && b == 0.0f); }

//==============================================================================
// 1. Bit-exact neutral

void testNeutral (const Audio& drums)
{
    section ("1. Bit-exact neutral (Attack = Sustain = 0)");
    const double rates[] = { 44100.0, 48000.0, 96000.0 };
    const int blocks[] = { 1, 64, 512, 4096 };
    int runs = 0, bad = 0;
    for (const double fs : rates)
    {
        std::vector<Audio> signals;
        signals.push_back (mono (noise (fs, -12.0, 0.7, 11u), fs));
        signals.push_back (mono (sine (fs, 997.0, -6.0, 0.7), fs));
        if (! drums.ch.empty() && std::abs (drums.fs - fs) < 1.0)
            signals.push_back (slice (drums, 0.0, 1.5));
        else
        {
            std::vector<float> k ((size_t) (0.7 * fs), 0.0f);
            addKick (k, fs, 2000);
            addKick (k, fs, 20000, 0.5);
            signals.push_back (mono (k, fs));
        }
        for (const auto& m : signals)
            for (const bool stereo : { false, true })
            {
                Audio in = m;
                if (stereo)
                {
                    in = stereoOf (m);
                    for (auto& v : in.ch[1])
                        v *= -0.5f;
                }
                for (const int block : blocks)
                {
                    const auto r = renderEngine (in, 0.0, 0.0, block);
                    bool ok = true;
                    for (size_t c = 0; c < in.ch.size(); ++c)
                    {
                        const size_t n = in.length() - (size_t) r.latency;
                        ok = ok && std::memcmp (r.out[c].data() + r.latency, in.ch[c].data(), n * sizeof (float)) == 0;
                        for (int i = 0; i < r.latency; ++i)
                            ok = ok && r.out[c][(size_t) i] == 0.0f;
                    }
                    for (const double g : r.gain)
                        ok = ok && g == 0.0;
                    ++runs;
                    bad += ok ? 0 : 1;
                }
            }
    }
    check (bad == 0, fmt ("output == input delayed by latencySamples(), memcmp, gain exactly 0 dB: %d runs (mono/stereo, 44.1/48/96 kHz, blocks 1/64/512/4096), %d differ", runs, bad));

    // Returning to zero after a wild setting: exact again once the ramp and the look-ahead have passed.
    const double fs = 48000.0;
    Audio in = mono (noise (fs, -10.0, 2.0, 5u), fs);
    TransientShaper engine;
    engine.prepare (fs, 512, 1);
    engine.reset();
    std::vector<float> buf = in.ch[0];
    bool exact = true;
    const size_t switchAt = 48000, settleAt = switchAt + 3000;
    for (size_t pos = 0; pos < buf.size(); pos += 512)
    {
        const size_t count = std::min<size_t> (512, buf.size() - pos);
        engine.setParameters (pos < switchAt ? 24.0 : 0.0, pos < switchAt ? -24.0 : 0.0);
        float* io[1] = { buf.data() + pos };
        engine.process (io, 1, (int) count);
    }
    const int latency = engine.latencySamples();
    for (size_t i = settleAt; i < buf.size(); ++i)
        exact = exact && same (buf[i], in.ch[0][i - (size_t) latency]);
    check (exact, "bit-exact again 3000 samples after the knobs return from +24/-24 to 0");

    // Through the processor (parameters, smoothing, ScopedNoDenormals).
    Transient1Processor processor;
    processor.setPlayConfigDetails (2, 2, fs, 512);
    processor.prepareToPlay (fs, 512);
    Audio st = stereoOf (in);
    juce::AudioBuffer<float> io (2, 512);
    juce::MidiBuffer midi;
    bool procExact = true;
    std::vector<float> outL;
    for (size_t pos = 0; pos + 512 <= st.length(); pos += 512)
    {
        for (int c = 0; c < 2; ++c)
            std::memcpy (io.getWritePointer (c), st.ch[(size_t) c].data() + pos, 512 * sizeof (float));
        processor.processBlock (io, midi);
        outL.insert (outL.end(), io.getReadPointer (0), io.getReadPointer (0) + 512);
    }
    const int pl = processor.getLatencySamples();
    for (size_t i = (size_t) pl; i < outL.size(); ++i)
        procExact = procExact && same (outL[i], st.ch[0][i - (size_t) pl]);
    check (procExact && pl == 48, fmt ("processor at defaults is bit-exact, latency %d", pl));
}

//==============================================================================
// 2. Honest dB

void testHonestDb (const Audio& drums)
{
    section ("2. Honest dB (knob = the largest gain, reached and never exceeded)");
    const double fs = 48000.0;
    for (const double knob : { 24.0, -24.0 })
    {
        // Attack onset from silence: a loud 1 kHz burst.
        std::vector<float> x ((size_t) (2.0 * fs), 0.0f);
        const auto tone = sine (fs, 1000.0, -12.0, 1.5);
        std::copy (tone.begin(), tone.end(), x.begin() + 24000);
        auto r = renderEngine (mono (x, fs), knob, 0.0);
        double lo, hi;
        extremes (r.gain, 0, r.gain.size(), lo, hi);
        double peak = knob > 0 ? hi : lo;
        check (std::abs (peak - knob) <= 0.01, fmt ("Attack %+.0f dB onset from silence reaches %+.4f dB", knob, peak));

        // Sustain decay: -0 dBFS burst decaying with tau 100 ms.
        std::vector<float> d ((size_t) (3.0 * fs), 0.0f);
        for (size_t i = 24000; i < d.size(); ++i)
            d[i] = (float) (0.9 * std::exp (-(double) (i - 24000) / (0.1 * fs)) * std::sin (2.0 * kPi * 1000.0 * (double) i / fs));
        r = renderEngine (mono (d, fs), 0.0, knob);
        extremes (r.gain, 0, r.gain.size(), lo, hi);
        peak = knob > 0 ? hi : lo;
        check (std::abs (peak - knob) <= 0.01, fmt ("Sustain %+.0f dB decay (tau 100 ms) reaches %+.4f dB", knob, peak));
    }

    // Never beyond the knob on real material, any sample.
    for (const double knob : { 24.0, -24.0 })
    {
        const Audio a = drums.ch.empty() ? mono (noise (fs, -20.0, 1.0, 3u), fs) : slice (drums, 0.0, 12.0);
        const auto r = renderEngine (a, knob, knob);
        double lo, hi;
        extremes (r.gain, 0, r.gain.size(), lo, hi);
        // Both sections are at the knob, so the sum may reach twice the knob: each section on its own stays inside.
        const auto ra = renderEngine (a, knob, 0.0), rs = renderEngine (a, 0.0, knob);
        double alo, ahi, slo, shi;
        extremes (ra.gain, 0, ra.gain.size(), alo, ahi);
        extremes (rs.gain, 0, rs.gain.size(), slo, shi);
        const double aBound = std::abs (knob) + 0.01;
        check (std::max (std::abs (alo), std::abs (ahi)) <= aBound && std::max (std::abs (slo), std::abs (shi)) <= aBound,
               fmt ("%s, knob %+.0f: Attack alone in [%.3f, %.3f], Sustain alone in [%.3f, %.3f] dB",
                    drums.ch.empty() ? "noise" : "drum mix", knob, alo, ahi, slo, shi));
        (void) lo; (void) hi;
    }

    // A 6 dB noise step is not much of an attack.
    {
        std::vector<float> x = noise (fs, -30.0, 1.0, 17u);
        auto louder = noise (fs, -24.0, 1.0, 19u);
        x.insert (x.end(), louder.begin(), louder.end());
        const auto r = renderEngine (mono (x, fs), 24.0, 0.0);
        double lo, hi;
        extremes (r.gain, (size_t) fs, 2 * (size_t) fs, lo, hi);
        check (hi <= 0.55 * 24.0, fmt ("6 dB noise step: Attack gain peaks at %.2f dB (%.0f %% of 24, limit 55 %%)", hi, 100.0 * hi / 24.0));
    }
}

//==============================================================================
// 3. Level independence

void testLevelIndependence (const Audio& drums)
{
    section ("3. Level independence");
    if (drums.ch.empty())
    {
        std::printf ("  SKIP  drum mix not found (TRANSIENT1_TEST_AUDIO/drums/drums.wav)\n");
        return;
    }
    const Audio base = slice (drums, 0.0, 12.0);
    const double fs = base.fs;
    const auto ref = renderEngine (base, 24.0, 24.0);
    const size_t start = (size_t) (0.1 * fs);

    // Active = the input peak over the previous 100 ms is above -60 dBFS (at that scale).
    const size_t hop = (size_t) (0.01 * fs);
    std::vector<double> blockPeak (base.length() / hop + 1, 0.0);
    for (size_t i = 0; i < base.length(); ++i)
    {
        const double v = std::abs ((double) base.ch[0][i]);
        blockPeak[i / hop] = std::max (blockPeak[i / hop], v);
    }
    for (const double scaleDb : { 6.0, -6.0, -12.0, -24.0, -36.0 })
    {
        Audio scaled = base;
        const float s = (float) dbToLin (scaleDb);
        for (auto& c : scaled.ch)
            for (auto& v : c)
                v *= s;
        const auto r = renderEngine (scaled, 24.0, 24.0);
        double worst = 0.0;
        size_t counted = 0;
        for (size_t i = start; i < r.gain.size(); ++i)
        {
            double peak = 0.0;
            const size_t b = i / hop;
            for (size_t k = (b >= 10 ? b - 10 : 0); k <= b; ++k)
                peak = std::max (peak, blockPeak[k]);
            if (linToDb (peak * dbToLin (scaleDb)) < -60.0)
                continue;
            ++counted;
            worst = std::max (worst, std::abs (r.gain[i] - ref.gain[i]));
        }
        check (worst <= 0.3, fmt ("scale %+5.0f dB: gain within %.4f dB of the unscaled trajectory (%.0f %% of samples compared, limit 0.3)",
                                  scaleDb, worst, 100.0 * (double) counted / (double) (r.gain.size() - start)));
    }

    // Below the floor the Sustain gain fades to 0: a decay with tau 100 ms reads <= 0.5 dB at -100 dBFS.
    std::vector<float> d ((size_t) (6.0 * fs), 0.0f);
    for (size_t i = (size_t) fs; i < d.size(); ++i)
        d[i] = (float) (0.5 * std::exp (-(double) (i - (size_t) fs) / (0.1 * fs)) * std::sin (2.0 * kPi * 1000.0 * (double) i / fs));
    const auto r = renderEngine (mono (d, fs), 24.0, 24.0);
    // -100 dBFS: 0.5 * exp (-t / 0.1) = 1e-5  ->  t = 0.1 * ln (50000)
    const size_t at = (size_t) fs + (size_t) (0.1 * std::log (0.5 / 1.0e-5) * fs);
    double lo, hi;
    extremes (r.gain, at, at + (size_t) (0.1 * fs), lo, hi);
    check (std::max (std::abs (lo), std::abs (hi)) <= 0.5, fmt ("decay at -100 dBFS: gain in [%.3f, %.3f] dB", lo, hi));
}

//==============================================================================
// 4. Steady and held material

void testSteady()
{
    section ("4. Steady / held material (constant body gain 0.12 x Sustain)");
    const double fs = 48000.0;
    const double freqs[] = { 40.0, 100.0, 1000.0, 5000.0 };
    const double levels[] = { -12.0, -40.0, -70.0 };
    bool attackZero = true;
    double worstBody = 0.0, worstRipple = 0.0, worstLevel = 0.0, worstThd = -300.0;
    for (const double hz : freqs)
    {
        double firstLevelGain = 0.0;
        for (const double level : levels)
        {
            const Audio in = mono (sine (fs, hz, level, 3.0), fs);
            for (const double knob : { 15.0, -15.0 })
            {
                const auto r = renderEngine (in, knob, 0.0);
                double lo, hi;
                extremes (r.gain, (size_t) fs, r.gain.size(), lo, hi);
                attackZero = attackZero && std::abs (lo) < 0.001 && std::abs (hi) < 0.001;
            }
            for (const double knob : { 24.0, -24.0 })
            {
                const auto r = renderEngine (in, 0.0, knob);
                double lo, hi;
                extremes (r.gain, (size_t) fs, r.gain.size(), lo, hi);
                const double expected = knob > 0 ? kBody : -kBody;
                worstBody = std::max (worstBody, std::max (std::abs (lo - expected), std::abs (hi - expected)));
                worstRipple = std::max (worstRipple, hi - lo);
                if (knob > 0)
                {
                    if (level < levels[0] + 0.5)
                        firstLevelGain = hi;
                    worstLevel = std::max (worstLevel, std::abs (hi - firstLevelGain));
                }
                if (hz <= 100.0)
                    worstThd = std::max (worstThd, thdDb (r.out[0], fs, hz, (size_t) fs + (size_t) r.latency, (int) hz));
            }
        }
    }
    check (attackZero, "Attack +-15 alone on 40 Hz .. 5 kHz at -12/-40/-70 dBFS: gain 0.000 dB (< 0.001)");
    check (worstBody <= 0.01, fmt ("Sustain +-24: gain is +-%.2f dB, worst deviation %.5f dB (limit 0.01)", kBody, worstBody));
    check (worstRipple <= 0.01, fmt ("ripple %.6f dB p-p (limit 0.01)", worstRipple));
    check (worstLevel <= 0.01, fmt ("body gain equal at -12/-40/-70 dBFS within %.6f dB (limit 0.01)", worstLevel));
    check (worstThd <= -60.0, fmt ("THD of held 40 / 100 Hz at Sustain +-24: %.1f dB (limit -60)", worstThd));

    // Held noise onset: reaches the body gain within 70 ms, no onset boost.
    {
        std::vector<float> x ((size_t) (0.5 * fs), 0.0f);
        const auto n = noise (fs, -20.0, 1.5, 23u);
        x.insert (x.end(), n.begin(), n.end());
        const auto r = renderEngine (mono (x, fs), 0.0, 24.0);
        const size_t onset = (size_t) (0.5 * fs);
        const double early = meanOf (r.gain, onset, onset + (size_t) (0.005 * fs));
        double lo, hi;
        extremes (r.gain, onset, onset + (size_t) (0.005 * fs), lo, hi);
        const double settled = meanOf (r.gain, onset + (size_t) (0.07 * fs), onset + (size_t) (0.5 * fs));
        check (hi < 3.0 && settled >= 0.98 * kBody - 0.15,
               fmt ("noise onset: first 5 ms mean %.2f max %.2f dB (< 3), mean 70-500 ms %.2f dB (>= %.2f)", early, hi, settled, 0.98 * kBody - 0.15));
    }
    // Stationary white noise: mean within 0.3 dB of the body gain.
    {
        const auto r = renderEngine (mono (noise (fs, -20.0, 4.0, 29u), fs), 0.0, 24.0);
        const double m = meanOf (r.gain, (size_t) (1.0 * fs), r.gain.size());
        check (std::abs (m - kBody) <= 0.3, fmt ("stationary white noise: mean gain %.3f dB vs body %.2f (+-0.3)", m, kBody));
    }

    // Room-style material: a steady noise wash with a decaying noise hit (tau 80 ms) every 300 ms, peaking 6 dB above the wash
    // (8 dB would fire the Sustain reset and hide the effect).
    // The body term must not cut the hits along with the wash (Sustain -24), nor lose them at +24.
    {
        const size_t period = (size_t) (0.3 * fs);
        auto x = noise (fs, -30.0, 6.0, 31u);
        const auto burst = noise (fs, -30.0 + 6.0, 6.0, 37u);
        const double b = std::sqrt (dbToLin (6.0) * dbToLin (6.0) - 1.0) / dbToLin (6.0);   // hit rms so that wash + hit = +6 dB
        std::vector<size_t> starts;
        for (size_t t = (size_t) (1.0 * fs); t + period < x.size(); t += period)
        {
            starts.push_back (t);
            for (size_t i = 0; i < (size_t) (0.2 * fs); ++i)
                x[t + i] += (float) (b * (double) burst[t + i] * std::exp (-(double) i / (0.08 * fs)));
        }
        for (const double knob : { -24.0, 24.0 })
        {
            const auto r = renderEngine (mono (x, fs), 0.0, knob);
            double onsetMean = 0.0, washMean = 0.0;
            for (const size_t t : starts)
            {
                double lo, hi;
                extremes (r.gain, t, t + (size_t) (0.016 * fs), lo, hi);
                onsetMean += hi / (double) starts.size();   // peak gain within 16 ms of the hit
                washMean += meanOf (r.gain, t + (size_t) (0.2 * fs), t + period) / (double) starts.size();
            }
            const double contrast = onsetMean - washMean;
            // -24: the hits must be at least 2.5 dB less attenuated than the wash. +24: the hits must keep up with the wash (within 1 dB).
            const bool ok = knob < 0 ? contrast >= 2.5 : std::abs (contrast) <= 1.0 && onsetMean >= 3.0;
            check (ok, fmt ("room-style hits 6 dB over a wash, Sustain %+.0f: hit peak gain %.2f dB, wash %.2f dB, hits minus wash %+.2f dB (%s)",
                            knob, onsetMean, washMean, contrast, knob < 0 ? ">= 2.5" : "within 1, hits >= 3"));
        }
    }

    // Above 96 kHz the detector runs at half the rate: held 20 / 40 Hz must stay clean.
    {
        double worst = -300.0, worstRipple192 = 0.0;
        for (const double rate : { 176400.0, 192000.0 })
            for (const double hz : { 20.0, 40.0 })
                for (const double knob : { 24.0, -24.0 })
                {
                    const Audio in = mono (sine (rate, hz, -12.0, 3.0), rate);
                    const auto r = renderEngine (in, 0.0, knob);
                    worst = std::max (worst, thdDb (r.out[0], rate, hz, (size_t) rate + (size_t) r.latency, (int) hz));
                    double lo, hi;
                    extremes (r.gain, (size_t) rate, r.gain.size(), lo, hi);
                    worstRipple192 = std::max (worstRipple192, hi - lo);
                }
        check (worst <= -60.0, fmt ("176.4 / 192 kHz, held 20 and 40 Hz, Sustain +-24: THD %.1f dB (limit -60), gain ripple %.4f dB p-p", worst, worstRipple192));
    }
}

//==============================================================================
// 5. Fast repeats

double hitGain (const std::vector<double>& g, size_t from, size_t to)
{
    double lo, hi;
    extremes (g, from, to, lo, hi);
    return hi;
}

void testRepeats (const Audio& kick)
{
    section ("5. Fast repeats (Attack +24)");
    const double fs = 48000.0;
    for (const double rate : { 8.0, 16.0 })
    {
        std::vector<float> iso ((size_t) (1.5 * fs), 0.0f);
        addKick (iso, fs, 24000);
        const double isolated = hitGain (renderEngine (mono (iso, fs), 24.0, 0.0).gain, 24000, 24000 + 6000);
        std::vector<float> train ((size_t) (2.0 * fs), 0.0f);
        const size_t period = (size_t) std::lround (fs / rate);
        std::vector<size_t> starts;
        for (size_t s = 24000; s + period < train.size(); s += period)
        {
            addKick (train, fs, s);
            starts.push_back (s);
        }
        const auto r = renderEngine (mono (train, fs), 24.0, 0.0);
        double worst = 1.0e9;
        for (size_t k = 3; k + 1 < starts.size(); ++k)   // steady state: the first repeats are lower (see dsp.md)
            worst = std::min (worst, hitGain (r.gain, starts[k], starts[k] + period));
        const double limit = rate < 10.0 ? 0.90 : 0.70;
        check (worst >= limit * isolated, fmt ("synthetic kick at %.0f Hz keeps %.0f %% of an isolated hit's %.2f dB (limit %.0f %%)",
                                               rate, 100.0 * worst / isolated, isolated, 100.0 * limit));
    }

    if (kick.ch.empty())
    {
        std::printf ("  SKIP  real kick hit (TRANSIENT1_TEST_AUDIO/drums/kick.wav not found)\n");
        return;
    }
    // The loudest hit of the stem, overlap-added at 8 / 16 Hz.
    const auto& k = kick.ch[0];
    size_t peakAt = 0;
    float peak = 0.0f;
    for (size_t i = 0; i < k.size(); ++i)
        if (std::abs (k[i]) > peak)
        {
            peak = std::abs (k[i]);
            peakAt = i;
        }
    const size_t from = peakAt > 480 ? peakAt - 480 : 0, len = (size_t) (0.15 * kick.fs);   // the hit and its first 150 ms
    for (const double rate : { 8.0, 16.0 })
    {
        const size_t period = (size_t) std::lround (kick.fs / rate);
        std::vector<float> iso ((size_t) (1.5 * kick.fs), 0.0f), train ((size_t) (2.5 * kick.fs), 0.0f);
        const size_t lead = 24000;
        for (size_t i = 0; i < len && from + i < k.size(); ++i)
            iso[lead + i] = k[from + i];
        std::vector<size_t> starts;
        for (size_t s = lead; s + period < train.size(); s += period)
        {
            for (size_t i = 0; i < len && from + i < k.size() && s + i < train.size(); ++i)
                train[s + i] += k[from + i];
            starts.push_back (s);
        }
        const double isolated = hitGain (renderEngine (mono (iso, kick.fs), 24.0, 0.0).gain, lead, lead + period);
        const auto r = renderEngine (mono (train, kick.fs), 24.0, 0.0);
        double worst = 1.0e9;
        for (size_t i = 3; i + 1 < starts.size(); ++i)
            worst = std::min (worst, hitGain (r.gain, starts[i], starts[i] + period));
        check (worst >= 0.90 * isolated, fmt ("real kick hit at %.0f Hz keeps %.0f %% of an isolated hit's %.2f dB (limit 90 %%)",
                                              rate, 100.0 * worst / isolated, isolated));
    }
}

//==============================================================================
// 6. Shape

void testShape()
{
    section ("6. Shape (tone onset from silence, -12 dBFS)");
    const double fs = 48000.0;
    for (const double knob : { 24.0, 12.0, -24.0 })
    {
        std::vector<float> x ((size_t) (2.0 * fs), 0.0f);
        const auto tone = sine (fs, 1000.0, -12.0, 1.5);
        std::copy (tone.begin(), tone.end(), x.begin() + 24000);
        const auto r = renderEngine (mono (x, fs), knob, 0.0);
        const double sign = knob > 0 ? 1.0 : -1.0;
        size_t peakAt = 0;
        double peak = 0.0;
        for (size_t i = 0; i < r.gain.size(); ++i)
            if (sign * r.gain[i] > peak)
            {
                peak = sign * r.gain[i];
                peakAt = i;
            }
        // Last sample at the knob (the plateau), then monotone decay.
        size_t plateauEnd = peakAt;
        while (plateauEnd + 1 < r.gain.size() && sign * r.gain[plateauEnd + 1] >= peak - 1.0e-9)
            ++plateauEnd;
        bool monotone = true;
        double minAfter = 1.0e9;
        for (size_t i = plateauEnd; i + 1 < r.gain.size(); ++i)
        {
            monotone = monotone && sign * r.gain[i + 1] <= sign * r.gain[i] + 1.0e-9;
            minAfter = std::min (minAfter, sign * r.gain[i + 1]);
        }
        const double at60 = std::abs (r.gain[24000 + (size_t) r.latency + (size_t) (0.06 * fs)]);
        const double atOnset = sign * r.gain[24000 + (size_t) r.latency];
        check (std::abs (peak - std::abs (knob)) <= 0.01 && monotone && minAfter >= -0.01 && at60 < 0.1,
               fmt ("Attack %+.0f: peak %.3f dB, gain at the audible onset %.2f dB, |gain| %.3f dB 60 ms later, monotone decay %s, undershoot %.4f dB",
                    knob, peak, atOnset, at60, monotone ? "yes" : "NO", std::min (0.0, minAfter)));
    }
}

//==============================================================================
// 7. Latency

int impulseAlignment (double fs, double attack, double sustain)
{
    std::vector<float> x ((size_t) (0.1 * fs), 0.0f);
    x[200] = 0.5f;
    const auto r = renderEngine (mono (x, fs), attack, sustain);
    size_t at = 0;
    float peak = 0.0f;
    for (size_t i = 0; i < r.out[0].size(); ++i)
        if (std::abs (r.out[0][i]) > peak)
        {
            peak = std::abs (r.out[0][i]);
            at = i;
        }
    return (int) at - 200;
}

void testLatency()
{
    section ("7. Latency (reported == measured, constant)");
    for (const double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 })
    {
        TransientShaper engine;
        engine.prepare (fs, 512, 2);
        const int expected = (int) std::lround (0.001 * fs);
        const int measured = impulseAlignment (fs, 0.0, 0.0), measured2 = impulseAlignment (fs, 24.0, -24.0);

        Transient1Processor processor;
        processor.setPlayConfigDetails (2, 2, fs, 512);
        processor.prepareToPlay (fs, 512);
        check (engine.latencySamples() == expected && measured == expected && measured2 == expected && processor.getLatencySamples() == expected,
               fmt ("%7.1f Hz: engine %d, processor %d, impulse at neutral %d, at A+24/S-24 %d samples (%.3f ms)", fs,
                    engine.latencySamples(), processor.getLatencySamples(), measured, measured2, 1000.0 * expected / fs));
    }
}

//==============================================================================
// 8. Clicks

void testClicks()
{
    section ("8. Clicks (HF after an Attack cut, against an unsmoothed zero-latency gain)");
    const double fs = 48000.0;
    // 1 kHz with a 5 ms raised-cosine onset at -6 dBFS.
    std::vector<float> x ((size_t) (1.0 * fs), 0.0f);
    const size_t onset = 12000, rise = (size_t) (0.005 * fs);
    for (size_t i = onset; i < x.size(); ++i)
    {
        const double env = i < onset + rise ? 0.5 * (1.0 - std::cos (kPi * (double) (i - onset) / (double) rise)) : 1.0;
        x[i] = (float) (dbToLin (-6.0) * env * std::sin (2.0 * kPi * 1000.0 * (double) i / fs));
    }
    const auto r = renderEngine (mono (x, fs), -24.0, 0.0);
    const size_t L = (size_t) r.latency;

    // The raw per-sample gain, recovered from the boxcar means: raw[i] = raw[i-L-1] + (L+1) (mean[i] - mean[i-1]).
    std::vector<double> raw (r.gain.size(), 0.0);
    for (size_t i = 0; i < raw.size(); ++i)
    {
        const double step = (r.gain[i] - (i > 0 ? r.gain[i - 1] : 0.0)) * (double) (L + 1);
        raw[i] = step + (i > L ? raw[i - L - 1] : 0.0);
    }
    std::vector<double> ours (x.size()), plain (x.size());
    for (size_t i = 0; i < x.size(); ++i)
    {
        ours[i] = (double) r.out[0][i];
        plain[i] = (double) x[i] * dbToLin (raw[i]);
    }
    const auto hpOurs = highPass (ours, fs, 5000.0), hpRef = highPass (plain, fs, 5000.0);
    const size_t a = onset - 200, b = onset + (size_t) (0.1 * fs);
    const double eOurs = energy (hpOurs, a, b), eRef = energy (hpRef, a, b);
    const double diff = 10.0 * std::log10 (std::max (eRef, 1.0e-30) / std::max (eOurs, 1.0e-30));
    check (diff >= 15.0, fmt ("energy above 5 kHz: smoothed look-ahead %.1f dB, unsmoothed zero-latency %.1f dB re 1: %.1f dB lower (limit 15)",
                              10.0 * std::log10 (std::max (eOurs, 1.0e-30)), 10.0 * std::log10 (std::max (eRef, 1.0e-30)), diff));
}

//==============================================================================
// 9. Stereo link

void testStereo (const Audio& drums)
{
    section ("9. Stereo link");
    const double fs = 48000.0;

    // R is a steady tone louder than any hit on L: the linked detector sees no onset.
    std::vector<float> l ((size_t) (2.0 * fs), 0.0f);
    addKick (l, fs, 12000, 0.45);
    addKick (l, fs, 50000, 0.45);
    Audio st;
    st.fs = fs;
    st.ch.push_back (l);
    st.ch.push_back (sine (fs, 1000.0, -6.0, 2.0));
    auto r = renderEngine (st, 24.0, 0.0);
    double lo, hi;
    extremes (r.gain, (size_t) (0.5 * fs), r.gain.size(), lo, hi);
    check (std::max (std::abs (lo), std::abs (hi)) <= 0.1, fmt ("L hits under a steady R: Attack gain stays within [%.4f, %.4f] dB (limit 0.1)", lo, hi));

    // One gain on both channels: out / delayed in is equal.
    Audio in;
    in.fs = fs;
    in.ch.push_back (drums.ch.empty() ? l : slice (drums, 0.0, 6.0).ch[0]);
    in.ch.push_back (drums.ch.empty() ? noise (fs, -30.0, 2.0, 31u) : slice (drums, 0.0, 6.0).ch[drums.ch.size() > 1 ? 1 : 0]);
    r = renderEngine (in, 18.0, -12.0);
    double worst = 0.0;
    size_t used = 0;
    const size_t L = (size_t) r.latency;
    for (size_t i = L; i < in.length(); ++i)
    {
        const float a = in.ch[0][i - L], b = in.ch[1][i - L];
        if (std::abs (a) > 0.01f && std::abs (b) > 0.01f)
        {
            ++used;
            const double ga = (double) r.out[0][i] / (double) a, gb = (double) r.out[1][i] / (double) b;
            worst = std::max (worst, std::abs (ga - gb) / std::max (ga, 1.0e-9));
        }
    }
    check (used > 1000 && worst <= 1.0e-5, fmt ("both channels share one gain: worst relative difference %.2e over %zu samples (limit 1e-5)", worst, used));

    // Mono equals dual mono.
    const Audio m = mono (noise (fs, -20.0, 1.0, 37u), fs);
    const auto rm = renderEngine (m, 20.0, 20.0), rs = renderEngine (stereoOf (m), 20.0, 20.0);
    bool same = rm.gain == rs.gain && rm.out[0] == rs.out[0] && rs.out[0] == rs.out[1];
    check (same, "mono renders identically to dual mono (gain and both output channels)");
}

//==============================================================================
// 10. Allocation-free

void testAllocation()
{
    section ("10. Allocation-free");
    const double fs = 48000.0;
    const Audio in = stereoOf (mono (noise (fs, -15.0, 1.0, 41u), fs));
    long engineAllocations = 0, processorAllocations = 0;
    {
        TransientShaper engine;
        engine.prepare (fs, 512, 2);
        engine.reset();
        std::vector<float> a (512, 0.1f), b (512, 0.2f);
        float* io[2] = { a.data(), b.data() };
        gAllocations = 0;
        gCountAllocations = true;
        for (int i = 0; i < 200; ++i)
        {
            engine.setParameters (i % 2 ? 24.0 : -24.0, i % 3 ? 5.0 : -9.0);
            engine.process (io, 2, 512, i % 7 == 0);
            TelemetryFrame f;
            while (engine.telemetry().pop (f)) {}
        }
        gCountAllocations = false;
        engineAllocations = gAllocations.load();
    }
    {
        Transient1Processor processor;
        processor.setPlayConfigDetails (2, 2, fs, 512);
        processor.prepareToPlay (fs, 512);
        juce::AudioBuffer<float> io (2, 512);
        io.clear();
        juce::MidiBuffer midi;
        gAllocations = 0;
        gCountAllocations = true;
        for (int i = 0; i < 200; ++i)
        {
            if (i % 5 == 0)
                processor.processBlockBypassed (io, midi);
            else
                processor.processBlock (io, midi);
        }
        gCountAllocations = false;
        processorAllocations = gAllocations.load();
    }
    check (engineAllocations == 0, fmt ("engine process: %ld allocations in 200 blocks", engineAllocations));
    check (processorAllocations == 0, fmt ("processor processBlock / processBlockBypassed: %ld allocations in 200 blocks", processorAllocations));
}

//==============================================================================
// 11. State round trip and parameters

void testState()
{
    section ("11. State round trip, parameter mapping");
    const auto setValue = [] (Transient1Processor& p, const char* id, float v)
    {
        auto* param = p.apvts.getParameter (id);
        param->setValueNotifyingHost (param->convertTo0to1 (v));
    };
    const auto raw = [] (Transient1Processor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); };

    Transient1Processor a;
    setValue (a, ParamID::attack, 6.0f);
    setValue (a, ParamID::sustain, -13.7f);
    juce::MemoryBlock state;
    a.getStateInformation (state);
    Transient1Processor b;
    b.setStateInformation (state.getData(), (int) state.getSize());
    check (same (raw (a, ParamID::attack), raw (b, ParamID::attack)) && same (raw (a, ParamID::sustain), raw (b, ParamID::sustain))
               && std::abs (raw (b, ParamID::attack) - 6.0f) < 1.0e-4f && std::abs (raw (b, ParamID::sustain) + 13.7f) < 1.0e-4f,
           fmt ("round trip reproduces both values bit for bit (attack %.4f, sustain %.4f)", raw (b, ParamID::attack), raw (b, ParamID::sustain)));

    Transient1Processor fresh;
    juce::MemoryBlock freshState;
    fresh.getStateInformation (freshState);
    Transient1Processor c;
    setValue (c, ParamID::attack, 9.0f);
    c.setStateInformation (freshState.getData(), (int) freshState.getSize());
    check (std::abs (raw (c, ParamID::attack)) < 1.0e-5f && std::abs (raw (c, ParamID::sustain)) < 1.0e-5f, "a defaults-only state loads attack 0, sustain 0");

    // Newer version: defaults, no crash.
    {
        auto xml = juce::parseXML (state.toString());
        std::unique_ptr<juce::XmlElement> parsed = a.apvts.copyState().createXml();
        parsed->setAttribute ("version", 2);
        juce::MemoryBlock newer;
        juce::AudioProcessor::copyXmlToBinary (*parsed, newer);
        Transient1Processor d;
        setValue (d, ParamID::attack, 11.0f);
        d.setStateInformation (newer.getData(), (int) newer.getSize());
        check (std::abs (raw (d, ParamID::attack)) < 1.0e-5f && std::abs (raw (d, ParamID::sustain)) < 1.0e-5f, "an unknown newer state version loads the defaults");
        const char garbage[] = "not a state";
        d.setStateInformation (garbage, (int) sizeof (garbage));
        check (true, "garbage state data is ignored without a crash");
    }

    // Mapping, text, defaults.
    auto* attack = a.apvts.getParameter (ParamID::attack);
    check (std::abs (attack->convertTo0to1 (6.0f) - 0.75f) < 1.0e-3f && std::abs (attack->convertTo0to1 (-6.0f) - 0.25f) < 1.0e-3f
               && std::abs (attack->convertTo0to1 (0.0f) - 0.5f) < 1.0e-3f && std::abs (attack->convertTo0to1 (24.0f) - 1.0f) < 1.0e-6f,
           fmt ("symmetric skew 0.5: +6 dB at %.3f, -6 dB at %.3f, 0 at %.3f of the travel", attack->convertTo0to1 (6.0f), attack->convertTo0to1 (-6.0f), attack->convertTo0to1 (0.0f)));
    check (attack->getText (attack->convertTo0to1 (6.0f), 0) == "+6.0 dB" && attack->getText (attack->convertTo0to1 (-12.5f), 0) == "-12.5 dB"
               && attack->getText (attack->convertTo0to1 (0.0f), 0) == "0.0 dB",
           fmt ("text '%s' '%s' '%s'", attack->getText (attack->convertTo0to1 (6.0f), 0).toRawUTF8(),
                attack->getText (attack->convertTo0to1 (-12.5f), 0).toRawUTF8(), attack->getText (attack->convertTo0to1 (0.0f), 0).toRawUTF8()));
    check (same (attack->getDefaultValue(), attack->convertTo0to1 (0.0f)) && std::abs (attack->convertFrom0to1 (attack->convertTo0to1 (6.1f)) - 6.1f) < 0.051f,
           "default is 0 dB, values snap to 0.1 dB steps");
}

//==============================================================================
// 12. Parameter sweeps

void testSweeps()
{
    section ("12. Parameter sweeps (no zipper)");
    const double fs = 48000.0;
    const Audio in = mono (sine (fs, 1000.0, -12.0, 4.0), fs);
    TransientShaper engine;
    engine.prepare (fs, 512, 1);
    engine.setParameters (0.0, 0.0);
    engine.reset();
    std::vector<float> x = in.ch[0];
    std::vector<double> g (x.size());
    for (size_t pos = 0; pos < x.size(); pos += 512)
    {
        const size_t count = std::min<size_t> (512, x.size() - pos);
        const double t = (double) pos / fs;
        engine.setParameters (24.0 * std::sin (2.0 * kPi * 1.0 * t), 24.0 * std::sin (2.0 * kPi * 1.0 * t + 1.0));
        float* io[1] = { x.data() + pos };
        engine.process (io, 1, (int) count, false, g.data() + pos);
    }
    double maxStep = 0.0;
    for (size_t i = 1; i < g.size(); ++i)
        maxStep = std::max (maxStep, std::abs (g[i] - g[i - 1]));
    check (maxStep <= 0.01, fmt ("1 Hz +-24 dB sweep of both knobs, host blocks of 512: largest gain step %.5f dB per sample (limit 0.01)", maxStep));

    // A jump of Sustain from -24 to +24 and of Attack on a hit.
    TransientShaper jump;
    jump.prepare (fs, 512, 1);
    jump.setParameters (-24.0, -24.0);
    jump.reset();
    std::vector<float> y = in.ch[0];
    std::vector<double> gj (y.size());
    for (size_t pos = 0; pos < y.size(); pos += 512)
    {
        const size_t count = std::min<size_t> (512, y.size() - pos);
        if (pos >= 96000)
            jump.setParameters (24.0, 24.0);
        float* io[1] = { y.data() + pos };
        jump.process (io, 1, (int) count, false, gj.data() + pos);
    }
    double jumpStep = 0.0, endGain = 0.0;
    for (size_t i = 60000; i < gj.size(); ++i)
        jumpStep = std::max (jumpStep, std::abs (gj[i] - gj[i - 1]));
    endGain = gj.back();
    check (jumpStep <= 0.01 && std::abs (endGain - kBody) < 1.0e-6, fmt ("jump -24 -> +24: largest step %.5f dB per sample, ends exactly at %.6f dB", jumpStep, endGain));
}

//==============================================================================
// 14. CPU

void testCpu()
{
    section ("14. CPU guard");
    const double fs = 48000.0;
    const Audio in = stereoOf (mono (noise (fs, -15.0, 10.0, 43u), fs));
    TransientShaper engine;
    engine.prepare (fs, 512, 2);
    engine.setParameters (12.0, -12.0);
    engine.reset();
    Channels work = in.ch;
    double seconds = 1.0e9;   // best of 5: a loaded machine only ever makes a run slower
    for (int run = 0; run < 5; ++run)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t pos = 0; pos + 512 <= in.length(); pos += 512)
        {
            float* io[2] = { work[0].data() + pos, work[1].data() + pos };
            engine.process (io, 2, 512);
        }
        seconds = std::min (seconds, std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count());
    }
    // A guard against gross regressions only (an unoptimised or allocating build is far slower):
    // an idle Apple-silicon machine measures about 400x, a heavily loaded one has measured 142x.
    const double times = 10.0 / std::max (seconds, 1.0e-9);
    check (times > 50.0, fmt ("10 s of stereo at 48 kHz, best of 5, in %.3f s: %.0f x realtime (limit 50)", seconds, times));
}

//==============================================================================
// 15. Held-material Sustain on the DIs

void testDi (const Audio& bass)
{
    section ("15. Held-material Sustain on the bass DI (reduced: unweighted energy, no BS.1770)");
    if (bass.ch.empty())
    {
        std::printf ("  SKIP  bass DI not found (TRANSIENT1_TEST_AUDIO/di/bass.wav)\n");
        return;
    }
    const auto changeDb = [&] (double knob)
    {
        const auto r = renderEngine (bass, 0.0, knob);
        double eIn = 0.0, eOut = 0.0;
        const size_t L = (size_t) r.latency;
        for (size_t i = L; i < bass.length(); ++i)
        {
            eIn += (double) bass.ch[0][i - L] * (double) bass.ch[0][i - L];
            eOut += (double) r.out[0][i] * (double) r.out[0][i];
        }
        return 10.0 * std::log10 (eOut / eIn);
    };
    const double up = changeDb (24.0), down = changeDb (-24.0);
    // Before the body term was suppressed on onsets this read +2.94 / -2.84.
    check (std::abs (up - 2.55) <= 0.3 && std::abs (down + 2.40) <= 0.3 && up >= 2.34 && down <= -2.24,
           fmt ("bass DI energy change at Sustain +24 / -24: %+.2f / %+.2f dB (expected +2.55 / -2.40, +-0.3; within 0.6 of +2.94 / -2.84)", up, down));

    // No boost in the first 5 ms after an onset (found as a 10 dB jump of the 2 ms level).
    const auto r = renderEngine (bass, 0.0, 24.0);
    const size_t block = (size_t) (0.002 * bass.fs);
    std::vector<double> level;
    for (size_t i = 0; i + block <= bass.length(); i += block)
    {
        double e = 0.0;
        for (size_t k = 0; k < block; ++k)
            e += (double) bass.ch[0][i + k] * (double) bass.ch[0][i + k];
        level.push_back (linToDb (std::sqrt (e / (double) block)));
    }
    int onsets = 0;
    double worst = -100.0;
    size_t lastOnset = 0;
    for (size_t b = 12; b < level.size(); ++b)
    {
        double before = -200.0;
        for (size_t k = b - 10; k < b; ++k)
            before = std::max (before, level[k]);
        if (level[b] > -45.0 && level[b] > before + 10.0 && (lastOnset == 0 || (b - lastOnset) * block > (size_t) (0.1 * bass.fs)))
        {
            lastOnset = b;
            ++onsets;
            double lo, hi;
            extremes (r.gain, b * block + (size_t) r.latency, b * block + (size_t) r.latency + (size_t) (0.005 * bass.fs), lo, hi);
            worst = std::max (worst, hi);
        }
    }
    check (onsets >= 5 && worst < 3.0, fmt ("%d onsets: largest Sustain gain in the 5 ms after the audible onset %.2f dB (limit 3)", onsets, worst));
}

} // namespace

int main()
{
    const juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("Transient1 tests\n");

    Audio drums, kick, bass;
    const bool haveDrums = loadMono ("drums/drums.wav", drums);
    const bool haveKick = loadMono ("drums/kick.wav", kick);
    const bool haveBass = loadMono ("di/bass.wav", bass);
    if (! haveDrums || ! haveKick || ! haveBass)
        std::printf ("note: TRANSIENT1_TEST_AUDIO not set or incomplete (drums %d, kick %d, bass %d); the checks that need it are skipped or use synthetic signals\n",
                     haveDrums, haveKick, haveBass);
    if (! haveDrums) drums.ch.clear();
    if (! haveKick) kick.ch.clear();
    if (! haveBass) bass.ch.clear();

    testNeutral (drums);
    testHonestDb (drums);
    testLevelIndependence (drums);
    testSteady();
    testRepeats (kick);
    testShape();
    testLatency();
    testClicks();
    testStereo (drums);
    testAllocation();
    testState();
    testSweeps();
    section ("13. Port fidelity");
    std::printf ("  note  checked by a separate offline script against a C model of the algorithm (target 0.05 dB), not by this program\n");
    testCpu();
    testDi (bass);

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
