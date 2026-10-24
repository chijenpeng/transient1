#include "PluginEditor.h"

#include <BinaryData.h>

namespace
{
    // Ultraviolet: a near-black violet ground, panels one layer up, white type. Pale lavender is
    // what you control (knob faces, the plug-in's name); violet is the gain being applied (the
    // Attack trace). Contrast against the background: text 19.2, label 8.2, pale 14.9, accent 6.0
    // (5.4 on the panel). The palette is a choice for this plug-in, not hardware.
    namespace Theme
    {
        const juce::Colour background { 0xff110d17 }, panel { 0xff1d1827 }, text { 0xffffffff },
                           label { 0xffaea6bd }, accent { 0xffb26dff }, pale { 0xffe9defc },
                           field { 0xff3a3149 };

        juce::Font make (const juce::Typeface::Ptr& typeface, float height, float tracking)
        {
            return juce::Font (juce::FontOptions (typeface).withHeight (height)).withExtraKerningFactor (tracking);
        }

        /** Labels are upper-case and tracked out; pass upper-case text. */
        juce::Font labelFont (float height = 11.0f)  { return make (juce::SharedResourcePointer<Transient1Fonts>()->inter, height, 0.09f); }
        juce::Font titleFont (float height)          { return make (juce::SharedResourcePointer<Transient1Fonts>()->inter, height, 0.18f); }
        juce::Font bodyFont (float height = 12.0f)   { return make (juce::SharedResourcePointer<Transient1Fonts>()->inter, height, 0.01f); }
        juce::Font valueFont (float height = 12.0f)  { return make (juce::SharedResourcePointer<Transient1Fonts>()->mono, height, 0.0f); }
    }

    constexpr int kMargin = 24, kHeaderHeight = 50, kTraceHeight = 168, kTraceGap = 20, kKnobGap = 40;

    const juce::Identifier helpId ("help"), helpTitleId ("helpTitle"), editorScaleId ("editorScale");

    constexpr auto kPluginName = "TRANSIENT1";
    constexpr auto kDefaultHelpTitle = kPluginName;
    constexpr auto kDefaultHelp = "Shapes the start of each hit (Attack) and the decay after it (Sustain), in dB, at any input level. "
                                  "Hover over a control to see what it does; double-click a knob for its default, or its value to type one.";
    constexpr auto kAttackHelp = "Gain on the onset of each hit. The number is the most it ever applies: a clear hit gets all of it, a small "
                                 "onset proportionally less, at any input level. Positive sharpens, negative softens. Works 1 ms ahead; the host compensates.";
    constexpr auto kSustainHelp = "Gain on the decay and the held part of a note. Held material gets 12 % of the knob, a real decay up to all "
                                  "of it. Negative tightens tails, positive lengthens them. Output can exceed 0 dBFS: there is no limiter.";
    constexpr auto kTraceHelp = "The gain applied over the last 2 seconds, in dB, on a scale that opens up around 0. Violet is Attack, the pale "
                                "line is Sustain, the faint shape behind is the input level (it has no axis). Flat at 0 means nothing applied.";

    void setHelp (juce::Component& c, const juce::String& title, const juce::String& help)
    {
        c.getProperties().set (helpTitleId, title.toUpperCase());
        c.getProperties().set (helpId, help);
    }

    /** The knobs' own law (symmetric skew 0.5, +-24 dB): -1..+1 for -24..+24 dB. */
    float skewed (float db) noexcept
    {
        const float s = std::sqrt (juce::jmin (std::abs (db), 24.0f) / 24.0f);
        return db < 0.0f ? -s : s;
    }
}

//==============================================================================
Transient1Fonts::Transient1Fonts()
    : inter (juce::Typeface::createSystemTypefaceFor (BinaryData::InterSemiBold_ttf, BinaryData::InterSemiBold_ttfSize)),
      mono (juce::Typeface::createSystemTypefaceFor (BinaryData::JetBrainsMonoMedium_ttf, BinaryData::JetBrainsMonoMedium_ttfSize))
{
}

//==============================================================================
void Transient1LookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float position,
                                              float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto centre = bounds.getCentre();
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 1.0f;
    const float angle = startAngle + position * (endAngle - startAngle);
    const float dot = juce::jmax (3.0f, radius * 0.11f);

    g.setColour (slider.isMouseOverOrDragging() ? Theme::text : Theme::pale);
    g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre));

    g.setColour (Theme::background);
    g.fillEllipse (juce::Rectangle<float> (dot * 2.0f, dot * 2.0f).withCentre (centre.getPointOnCircumference (radius * 0.72f, angle)));
}

void Transient1LookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool, bool)
{
    g.setColour (Theme::label.withAlpha (0.35f));
    for (float i = 0.0f; i < 2.0f; ++i)
        g.drawLine ((float) w * (0.45f + i * 0.25f), (float) h, (float) w, (float) h * (0.45f + i * 0.25f), 1.0f);
}

//==============================================================================
Knob::Knob (juce::AudioProcessorValueTreeState& state, const char* paramId, const juce::String& nameToUse, const juce::String& help,
            int diameterToUse, float sweepDegrees)
    : name (nameToUse.toUpperCase()), diameter (diameterToUse), attachment (*state.getParameter (paramId), slider)
{
    const float halfSweep = juce::degreesToRadians (sweepDegrees * 0.5f), twoPi = juce::MathConstants<float>::twoPi;
    slider.setRotaryParameters (twoPi - halfSweep, twoPi + halfSweep, true);
    slider.setVelocityModeParameters (0.15, 1, 0.0, true, juce::ModifierKeys::shiftModifier);   // Shift: fine
    slider.setDoubleClickReturnValue (true, 0.0);
    slider.onValueChange = [this] { repaint(); };
    addAndMakeVisible (slider);

    editor.setJustification (juce::Justification::centred);
    editor.setFont (Theme::valueFont (12.0f));
    editor.setIndents (0, 0);
    editor.setColour (juce::TextEditor::backgroundColourId, Theme::field);
    editor.setColour (juce::TextEditor::textColourId, Theme::pale);
    editor.setColour (juce::TextEditor::highlightColourId, Theme::accent.withAlpha (0.5f));
    editor.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    editor.onReturnKey = [this] { finishEditing (true); };
    editor.onEscapeKey = [this] { finishEditing (false); };
    editor.onFocusLost = [this] { finishEditing (true); };
    addChildComponent (editor);

    setHelp (*this, name, help);
}

void Knob::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop (kLabelHeight + kGap - 1);
    slider.setBounds (area.removeFromTop (diameter + 2).withSizeKeepingCentre (diameter + 2, diameter + 2));
    editor.setBounds (getLocalBounds().removeFromBottom (kLabelHeight).withSizeKeepingCentre (72, kLabelHeight));
}

void Knob::paint (juce::Graphics& g)
{
    auto area = getLocalBounds();

    g.setColour (Theme::label);
    g.setFont (Theme::labelFont());
    g.drawText (name, area.removeFromTop (kLabelHeight), juce::Justification::centred, false);

    g.setFont (Theme::valueFont (12.0f));
    g.drawText (slider.getTextFromValue (slider.getValue()), area.removeFromBottom (kLabelHeight), juce::Justification::centred, false);
}

void Knob::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.y < getHeight() - kLabelHeight - 2)
        return;

    editor.setText (slider.getTextFromValue (slider.getValue()), false);
    editor.setVisible (true);
    editor.selectAll();
    editor.grabKeyboardFocus();
}

void Knob::finishEditing (bool apply)
{
    if (! editor.isVisible())
        return;

    editor.setVisible (false);
    const auto text = editor.getText().trim().toLowerCase();

    // Only text that names a value ("-3", "+6 dB", "4.5"); anything else is ignored.
    if (apply && text.containsAnyOf ("0123456789"))
        slider.setValue (slider.getValueFromText (text), juce::sendNotificationSync);
    repaint();
}

//==============================================================================
GainTrace::GainTrace (const juce::String& help)
{
    setHelp (*this, "Applied gain", help);
    shape.preallocateSpace (3 * (kFrames * 2 + 8));
    edge.preallocateSpace (3 * (kFrames + 4));
}

void GainTrace::clear() noexcept
{
    frames.fill ({});
    head = 0;
}

void GainTrace::push (const transient::TelemetryFrame& f) noexcept
{
    frames[(size_t) head] = f;       // overwrite the oldest, which becomes the newest
    head = (head + 1) % kFrames;
}

void GainTrace::endTick (bool changed)
{
    if (changed)
        repaint();
}

void GainTrace::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (Theme::panel);
    g.fillRect (bounds);

    constexpr float axisWidth = 30.0f;
    const auto plot = bounds.withTrimmedRight (axisWidth).reduced (0.0f, 8.0f);
    const float cy = plot.getCentreY(), half = plot.getHeight() * 0.5f;
    const auto yFor = [&] (float db) { return cy - skewed (db) * half; };
    const auto xFor = [&] (int age) { return plot.getX() + plot.getWidth() * (float) age / (float) (kFrames - 1); };

    // Grid and axis.
    g.setFont (Theme::valueFont (9.0f));
    for (const float db : { 24.0f, 12.0f, 6.0f, 3.0f, 0.0f, -3.0f, -6.0f, -12.0f, -24.0f })
    {
        const float y = yFor (db);
        g.setColour (Theme::label.withAlpha (db == 0.0f ? 0.4f : 0.15f));
        g.fillRect (plot.getX(), y - 0.5f, plot.getWidth(), 1.0f);
        g.setColour (Theme::label.withAlpha (0.8f));
        const auto text = db == 0.0f ? juce::String ("0") : (db > 0.0f ? "+" : "") + juce::String ((int) db);
        g.drawText (text, juce::Rectangle<float> (plot.getRight() + 6.0f, y - 6.0f, axisWidth - 8.0f, 12.0f), juce::Justification::centredLeft);
    }

    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (plot.getSmallestIntegerContainer());

    // Input level, mirrored about the 0 line (-60..0 dBFS over the full half height).
    const auto levelHalf = [&] (int age) { return juce::jlimit (0.0f, 1.0f, (at (age).inputPeakDb + 60.0f) / 60.0f) * half * 0.94f; };
    shape.clear();
    shape.startNewSubPath (xFor (0), cy - levelHalf (0));
    for (int i = 1; i < kFrames; ++i)
        shape.lineTo (xFor (i), cy - levelHalf (i));
    for (int i = kFrames - 1; i >= 0; --i)
        shape.lineTo (xFor (i), cy + levelHalf (i));
    shape.closeSubPath();
    g.setColour (Theme::label.withAlpha (0.17f));
    g.fillPath (shape);

    // Attack: a filled area from the 0 line with a 1.5 px edge.
    shape.clear();
    edge.clear();
    shape.startNewSubPath (xFor (0), cy);
    for (int i = 0; i < kFrames; ++i)
    {
        const float x = xFor (i), y = yFor (at (i).attackGainDb);
        shape.lineTo (x, y);
        if (i == 0) edge.startNewSubPath (x, y); else edge.lineTo (x, y);
    }
    shape.lineTo (xFor (kFrames - 1), cy);
    shape.closeSubPath();
    g.setColour (Theme::accent.withAlpha (0.4f));
    g.fillPath (shape);
    g.setColour (Theme::accent);
    g.strokePath (edge, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved));

    // Sustain: a pale line.
    edge.clear();
    for (int i = 0; i < kFrames; ++i)
    {
        const float x = xFor (i), y = yFor (at (i).sustainGainDb);
        if (i == 0) edge.startNewSubPath (x, y); else edge.lineTo (x, y);
    }
    g.setColour (Theme::pale);
    g.strokePath (edge, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved));
}

//==============================================================================
Transient1Surface::Transient1Surface (Transient1Processor& p)
    : processor (p),
      trace (kTraceHelp),
      attack (p.apvts, ParamID::attack, "Attack", kAttackHelp, 64),
      sustain (p.apvts, ParamID::sustain, "Sustain", kSustainHelp, 64)
{
    addAndMakeVisible (trace);
    addAndMakeVisible (attack);
    addAndMakeVisible (sustain);

    helpTitle = kDefaultHelpTitle;
    helpText = kDefaultHelp;
    lastFrameMs = lastTickMs = juce::Time::getMillisecondCounterHiRes();
    setSize (kWidth, kHeight);
    startTimerHz (60);
}

void Transient1Surface::showHelpFor (juce::Component* component)
{
    juce::String title = kDefaultHelpTitle, text = kDefaultHelp;

    // The help belongs to the nearest ancestor that has any (a knob's slider has none itself).
    for (auto* c = component; c != nullptr && c != this; c = c->getParentComponent())
    {
        if (c->getProperties().contains (helpId))
        {
            title = c->getProperties()[helpTitleId].toString();
            text = c->getProperties()[helpId].toString();
            break;
        }
    }

    if (title != helpTitle || text != helpText)
    {
        helpTitle = title;
        helpText = text;
        repaint (helpArea);
    }
}

void Transient1Surface::pumpTelemetry (bool allowIdleFlatten)
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    int popped = 0;
    transient::TelemetryFrame frame;
    while (processor.popTelemetry (frame))
    {
        trace.push (frame);
        ++popped;
    }

    int injected = 0;
    if (popped > 0)
    {
        lastFrameMs = now;
        idleCarry = 0.0;
    }
    else if (allowIdleFlatten && now - lastFrameMs > 250.0 && now - lastFrameMs < 2600.0)
    {
        // No audio is running: let the picture drain to a flat line at the engine's own rate.
        idleCarry += (now - lastTickMs) * 0.001 * (double) GainTrace::kFramesPerSecond;
        injected = juce::jmin ((int) idleCarry, GainTrace::kFrames);
        idleCarry -= injected;
        for (int i = 0; i < injected; ++i)
            trace.push ({});
    }

    lastTickMs = now;
    trace.endTick (popped > 0 || injected > 0);
}

void Transient1Surface::timerCallback()
{
    pumpTelemetry (true);

    if ((++tick & 1) == 0)   // help follows the mouse at 30 Hz
    {
        auto* under = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();
        showHelpFor (under != nullptr && isParentOf (under) ? under : nullptr);
    }
}

void Transient1Surface::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);

    g.setColour (Theme::pale);
    g.setFont (Theme::titleFont (15.0f));
    g.drawSingleLineText (kPluginName, kMargin, 33);

    // Help, bottom: what the control under the mouse does.
    auto help = helpArea;
    g.setColour (Theme::pale);
    g.setFont (Theme::labelFont());
    g.drawText (helpTitle, help.removeFromTop (16), juce::Justification::topLeft, false);
    g.setColour (Theme::label);
    g.setFont (Theme::bodyFont());
    g.drawFittedText (helpText, help, juce::Justification::topLeft, 3, 1.0f);
}

void Transient1Surface::resized()
{
    auto area = getLocalBounds().reduced (kMargin, 0);
    area.removeFromTop (kHeaderHeight);

    area.removeFromBottom (10);
    helpArea = area.removeFromBottom (54);
    area.removeFromBottom (14);

    trace.setBounds (area.removeFromTop (kTraceHeight));
    area.removeFromTop (kTraceGap);

    const int total = 2 * Knob::kCellWidth + kKnobGap;
    const int x = area.getCentreX() - total / 2;
    attack.setBounds (x, area.getY(), Knob::kCellWidth, attack.preferredHeight());
    sustain.setBounds (x + Knob::kCellWidth + kKnobGap, area.getY(), Knob::kCellWidth, sustain.preferredHeight());
}

//==============================================================================
Transient1Editor::Transient1Editor (Transient1Processor& p)
    : AudioProcessorEditor (p), owner (p), surface (p)
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (surface);

    // Scales as a whole, 0.75x to 2x, keeping the design's aspect ratio.
    constexpr int w = Transient1Surface::kWidth, h = Transient1Surface::kHeight;
    setResizable (true, true);
    setResizeLimits (w * 3 / 4, h * 3 / 4, w * 2, h * 2);
    getConstrainer()->setFixedAspectRatio ((double) w / (double) h);

    const double scale = juce::jlimit (0.75, 2.0, (double) p.apvts.state.getProperty (editorScaleId, 1.0));
    setSize (juce::roundToInt (w * scale), juce::roundToInt (h * scale));
}

Transient1Editor::~Transient1Editor()
{
    setLookAndFeel (nullptr);
}

void Transient1Editor::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);
}

void Transient1Editor::resized()
{
    const float scale = (float) getWidth() / (float) Transient1Surface::kWidth;
    surface.setTransform (juce::AffineTransform::scale (scale));
    surface.setBounds (0, 0, Transient1Surface::kWidth, Transient1Surface::kHeight);

    // Remember a size the user chose (a window on screen), not the ones a host passes through.
    if (isShowing() && getPeer() != nullptr)
        owner.apvts.state.setProperty (editorScaleId, (double) scale, nullptr);
}
