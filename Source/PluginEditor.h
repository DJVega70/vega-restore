#pragma once
#include "PluginProcessor.h"
#include "VegaUI.h"

class VegaRestoreEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit VegaRestoreEditor (VegaRestoreProcessor&);
    ~VegaRestoreEditor() override;
    void paint (juce::Graphics&) override {}
    void resized() override;
    static constexpr int W = 1060, H = 560;
    class Main;
private:
    void timerCallback() override;
    VegaRestoreProcessor& proc;
    VegaLookAndFeel lnf;
    std::unique_ptr<Main> main;
    juce::ComponentBoundsConstrainer constrainer;
};
