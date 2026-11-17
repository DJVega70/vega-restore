#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "RestoreDSP.h"

class VegaRestoreProcessor : public juce::AudioProcessor
{
public:
    VegaRestoreProcessor();
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void prepareToPlay (double, int) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Vega Productions Restore"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout layout();
    juce::AudioProcessorValueTreeState apvts;
    static constexpr int latency = 6 * restore::H;

    // UI telemetry (channel 1)
    static constexpr int specPts = 160;
    std::array<std::atomic<float>, specPts> uiIn {}, uiOut {}, uiNoise {};
    std::atomic<int> uiClicks { 0 }, uiCrackles { 0 }, uiClips { 0 };
    std::atomic<bool> uiLearning { false }, uiHasProfile { false };
    std::atomic<float> outL { 0 }, outR { 0 }, uiScale { 1.0f };
    void resetCounters() { resetCountersFlag = true; }
    static float specFreq (int i) { return 20.0f * std::pow (1000.0f, (float) i / (specPts - 1)); }

private:
    void processHop();
    void updateFilters();
    double sr = 48000;
    int pos = 0;
    float inHop[2][restore::H] {}, outHop[2][restore::H] {}, dry[2][restore::H * 6] {};
    restore::ImpulseStage imp[2];
    restore::SpectralStage spec[2];
    restore::HighRebuild hf[2];
    juce::dsp::IIR::Filter<float> rumble[2][2], hum[2][5];
    float lastRumble = -1, lastHum = -1, lastHf = -1;
    std::vector<float> profile = std::vector<float> (restore::bins, 0.0f), learnAcc = std::vector<float> (restore::bins, 0.0f);
    int learnFrames = 0; bool wasLearning = false; bool hasProfile = false;
    std::atomic<bool> resetCountersFlag { false };
    int hopCount = 0;
    std::atomic<float>* p[20] {};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VegaRestoreProcessor)
};
