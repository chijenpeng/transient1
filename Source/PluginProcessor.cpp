#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    const juce::Identifier versionId ("version");

    /** The 0.1 dB snapping can leave a zero as 4e-7; the engine must see a true 0 for the bit-exact neutral. */
    double cleanKnob (float v) noexcept { return std::abs (v) < 0.05f ? 0.0 : (double) v; }

    juce::AudioParameterFloatAttributes knobAttributes()
    {
        return juce::AudioParameterFloatAttributes()
            .withLabel ("dB")
            .withStringFromValueFunction ([] (float v, int)
                                         { return v > 0.04f ? "+" + juce::String (v, 1) + " dB" : (v < -0.04f ? juce::String (v, 1) + " dB" : juce::String ("0.0 dB")); })
            .withValueFromStringFunction ([] (const juce::String& text) { return text.getFloatValue(); });
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout Transient1Processor::createLayout()
{
    using namespace transient::voicing;
    using Float = juce::AudioParameterFloat;

    // Symmetric skew 0.5 about 0 dB: +-6 dB sits at half of each side's travel (Custom choice).
    juce::NormalisableRange<float> range (-kKnobRangeDb, kKnobRangeDb, kKnobStepDb, kKnobSkew, true);

    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::attack, 1 }, "Attack", range, 0.0f, knobAttributes()));
    layout.add (std::make_unique<Float> (juce::ParameterID { ParamID::sustain, 1 }, "Sustain", range, 0.0f, knobAttributes()));
    return layout;
}

Transient1Processor::Transient1Processor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Transient1", createLayout())
{
    attackValue = apvts.getRawParameterValue (ParamID::attack);
    sustainValue = apvts.getRawParameterValue (ParamID::sustain);
}

bool Transient1Processor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
           && out == layouts.getMainInputChannelSet();
}

void Transient1Processor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock, juce::jmin (getTotalNumInputChannels(), 2));
    engine.setParameters (cleanKnob (attackValue->load()), cleanKnob (sustainValue->load()));
    engine.reset();
    setLatencySamples (engine.latencySamples());
}

void Transient1Processor::run (juce::AudioBuffer<float>& buffer, bool bypassed) noexcept
{
    juce::ScopedNoDenormals noDenormals;

    const int numChannels = juce::jmin (getTotalNumInputChannels(), getTotalNumOutputChannels(), 2);
    for (int ch = numChannels; ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    engine.setParameters (cleanKnob (attackValue->load()), cleanKnob (sustainValue->load()));
    engine.process (buffer.getArrayOfWritePointers(), numChannels, buffer.getNumSamples(), bypassed);
}

void Transient1Processor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    run (buffer, false);
}

void Transient1Processor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // The audio is still delayed by the reported latency, so bypass stays time-aligned.
    run (buffer, true);
}

void Transient1Processor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty (versionId, kStateVersion, nullptr);
    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void Transient1Processor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;
    auto tree = juce::ValueTree::fromXml (*xml);
    if (static_cast<int> (tree.getProperty (versionId, 1)) > kStateVersion)
    {
        // Written by a newer version: its meaning is unknown, so load the defaults.
        for (const auto* id : { ParamID::attack, ParamID::sustain })
            if (auto* p = apvts.getParameter (id))
                p->setValueNotifyingHost (p->getDefaultValue());
        return;
    }
    for (const auto* id : { ParamID::attack, ParamID::sustain })   // anything the state does not mention is a default
        if (auto* p = apvts.getParameter (id))
            p->setValueNotifyingHost (p->getDefaultValue());
    apvts.replaceState (tree);
}

juce::AudioProcessorEditor* Transient1Processor::createEditor()
{
    return new Transient1Editor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new Transient1Processor();
}
