#pragma once

#include "dsp/TransientShaper.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>

/** Parameter IDs. Stable: never reorder or rename, sessions depend on them. */
namespace ParamID
{
    inline constexpr auto attack = "attack", sustain = "sustain";
}

class Transient1Processor final : public juce::AudioProcessor
{
public:
    Transient1Processor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // For the UI
    juce::AudioProcessorValueTreeState apvts;

    /** Pops one telemetry frame (about 400 per second, aligned with the delayed audio the
        editor hears); false when none is waiting. Call from the message thread only. */
    bool popTelemetry (transient::TelemetryFrame& frame) noexcept { return engine.telemetry().pop (frame); }

    const transient::TransientShaper& getEngine() const noexcept { return engine; }

    static constexpr int kStateVersion = 1;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void run (juce::AudioBuffer<float>&, bool bypassed) noexcept;

    transient::TransientShaper engine;
    std::atomic<float>* attackValue = nullptr;
    std::atomic<float>* sustainValue = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Transient1Processor)
};
