#include "PluginEditor.h"

namespace
{
class SpectrumView : public juce::Component
{
public:
    explicit SpectrumView (VegaRestoreProcessor& p) : proc (p) {}
    void paint (juce::Graphics& g) override
    {
        vegaui::drawScreen (g, getLocalBounds().toFloat());
        auto r = getLocalBounds().toFloat().reduced (14.0f, 10.0f);
        auto top = r.removeFromTop (20.0f);
        g.setFont (VegaFonts::ui (10.5f, true));
        g.setColour (VegaColours::accent);
        g.drawText ("CLICKS " + juce::String (proc.uiClicks.load()) + "    CRACKLE " + juce::String (proc.uiCrackles.load()) + "    CLIPS " + juce::String (proc.uiClips.load()),
                    top, juce::Justification::centredLeft);
        g.setColour (proc.uiLearning.load() ? VegaColours::gold : VegaColours::textDim);
        const bool learned = proc.apvts.getRawParameterValue ("noiseMode")->load() > 0.5f;
        juce::String np = proc.uiLearning.load() ? "LEARNING NOISE PRINT..." : learned ? (proc.uiHasProfile.load() ? "NOISE PRINT: LEARNED" : "NOISE PRINT: NONE YET (USING ADAPTIVE)") : "NOISE PRINT: ADAPTIVE";
        g.drawText (np, top, juce::Justification::centredRight);

        auto plot = r.withTrimmedBottom (12.0f);
        const float lo = -110.0f, hi = -10.0f;
        auto xOf = [&] (int i) { return plot.getX() + plot.getWidth() * (float) i / (VegaRestoreProcessor::specPts - 1); };
        auto yOf = [&] (float db) { return juce::jmap (juce::jlimit (lo, hi, db), lo, hi, plot.getBottom(), plot.getY()); };
        g.setColour (juce::Colour (0xff262626));
        for (float f : { 50.0f, 100.0f, 500.0f, 1000.0f, 5000.0f, 10000.0f })
        {
            const float x = plot.getX() + plot.getWidth() * std::log (f / 20.0f) / std::log (1000.0f);
            g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            g.setColour (VegaColours::textDim); g.setFont (VegaFonts::ui (9.0f, false));
            g.drawText (f >= 1000 ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f), (int) x - 15, (int) plot.getBottom() + 1, 30, 11, juce::Justification::centred);
            g.setColour (juce::Colour (0xff262626));
        }
        juce::Path in, out, nz;
        for (int i = 0; i < VegaRestoreProcessor::specPts; ++i)
        {
            const float x = xOf (i);
            if (i == 0) { in.startNewSubPath (x, plot.getBottom()); out.startNewSubPath (x, yOf (proc.uiOut[0])); nz.startNewSubPath (x, yOf (proc.uiNoise[0])); }
            in.lineTo (x, yOf (proc.uiIn[(size_t) i]));
            if (i > 0) { out.lineTo (x, yOf (proc.uiOut[(size_t) i])); nz.lineTo (x, yOf (proc.uiNoise[(size_t) i])); }
        }
        in.lineTo (plot.getRight(), plot.getBottom()); in.closeSubPath();
        g.setColour (juce::Colour (0xff5a5a5a).withAlpha (0.55f)); g.fillPath (in);
        g.setColour (VegaColours::gold.withAlpha (0.8f));
        juce::Path dashed; const float dl[] = { 4.0f, 3.0f };
        juce::PathStrokeType (1.2f).createDashedStroke (dashed, nz, dl, 2); g.fillPath (dashed);
        g.setColour (VegaColours::accent); g.strokePath (out, juce::PathStrokeType (1.6f));
        g.setFont (VegaFonts::ui (9.0f, true));
        g.setColour (juce::Colour (0xff8a8a8a)); g.drawText ("IN", (int) plot.getRight() - 120, (int) plot.getY(), 30, 12, juce::Justification::centredRight);
        g.setColour (VegaColours::accent); g.drawText ("OUT", (int) plot.getRight() - 85, (int) plot.getY(), 30, 12, juce::Justification::centredRight);
        g.setColour (VegaColours::gold); g.drawText ("NOISE", (int) plot.getRight() - 45, (int) plot.getY(), 45, 12, juce::Justification::centredRight);
    }
    void mouseDoubleClick (const juce::MouseEvent&) override { proc.resetCounters(); }
private:
    VegaRestoreProcessor& proc;
};
}

class VegaRestoreEditor::Main : public juce::Component
{
public:
    explicit Main (VegaRestoreProcessor& p) : proc (p), a (p.apvts), view (p),
        rumbleOn (a, "rumbleOn", "Rumble"), rumble (a, "rumble", "Below"), hum (a, "hum", "Hum"), mono (a, "mono", "Mono"),
        declick (a, "declick", "DE-CLICK", true), crackle (a, "crackle", "Crackle"), declip (a, "declip", "De-clip"), ceiling (a, "ceiling", "Clip Level"),
        denoise (a, "denoise", "DE-HISS", true), thresh (a, "thresh", "Threshold"), mode (a, "noiseMode", "Noise Print"), learn (a, "learn", "Learn"),
        hfOn (a, "hfOn", "Rebuild"), hfFreq (a, "hfFreq", "From"), hfAmt (a, "hfAmt", "Amount"),
        listen (a, "listen", "Listen Removed"), output (a, "output", "Output")
    {
        for (auto* c : std::initializer_list<juce::Component*> { &view, &rumbleOn, &rumble, &hum, &mono, &declick, &crackle, &declip, &ceiling, &denoise, &thresh, &mode, &learn,
                                                                 &hfOn, &hfFreq, &hfAmt, &listen, &output, &meter })
            addAndMakeVisible (c);
    }
    void refresh() { view.repaint(); meter.setLevels (proc.outL.exchange (0.0f), proc.outR.exchange (0.0f)); }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (VegaColours::background);
        vegaui::drawTopBar (g, W, "RESTORE  VP-24");
        vegaui::drawBody (g, { 0, 35, W, H - 65 });
        vegaui::drawSection (g, { 0, 232, 200, 298 }, "CLEAN-UP", false);
        vegaui::drawSection (g, { 200, 232, 280, 298 }, "CLICKS & CRACKLE");
        vegaui::drawSection (g, { 480, 232, 260, 298 }, "DE-HISS");
        vegaui::drawSection (g, { 740, 232, 160, 298 }, "MP3 REBUILD");
        vegaui::drawSection (g, { 900, 232, W - 900, 298 }, "OUTPUT");
        vegaui::drawBottomBar (g, { 0, H - 30, W, 30 }, "VINYL & OLD MP3 RESTORATION  -  LEARN: PLAY THE QUIET LEAD-IN GROOVE, TURN LEARN ON FOR 2 SECONDS, THEN OFF");
    }
    void resized() override
    {
        view.setBounds (12, 44, 950, 180); meter.setBounds (972, 40, 76, 184);
        rumbleOn.setBounds (12, 250, 70, 80); rumble.setBounds (100, 250, 90, 112); hum.setBounds (12, 380, 176, 46); mono.setBounds (12, 440, 70, 80);
        declick.setBounds (212, 250, 140, 170); crackle.setBounds (370, 250, 90, 112); ceiling.setBounds (370, 380, 90, 112); declip.setBounds (240, 430, 70, 80);
        denoise.setBounds (492, 250, 140, 170); thresh.setBounds (640, 250, 90, 112); learn.setBounds (650, 380, 70, 80); mode.setBounds (492, 440, 140, 46);
        hfOn.setBounds (752, 250, 70, 80); hfAmt.setBounds (826, 250, 70, 112); hfFreq.setBounds (790, 380, 70, 112);
        listen.setBounds (912, 250, 70, 80); output.setBounds (988, 250, 64, 112);
    }
private:
    VegaRestoreProcessor& proc; juce::AudioProcessorValueTreeState& a;
    SpectrumView view;
    vegaui::Toggle rumbleOn; vegaui::Knob rumble; vegaui::Choice hum; vegaui::Toggle mono;
    vegaui::Knob declick, crackle; vegaui::Toggle declip; vegaui::Knob ceiling;
    vegaui::Knob denoise, thresh; vegaui::Choice mode; vegaui::Toggle learn;
    vegaui::Toggle hfOn; vegaui::Knob hfFreq, hfAmt;
    vegaui::Toggle listen; vegaui::Knob output; vegaui::StereoMeter meter;
};

VegaRestoreEditor::VegaRestoreEditor (VegaRestoreProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&lnf);
    main = std::make_unique<Main> (p);
    addAndMakeVisible (*main);
    constrainer.setFixedAspectRatio ((double) W / H);
    constrainer.setSizeLimits (W / 2, H / 2, W * 2, H * 2);
    setConstrainer (&constrainer);
    setResizable (true, true);
    const float s = juce::jlimit (0.5f, 2.0f, proc.uiScale.load());
    setSize (juce::roundToInt (W * s), juce::roundToInt (H * s));
    startTimerHz (30);
}
VegaRestoreEditor::~VegaRestoreEditor() { stopTimer(); setConstrainer (nullptr); main.reset(); setLookAndFeel (nullptr); }
void VegaRestoreEditor::timerCallback() { main->refresh(); }
void VegaRestoreEditor::resized()
{
    main->setBounds (0, 0, W, H);
    const float s = (float) getWidth() / (float) W;
    main->setTransform (juce::AffineTransform::scale (s));
    proc.uiScale.store (s);
}
juce::AudioProcessorEditor* VegaRestoreProcessor::createEditor() { return new VegaRestoreEditor (*this); }
