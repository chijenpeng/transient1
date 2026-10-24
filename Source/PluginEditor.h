#pragma once

#include "PluginProcessor.h"

#include <array>

/** The embedded typefaces. Shared by all open editors and released with the last one, so
    nothing outlives the host's JUCE shutdown. */
struct Transient1Fonts
{
    Transient1Fonts();
    juce::Typeface::Ptr inter, mono;
};

/** Flat drawing for the two rotary knobs; everything else paints itself. */
class Transient1LookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float position,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;
};

/** A flat disc with a dot, its name above and its value below. Double-click the disc for the
    default; double-click the value to type one. */
class Knob final : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState&, const char* paramId, const juce::String& name, const juce::String& help,
          int diameter, float sweepDegrees = 270.0f);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    static constexpr int kLabelHeight = 16, kGap = 8, kCellWidth = 104;
    /** Label, disc and value. */
    int preferredHeight() const noexcept { return 2 * (kLabelHeight + kGap) + diameter; }

private:
    void finishEditing (bool apply);

    juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::String name;
    int diameter;
    juce::TextEditor editor;
    juce::SliderParameterAttachment attachment;
};

/** The last two seconds of applied gain, drawn from the engine's telemetry frames.
    Vertical axis: dB with the knobs' symmetric 0.5 skew, so +-6 dB is half way to +-24. */
class GainTrace final : public juce::Component
{
public:
    explicit GainTrace (const juce::String& help);

    /** Takes one frame (oldest first). */
    void push (const transient::TelemetryFrame&) noexcept;
    /** Called once per tick after the frames: repaints when the picture changed. */
    void endTick (bool changed);
    /** Fills the whole history with neutral frames (no gain, no input). */
    void clear() noexcept;

    void paint (juce::Graphics&) override;

    static constexpr int kFrames = 800;          // 2 s at about 400 frames per second
    static constexpr float kFramesPerSecond = 400.0f;

private:
    const transient::TelemetryFrame& at (int age) const noexcept { return frames[(size_t) ((head + age) % kFrames)]; }

    std::array<transient::TelemetryFrame, kFrames> frames;
    int head = 0;                                // the oldest frame; the newest is head - 1
    juce::Path shape, edge;                      // reused every paint
};

/** The fixed-size design surface; the editor scales it to its window. */
class Transient1Surface final : public juce::Component,
                                private juce::Timer
{
public:
    explicit Transient1Surface (Transient1Processor&);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** What the control under the mouse does; nullptr shows the default text. */
    void showHelpFor (juce::Component*);
    /** Pops the pending telemetry into the trace (the timer does this; the snapshot tool calls it directly). */
    void pumpTelemetry (bool allowIdleFlatten);

    static constexpr int kWidth = 560, kHeight = 428;

private:
    void timerCallback() override;

    Transient1Processor& processor;
    juce::SharedResourcePointer<Transient1Fonts> fonts;

    GainTrace trace;
    Knob attack, sustain;

    juce::Rectangle<int> helpArea;
    juce::String helpTitle, helpText;
    double lastFrameMs = 0.0, lastTickMs = 0.0, idleCarry = 0.0;
    int tick = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Transient1Surface)
};

class Transient1Editor final : public juce::AudioProcessorEditor
{
public:
    explicit Transient1Editor (Transient1Processor&);
    ~Transient1Editor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** For the snapshot tool: reads the telemetry now, without the idle fade. */
    void pumpTelemetry() { surface.pumpTelemetry (false); }

private:
    Transient1Processor& owner;
    juce::SharedResourcePointer<Transient1Fonts> fonts;
    Transient1LookAndFeel lookAndFeel;
    Transient1Surface surface;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Transient1Editor)
};
