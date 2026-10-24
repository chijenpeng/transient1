// Renders the editor to a PNG without a host or a screen, after running real programme through
// the processor so the gain trace shows genuine telemetry (Attack +6 dB, Sustain -4 dB).
//
//   transient1_snapshot out.png [editorScale=1] [pixelScale=2] [startSeconds=6] [--synthetic]
//
// The programme is a synthetic kick-and-snare pattern. When TRANSIENT1_TEST_AUDIO names a folder
// holding drums/drums.wav, that recording is used instead; --synthetic always uses the synthetic
// pattern (the published image is rendered this way). editorScale sizes the window (0.75 .. 2, as a user would); pixelScale
// is the render resolution on top of that.

#include "../Source/PluginEditor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{
    constexpr double kFallbackRate = 48000.0;

    /** Seconds [start, start + length) of the optional drum recording, or empty when it is not there. */
    juce::AudioBuffer<float> loadDrums (double startSeconds, double lengthSeconds, double& sampleRate)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const auto root = juce::SystemStats::getEnvironmentVariable ("TRANSIENT1_TEST_AUDIO", {});
        if (root.isEmpty())
            return {};
        const auto file = juce::File (root).getChildFile ("drums/drums.wav");
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr || reader->numChannels < 1)
            return {};

        sampleRate = reader->sampleRate;
        const auto first = (juce::int64) (startSeconds * sampleRate);
        const int length = (int) juce::jmin ((juce::int64) (lengthSeconds * sampleRate), reader->lengthInSamples - first);
        if (length <= 0)
            return {};

        juce::AudioBuffer<float> audio (2, length);
        reader->read (&audio, 0, length, first, true, true);
        if (reader->numChannels == 1)
            audio.copyFrom (1, 0, audio, 0, 0, length);
        return audio;
    }

    /** A kick every 0.5 s and a snare on the off-beats, each a decaying burst. */
    juce::AudioBuffer<float> synthetic (double sampleRate, double lengthSeconds)
    {
        const int length = (int) (sampleRate * lengthSeconds);
        juce::AudioBuffer<float> audio (2, length);
        audio.clear();
        juce::Random rng (7);
        for (int n = 0; n < length; ++n)
        {
            const double t = (double) n / sampleRate;
            const double inBeat = std::fmod (t, 0.5);
            const bool snare = std::fmod (t, 1.0) >= 0.5;
            const double kick = snare ? 0.0 : std::sin (juce::MathConstants<double>::twoPi * (50.0 + 90.0 * std::exp (-inBeat * 40.0)) * inBeat) * std::exp (-inBeat * 9.0);
            const double hit = snare ? (rng.nextFloat() * 2.0 - 1.0) * std::exp (-inBeat * 14.0) * 0.5 : kick * 0.8;
            audio.setSample (0, n, (float) hit);
            audio.setSample (1, n, (float) hit);
        }
        return audio;
    }
}

int main (int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI gui;
    std::vector<juce::String> args;
    bool forceSynthetic = false;
    for (int i = 1; i < argc; ++i)
    {
        if (juce::String (argv[i]) == "--synthetic")
            forceSynthetic = true;
        else
            args.emplace_back (argv[i]);
    }
    const auto arg = [&] (size_t i) { return i < args.size() ? args[i] : juce::String(); };
    const auto output = juce::File::getCurrentWorkingDirectory().getChildFile (args.empty() ? "editor.png" : args[0]);
    const double editorScale = juce::jlimit (0.75, 2.0, args.size() > 1 ? arg (1).getDoubleValue() : 1.0);
    const float pixelScale = juce::jlimit (0.25f, 4.0f, args.size() > 2 ? arg (2).getFloatValue() : 2.0f);
    const double start = args.size() > 3 ? arg (3).getDoubleValue() : 6.0;
    constexpr int block = 512;

    double fs = kFallbackRate;
    auto audio = forceSynthetic ? juce::AudioBuffer<float>() : loadDrums (start, 6.0, fs);
    if (audio.getNumSamples() == 0)
        audio = synthetic (fs, 6.0);

    std::unique_ptr<juce::AudioProcessor> processor (createPluginFilter());
    auto& transientProcessor = dynamic_cast<Transient1Processor&> (*processor);
    processor->setPlayConfigDetails (2, 2, fs, block);
    processor->prepareToPlay (fs, block);

    const auto set = [&] (const char* id, float value)
    {
        auto* p = transientProcessor.apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (value));
    };
    set (ParamID::attack, 6.0f);
    set (ParamID::sustain, -4.0f);

    Transient1Editor editor (transientProcessor);
    editor.setSize (juce::roundToInt (Transient1Surface::kWidth * editorScale), juce::roundToInt (Transient1Surface::kHeight * editorScale));

    juce::MidiBuffer midi;
    for (int pos = 0; pos + block <= audio.getNumSamples(); pos += block)
    {
        juce::AudioBuffer<float> chunk (audio.getArrayOfWritePointers(), 2, pos, block);
        processor->processBlock (chunk, midi);
        editor.pumpTelemetry();   // keeps the telemetry FIFO from filling during the faster-than-real-time run
    }

    const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, pixelScale);
    output.deleteFile();
    juce::FileOutputStream out (output);
    return juce::PNGImageFormat().writeImageToStream (image, out) ? 0 : 1;
}
