#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
enum { kRumbleOn, kRumble, kHum, kMono, kDeclick, kCrackle, kDeclip, kCeiling, kDenoise, kThresh, kMode, kLearn, kHfOn, kHfFreq, kHfAmt, kListen, kOut };
const char* ids[] = { "rumbleOn", "rumble", "hum", "mono", "declick", "crackle", "declip", "ceiling", "denoise", "thresh", "noiseMode", "learn", "hfOn", "hfFreq", "hfAmt", "listen", "output" };
constexpr int numP = 17;
}

juce::AudioProcessorValueTreeState::ParameterLayout VegaRestoreProcessor::layout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> v;
    auto pct = [] (float x, int) { return x < 0.5f ? String ("Off") : String (roundToInt (x)) + "%"; };
    auto hz = [] (float x, int) { return x >= 1000 ? String (x / 1000.0f, 1) + " kHz" : String (roundToInt (x)) + " Hz"; };
    auto F = [&] (int id, const char* name, NormalisableRange<float> r, float def, std::function<String (float, int)> f)
    { v.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ids[id], 1 }, name, r, def, AudioParameterFloatAttributes().withStringFromValueFunction (f))); };
    auto B = [&] (int id, const char* name, bool def) { v.push_back (std::make_unique<AudioParameterBool> (ParameterID { ids[id], 1 }, name, def)); };
    B (kRumbleOn, "Rumble Filter", true);
    F (kRumble, "Rumble Freq", NormalisableRange<float> (10, 120, 0, 0.5f), 25, hz);
    v.push_back (std::make_unique<AudioParameterChoice> (ParameterID { ids[kHum], 1 }, "Hum", StringArray { "Off", "50 Hz", "60 Hz" }, 0));
    B (kMono, "Mono", false);
    F (kDeclick, "De-click", NormalisableRange<float> (0, 100), 50, pct);
    F (kCrackle, "De-crackle", NormalisableRange<float> (0, 100), 30, pct);
    B (kDeclip, "De-clip", false);
    F (kCeiling, "Clip Level", NormalisableRange<float> (-12, 0), -0.3f, [] (float x, int) { return String (x, 1) + " dB"; });
    F (kDenoise, "De-hiss", NormalisableRange<float> (0, 30), 6, [] (float x, int) { return x < 0.1f ? String ("Off") : "-" + String (x, 1) + " dB"; });
    F (kThresh, "Threshold", NormalisableRange<float> (-10, 10), 0, [] (float x, int) { return (x > 0 ? "+" : "") + String (x, 1) + " dB"; });
    v.push_back (std::make_unique<AudioParameterChoice> (ParameterID { ids[kMode], 1 }, "Noise Print", StringArray { "Adaptive", "Learned" }, 0));
    B (kLearn, "Learn Noise", false);
    B (kHfOn, "MP3 Rebuild", false);
    F (kHfFreq, "Rebuild From", NormalisableRange<float> (6000, 20000, 0, 0.7f), 16000, hz);
    F (kHfAmt, "Rebuild Amount", NormalisableRange<float> (0, 100), 40, pct);
    B (kListen, "Listen Removed", false);
    F (kOut, "Output", NormalisableRange<float> (-24, 12), 0, [] (float x, int) { return String (x, 1) + " dB"; });
    return { v.begin(), v.end() };
}

VegaRestoreProcessor::VegaRestoreProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true).withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", layout())
{
    for (int i = 0; i < numP; ++i) p[i] = apvts.getRawParameterValue (ids[i]);
    setLatencySamples (latency);
    for (int i = 0; i < specPts; ++i) { uiIn[(size_t) i] = -140.0f; uiOut[(size_t) i] = -140.0f; uiNoise[(size_t) i] = -140.0f; }
}

bool VegaRestoreProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto o = l.getMainOutputChannelSet();
    return o == juce::AudioChannelSet::stereo() && (l.getMainInputChannelSet() == o || l.getMainInputChannelSet() == juce::AudioChannelSet::mono());
}

void VegaRestoreProcessor::prepareToPlay (double s, int)
{
    sr = s; pos = 0;
    for (int c = 0; c < 2; ++c)
    {
        imp[c].reset(); spec[c].reset(); hf[c].reset();
        for (auto& f : rumble[c]) f.reset();
        for (auto& f : hum[c]) f.reset();
        std::fill (inHop[c], inHop[c] + restore::H, 0.0f); std::fill (outHop[c], outHop[c] + restore::H, 0.0f);
        std::fill (dry[c], dry[c] + restore::H * 6, 0.0f);
    }
    lastRumble = lastHum = lastHf = -1;
    updateFilters();
    setLatencySamples (latency);
}

void VegaRestoreProcessor::updateFilters()
{
    using C = juce::dsp::IIR::Coefficients<float>;
    const float rf = p[kRumble]->load();
    if (rf != lastRumble)
    {
        auto a = C::makeHighPass (sr, rf, 0.541f), b = C::makeHighPass (sr, rf, 1.307f);
        for (int c = 0; c < 2; ++c) { rumble[c][0].coefficients = a; rumble[c][1].coefficients = b; }
        lastRumble = rf;
    }
    const float h = p[kHum]->load();
    if (h != lastHum && h > 0.5f)
    {
        const float f0 = h < 1.5f ? 50.0f : 60.0f;
        for (int k = 0; k < 5; ++k)
        {
            auto co = C::makeNotch (sr, f0 * (k + 1), 18.0f + 6.0f * k);
            for (int c = 0; c < 2; ++c) hum[c][k].coefficients = co;
        }
    }
    lastHum = h;
    const float hfF = p[kHfFreq]->load();
    if (hfF != lastHf) { for (auto& x : hf) x.prepare (sr, hfF); lastHf = hfF; }
}

void VegaRestoreProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals nd;
    const int n = buffer.getNumSamples();
    if (getTotalNumInputChannels() == 1 && buffer.getNumChannels() > 1) buffer.copyFrom (1, 0, buffer, 0, 0, n);
    float* ch[2] = { buffer.getWritePointer (0), buffer.getWritePointer (std::min (1, buffer.getNumChannels() - 1)) };
    float pkL = 0, pkR = 0;
    for (int i = 0; i < n; ++i)
    {
        for (int c = 0; c < 2; ++c) { inHop[c][pos] = ch[c][i]; }
        const float yl = outHop[0][pos], yr = outHop[1][pos];
        ch[0][i] = yl; if (ch[1] != ch[0]) ch[1][i] = yr;
        pkL = std::max (pkL, std::abs (yl)); pkR = std::max (pkR, std::abs (yr));
        if (++pos == restore::H) { pos = 0; processHop(); }
    }
    outL.store (std::max (outL.load(), pkL)); outR.store (std::max (outR.load(), pkR));
}

void VegaRestoreProcessor::processHop()
{
    using namespace restore;
    updateFilters();
    if (resetCountersFlag.exchange (false)) for (auto& s : imp) s.clicks = s.crackles = s.clips = 0;
    const bool rumbleOn = p[kRumbleOn]->load() > 0.5f, humOn = p[kHum]->load() > 0.5f, mono = p[kMono]->load() > 0.5f;
    const float clickS = p[kDeclick]->load() * 0.01f, crackS = p[kCrackle]->load() * 0.01f;
    const bool declip = p[kDeclip]->load() > 0.5f;
    const float ceiling = juce::Decibels::decibelsToGain (p[kCeiling]->load());
    const float red = p[kDenoise]->load();
    const float floorGain = red < 0.1f ? 1.0f : juce::Decibels::decibelsToGain (-red);
    const float thrF = std::pow (10.0f, p[kThresh]->load() / 10.0f);
    const bool learn = p[kLearn]->load() > 0.5f, useLearned = p[kMode]->load() > 0.5f;
    const bool hfOn = p[kHfOn]->load() > 0.5f; const float hfAmt = p[kHfAmt]->load() * 0.01f;
    const bool listen = p[kListen]->load() > 0.5f;
    const float outG = juce::Decibels::decibelsToGain (p[kOut]->load());

    if (learn && ! wasLearning) { std::fill (learnAcc.begin(), learnAcc.end(), 0.0f); learnFrames = 0; }
    wasLearning = learn;

    if (mono) for (int i = 0; i < H; ++i) { const float m = 0.5f * (inHop[0][i] + inHop[1][i]); inHop[0][i] = inHop[1][i] = m; }

    float a[H], b[H];
    for (int c = 0; c < 2; ++c)
    {
        std::memmove (dry[c], dry[c] + H, sizeof (float) * 5 * H);
        std::memcpy (dry[c] + 5 * H, inHop[c], sizeof (float) * H);
        for (int i = 0; i < H; ++i)
        {
            float x = inHop[c][i];
            if (rumbleOn) x = rumble[c][1].processSample (rumble[c][0].processSample (x));
            if (humOn) for (auto& f : hum[c]) x = f.processSample (x);
            a[i] = x;
        }
        imp[c].process (a, b, clickS, crackS, declip, ceiling, sr);
        spec[c].process (b, a, floorGain, thrF, useLearned && hasProfile && ! learn ? profile.data() : nullptr);
        if (learn) { for (int k = 0; k < bins; ++k) learnAcc[(size_t) k] += spec[c].power[k]; }
        for (int i = 0; i < H; ++i)
        {
            float y = hfOn ? hf[c].process (a[i], hfAmt) : a[i];
            if (listen) y = dry[c][i] - y;
            outHop[c][i] = y * outG;
        }
    }
    if (learn)
    {
        ++learnFrames;
        for (int k = 0; k < bins; ++k) profile[(size_t) k] = learnAcc[(size_t) k] / (2.0f * learnFrames);
        hasProfile = true;
    }

    uiClicks = imp[0].clicks + imp[1].clicks; uiCrackles = imp[0].crackles + imp[1].crackles; uiClips = imp[0].clips + imp[1].clips;
    uiLearning = learn; uiHasProfile = hasProfile;
    if ((++hopCount & 3) == 0)
    {
        const float binHz = (float) sr / N, norm = 4.0f / ((float) N * N);
        const bool learnedNow = useLearned && hasProfile && ! learn;
        for (int i = 0; i < specPts; ++i)
        {
            const float f0 = specFreq (i) / 1.06f, f1 = specFreq (i) * 1.06f;
            int k0 = std::max (1, (int) (f0 / binHz)), k1 = std::min (bins - 1, std::max (k0, (int) (f1 / binHz)));
            float mi = 0, mo = 0, mn = 0;
            for (int k = k0; k <= k1; ++k)
            {
                mi = std::max (mi, spec[0].power[k]); mo = std::max (mo, spec[0].outPow[k]);
                mn = std::max (mn, learnedNow ? profile[(size_t) k] : spec[0].noise[k]);
            }
            auto db = [norm] (float pw) { return 10.0f * std::log10 (pw * norm + 1.0e-14f); };
            uiIn[(size_t) i] = 0.7f * uiIn[(size_t) i] + 0.3f * db (mi);
            uiOut[(size_t) i] = 0.7f * uiOut[(size_t) i] + 0.3f * db (mo);
            uiNoise[(size_t) i] = db (mn);
        }
    }
}

void VegaRestoreProcessor::getStateInformation (juce::MemoryBlock& d)
{
    auto st = apvts.copyState();
    std::unique_ptr<juce::XmlElement> x (st.createXml());
    if (hasProfile)
    {
        juce::MemoryBlock mb (profile.data(), profile.size() * sizeof (float));
        x->setAttribute ("noiseProfile", mb.toBase64Encoding());
    }
    x->setAttribute ("uiScale", (double) uiScale.load());
    copyXmlToBinary (*x, d);
}

void VegaRestoreProcessor::setStateInformation (const void* data, int size)
{
    std::unique_ptr<juce::XmlElement> x (getXmlFromBinary (data, size));
    if (x == nullptr || ! x->hasTagName (apvts.state.getType())) return;
    uiScale = (float) x->getDoubleAttribute ("uiScale", 1.0);
    juce::MemoryBlock mb;
    if (mb.fromBase64Encoding (x->getStringAttribute ("noiseProfile")) && mb.getSize() == profile.size() * sizeof (float))
    {
        std::memcpy (profile.data(), mb.getData(), mb.getSize()); hasProfile = true;
    }
    apvts.replaceState (juce::ValueTree::fromXml (*x));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VegaRestoreProcessor(); }
