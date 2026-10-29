// PluginEditor.h — Alpha Sampler editor.
//
// The pt::ui theme (brushed metal / amber glass) is at the top, then the
// reusable cells, one page class per tab, and the editor itself.

#pragma once

#include "PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

// ============================================================================
// pt::ui — the visual language.
// ============================================================================
// ============================================================================
// pt::ui — the visual language (was Theme.h).
// ============================================================================
namespace pt::ui
{
constexpr float phi    = 1.6180339887f;
constexpr float invPhi = 0.6180339887f;

namespace col
{
inline const juce::Colour bg         { 0xff0d0e10 };
inline const juce::Colour metalHi    { 0xff3b3d43 };
inline const juce::Colour metalMid   { 0xff2a2c30 };
inline const juce::Colour metalLo    { 0xff1c1d21 };
inline const juce::Colour edgeLight  { 0xff5d6068 };
inline const juce::Colour edgeDark   { 0xff08090a };
inline const juce::Colour glass      { 0xff090b0f };
inline const juce::Colour glassHi    { 0xff151a23 };
inline const juce::Colour amber      { 0xffffb347 };
inline const juce::Colour amberDim   { 0xff6f4a1c };
inline const juce::Colour amberGhost { 0xff221a0d };
inline const juce::Colour cyan       { 0xff49dad2 };
inline const juce::Colour text       { 0xffdadce1 };
inline const juce::Colour textDim    { 0xff8d919a };
inline const juce::Colour textFaint  { 0xff5a5d66 };
inline const juce::Colour perfect    { 0xff9dffb8 };
inline const juce::Colour good       { 0xffe9f77e };
inline const juce::Colour offDyn     { 0xffff8fd0 };
inline const juce::Colour missed     { 0xff80404a };
inline const juce::Colour wrong      { 0xffff4d5e };
inline const juce::Colour rightHand  = amber;
inline const juce::Colour leftHand   = cyan;
} // namespace col

juce::Font sans (float height, bool bold = false);
juce::Font caps (float height);

class Textures
{
public:
    Textures();
    juce::Image brushed;
};

void brushedMetal (juce::Graphics&, juce::Rectangle<float>, float corner, bool raised, const Textures&);
void glassWell    (juce::Graphics&, juce::Rectangle<float>, float corner);
void glow         (juce::Graphics&, juce::Rectangle<float>, juce::Colour, float spread, float alpha);
void engravedText (juce::Graphics&, const juce::String&, juce::Rectangle<float>, juce::Justification,
                   const juce::Font&, bool lit = false);
void keyPlate     (juce::Graphics&, juce::Rectangle<float>, bool down, bool on, const Textures&);
float sevenSegmentWidthPerHeight (const juce::String&);
void sevenSegment (juce::Graphics&, const juce::String&, juce::Rectangle<float>, juce::Colour on, juce::Colour off, bool ghost = true);

class AlumLookAndFeel : public juce::LookAndFeel_V4
{
public:
    AlumLookAndFeel();

    void drawRotarySlider       (juce::Graphics&, int x, int y, int w, int h, float pos, float startAngle, float endAngle, juce::Slider&) override;
    void drawButtonBackground   (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawButtonText         (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    void drawToggleButton       (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawComboBox           (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    void positionComboBoxText   (juce::ComboBox&, juce::Label&) override;
    juce::Font getComboBoxFont  (juce::ComboBox&) override { return sans (13.f); }
    juce::Font getPopupMenuFont () override               { return sans (14.f); }
    void drawPopupMenuBackground (juce::Graphics&, int w, int h) override;
    void drawPopupMenuItem      (juce::Graphics&, const juce::Rectangle<int>&, bool separator, bool active,
                                 bool highlighted, bool ticked, bool hasSubMenu, const juce::String& text,
                                 const juce::String& shortcut, const juce::Drawable*, const juce::Colour*) override;
    void drawScrollbar          (juce::Graphics&, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                                 int thumbStart, int thumbSize, bool over, bool down) override;
    int  getDefaultScrollbarWidth () override { return 8; }
    void drawLinearSlider       (juce::Graphics&, int x, int y, int w, int h, float pos, float min, float max,
                                 juce::Slider::SliderStyle, juce::Slider&) override;
    juce::Font getLabelFont       (juce::Label&) override         { return sans (13.f); }
    juce::Font getTextButtonFont  (juce::TextButton&, int) override { return caps (11.f); }

    const Textures& textures () const { return *tex; }

private:
    juce::SharedResourcePointer<Textures> tex;
};
} // namespace pt::ui

// ============================================================================
// as — the editor.
// ============================================================================
namespace as {

// ---- meters ----------------------------------------------------------------
class BlockMeter final : public juce::Component
{
public:
    BlockMeter();
    void setValue (float v);
    void paint (juce::Graphics& g) override;

private:
    float value = 0.0f;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BlockMeter)
};

class LevelMeter final : public juce::Component
{
public:
    explicit LevelMeter (juce::Colour accent);
    void setValue (float v);
    void paint (juce::Graphics& g) override;

private:
    juce::Colour accent;
    float value = 0.0f;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeter)
};

class ScopeView final : public juce::Component
{
public:
    ScopeView();
    void pushSample (float env, float peak);   // call from a timer; repaints itself
    void paint (juce::Graphics& g) override;

private:
    std::vector<float> history, peakHistory;
    size_t index = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScopeView)
};

// Watches every parameter for UI gestures (knob drags, combo/toggle changes) so the waveform
// display can show what is being tweaked. Host automation (no gesture) is ignored.
class TweakWatcher final : private juce::AudioProcessorParameter::Listener
{
public:
    explicit TweakWatcher (juce::AudioProcessor&);
    ~TweakWatcher() override;
    juce::AudioProcessorParameter* active() const;   // message thread; nullptr when idle

private:
    void parameterValueChanged (int, float) override;
    void parameterGestureChanged (int, bool) override;

    juce::AudioProcessor& proc;
    std::atomic<int> index { -1 }, held { 0 };
    std::atomic<juce::uint32> stamp { 0 };
};

// The big display under the header. Idle: the current (processed) waveform with loop points and
// start offset. While a control is being tweaked: a picture of that parameter's effect on the
// source waveform (region, fades, filter response, envelope, stretch...). Also the drop target look.
class WaveformView final : public juce::Component
{
public:
    explicit WaveformView (juce::AudioProcessorValueTreeState&);
    void setWave (std::shared_ptr<const WaveView>);
    void setTweak (juce::AudioProcessorParameter*);   // call every tick; repaints only on change
    void setDragHover (bool);
    void setBusy (bool);
    void paint (juce::Graphics&) override;

private:
    void paintIdle  (juce::Graphics&, juce::Rectangle<float> area, juce::Rectangle<float> header);
    void paintTweak (juce::Graphics&, juce::Rectangle<float> area, juce::Rectangle<float> header);
    float norm (const juce::String& id) const;
    float val  (const juce::String& id) const;

    juce::AudioProcessorValueTreeState& apvts;
    std::shared_ptr<const WaveView> wave;
    juce::AudioProcessorParameter* tweak = nullptr;
    float tweakValue = -1.0f;
    bool hover = false, busy = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};

// ---- parameter cells ---------------------------------------------------------
class KnobCell final : public juce::Component
{
public:
    KnobCell();
    void attach (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramID, const juce::String& caption);
    void resized() override;

private:
    juce::Slider knob;
    juce::Label caption, valueLabel;
    BlockMeter block;
    juce::RangedAudioParameter* param = nullptr;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    void updateReadout();
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KnobCell)
};

class ToggleCell final : public juce::Component
{
public:
    ToggleCell();
    void attach (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramID, const juce::String& caption);
    void resized() override;

private:
    juce::ToggleButton toggle;
    juce::Label captionLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToggleCell)
};

// Labelled ComboBox for any AudioParameterChoice (also used for note names).
class ChoiceCell final : public juce::Component
{
public:
    ChoiceCell();
    void attach (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramID, const juce::String& caption);
    void resized() override;

private:
    juce::Label caption;
    juce::ComboBox combo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoiceCell)
};

// ---- pages --------------------------------------------------------------------
// Place components left-to-right, top-to-bottom in a grid; nullptr leaves a gap.
void placeGrid (juce::Rectangle<int> area, int columns, int rowHeight,
                std::initializer_list<juce::Component*> items, int pad = 4);

class SamplePage final : public juce::Component
{
public:
    explicit SamplePage (AlphaSamplerAudioProcessor&);
    void resized() override;
    void updateFileLabel();

private:
    AlphaSamplerAudioProcessor& processor;
    juce::TextButton loadButton { "LOAD" }, clearButton { "CLEAR" };
    juce::Label fileLabel;
    std::unique_ptr<juce::FileChooser> chooser;
    ToggleCell keepDisk, resample, loadRegions, loadSlices;
    ChoiceCell declickMode;
    KnobCell startOffset;
};

class LoopPage final : public juce::Component
{
public:
    explicit LoopPage (juce::AudioProcessorValueTreeState&);
    void resized() override;

private:
    KnobCell loopStart, loopEnd;
    ToggleCell loopUse, loopPingpong;
};

class StretchPage final : public juce::Component
{
public:
    explicit StretchPage (juce::AudioProcessorValueTreeState&);
    void resized() override;

private:
    KnobCell tsPitch, tsMul, tsTime;
    ChoiceCell tsMode;
};

class FxPage final : public juce::Component
{
public:
    explicit FxPage (juce::AudioProcessorValueTreeState&);
    void resized() override;

private:
    ToggleCell dc, normalize, reverse, polarity, fadeStereo, swap, clip, loopFollows;
    KnobCell start, length, in, out, crossfade, trim, boost, eq, rmMix, rmFreq, cut, res, reverb, sdelay, pogo;
    ChoiceCell reverbType;
};

class FilterPage final : public juce::Component
{
public:
    explicit FilterPage (juce::AudioProcessorValueTreeState&);
    void resized() override;

private:
    KnobCell cut, res;
    ChoiceCell type;
};

class MiscPage final : public juce::Component
{
public:
    explicit MiscPage (juce::AudioProcessorValueTreeState&);
    void resized() override;

private:
    ChoiceCell root, zoneLo, zoneHi;
    juce::Label fineLabel;
    juce::Slider fine;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fineAttachment;
    ToggleCell mainPitch, addKey;
};

// One envelope / LFO page (instantiated five times).
class EnvelopePage final : public juce::Component
{
public:
    EnvelopePage (juce::AudioProcessorValueTreeState&, const juce::String& prefix);
    void resized() override;

private:
    ToggleCell on, tempo, lfoTempo, lfoGlobal;
    KnobCell delay, att, hold, dec, sus, rel, attT, decT, relT, amt;
    ChoiceCell lfoShape;
    KnobCell lfoDelay, lfoAtt, lfoAmt, lfoSpeed;
};

class EnvTab final : public juce::Component
{
public:
    explicit EnvTab (juce::AudioProcessorValueTreeState&);
    ~EnvTab() override;
    void resized() override;

private:
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
};

// ---- the editor ---------------------------------------------------------------
class AlphaSamplerAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                               public juce::FileDragAndDropTarget,
                                               private juce::Timer
{
public:
    explicit AlphaSamplerAudioProcessorEditor (AlphaSamplerAudioProcessor&);
    ~AlphaSamplerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Drag & drop a sample file onto the window to load it.
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void timerCallback() override;

    AlphaSamplerAudioProcessor& processor;
    TweakWatcher watcher { processor };
    uint32_t shownWave = 0;
    pt::ui::AlumLookAndFeel lookAndFeel;

    juce::Label title, subtitle, status, scopeLabel, meterLabelOut, meterLabelPeak;
    ScopeView scope;
    WaveformView waveform { processor.apvts() };
    LevelMeter outMeter { pt::ui::col::cyan };
    LevelMeter peakMeter { pt::ui::col::missed.brighter (0.25f) };
    float shown = 0.0f, hold = 0.0f;

    // Pages must outlive `tabs`' use of them: declare them first, tabs last.
    SamplePage samplePage;
    LoopPage loopPage;
    StretchPage stretchPage;
    FxPage fxPage;
    FilterPage filterPage;
    MiscPage miscPage;
    EnvTab envTab;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AlphaSamplerAudioProcessorEditor)
};

} // namespace as
