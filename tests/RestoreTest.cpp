#include "../Source/PluginProcessor.h"
#include <iostream>
static int fails = 0;
static void check (bool ok, const juce::String& m) { std::cout << (ok ? "PASS " : "FAIL ") << m << "\n"; if (! ok) ++fails; }
static void set (VegaRestoreProcessor& p, const char* id, float v) { auto* q = p.apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v)); }
const double fs = 48000; const int blk = 441;   // odd block size on purpose
const double tau = 2 * juce::MathConstants<double>::pi;
static std::vector<float> run (VegaRestoreProcessor& p, const std::vector<float>& in)
{
    std::vector<float> out; juce::AudioBuffer<float> b (2, blk); juce::MidiBuffer m;
    for (size_t pos = 0; pos < in.size() + VegaRestoreProcessor::latency; pos += blk)
    {
        for (int i = 0; i < blk; ++i) { const float x = pos + (size_t) i < in.size() ? in[pos + (size_t) i] : 0.0f; b.setSample (0, i, x); b.setSample (1, i, x); }
        p.processBlock (b, m);
        for (int i = 0; i < blk; ++i) out.push_back (b.getSample (0, i));
    }
    out.erase (out.begin(), out.begin() + VegaRestoreProcessor::latency);   // align
    out.resize (in.size());
    return out;
}
static auto make()
{
    auto* p = new VegaRestoreProcessor(); p->setPlayConfigDetails (2, 2, fs, blk); p->prepareToPlay (fs, blk);
    return std::unique_ptr<VegaRestoreProcessor> (p);
}
static void allOff (VegaRestoreProcessor& p) { set (p, "rumbleOn", 0); set (p, "declick", 0); set (p, "crackle", 0); set (p, "denoise", 0); }
static double errDb (const std::vector<float>& a, const std::vector<float>& ref, size_t from, size_t to)
{
    double e = 0, r = 0; for (size_t i = from; i < to; ++i) { e += (a[i] - ref[i]) * (a[i] - ref[i]); r += ref[i] * ref[i]; }
    return 10 * std::log10 (e / r + 1e-30);
}
// band energy via a Goertzel-free brute DFT over a range of frequencies
static double bandDb (const std::vector<float>& v, double f0, double f1, size_t a, int len)
{
    juce::dsp::FFT fft (14); const int n = 1 << 14; double s = 0; int cnt = 0;
    for (size_t off = a; off + n <= a + (size_t) len; off += n / 2)
    {
        std::vector<float> d (2 * n, 0.0f);
        for (int i = 0; i < n; ++i) d[(size_t) i] = v[off + (size_t) i] * (float) (0.5 - 0.5 * std::cos (tau * i / n));
        fft.performFrequencyOnlyForwardTransform (d.data());
        for (int k = (int) (f0 * n / fs); k <= (int) (f1 * n / fs); ++k) s += (double) d[(size_t) k] * d[(size_t) k];
        ++cnt;
    }
    return 10 * std::log10 (s / cnt + 1e-30);
}
static std::vector<float> music (int n, unsigned seed = 1, float noise = 0.002f)
{
    juce::Random r ((int) seed); std::vector<float> v ((size_t) n);
    const double f[] = { 110, 220.5, 331, 523, 787, 1234, 2100, 3300 };
    for (int i = 0; i < n; ++i)
    {
        double s = 0; for (int k = 0; k < 8; ++k) s += (0.12 / (1 + k * 0.4)) * std::sin (tau * f[k] * i / fs + k) * (0.7 + 0.3 * std::sin (tau * (0.3 + 0.1 * k) * i / fs));
        v[(size_t) i] = (float) s + noise * (r.nextFloat() * 2 - 1);
    }
    return v;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    {
        auto p = make(); allOff (*p);
        check (p->getLatencySamples() == VegaRestoreProcessor::latency, "reports " + juce::String (p->getLatencySamples()) + " samples latency");
        juce::Random r (3); std::vector<float> in (96000); for (auto& x : in) x = r.nextFloat() - 0.5f;
        auto o = run (*p, in); double mx = 0; for (size_t i = 0; i < in.size(); ++i) mx = std::max (mx, (double) std::abs (o[i] - in[i]));
        check (mx < 1e-4, "everything off = bit-transparent apart from the delay (max err " + juce::String (mx, 7) + ")");
        set (*p, "listen", 1); auto d = run (*p, in); double md = 0; for (size_t i = 20000; i < in.size(); ++i) md = std::max (md, (double) std::abs (d[i]));
        check (md < 1e-4, "Listen Removed is silent when nothing is removed (" + juce::String (md, 7) + ")");
    }
    const int n = 48000 * 4;
    {   // vinyl clicks: big pops of various widths
        auto clean = music (n); auto dirty = clean; juce::Random r (7);
        for (int c = 0; c < 40; ++c)
        {
            const int at = 20000 + r.nextInt (n - 30000), w = 1 + r.nextInt (40); const float amp = (0.25f + 0.6f * r.nextFloat()) * (r.nextBool() ? 1 : -1);
            for (int i = 0; i < w; ++i) dirty[(size_t) (at + i)] += amp * (float) std::sin (juce::MathConstants<double>::pi * (i + 0.5) / w) * (i % 2 ? 0.6f : 1.0f);
        }
        auto p = make(); allOff (*p); set (*p, "declick", 50);
        auto o = run (*p, dirty);
        const double before = errDb (dirty, clean, 20000, (size_t) n), after = errDb (o, clean, 20000, (size_t) n);
        check (after < before - 12, "de-click: click noise " + juce::String (before, 1) + " dB -> " + juce::String (after, 1) + " dB (" + juce::String (p->uiClicks.load()) + " clicks fixed)");
    }
    for (int dens : { 200, 600 })
    {   // crackle: tiny ticks (moderate surface noise, then a really worn record)
        auto clean = music (n, 2); auto dirty = clean; juce::Random r (9);
        for (int c = 0; c < dens; ++c) { const int at = 10000 + r.nextInt (n - 20000), w = 1 + r.nextInt (3); const float amp = (0.02f + 0.05f * r.nextFloat()) * (r.nextBool() ? 1 : -1); for (int i = 0; i < w; ++i) dirty[(size_t) (at + i)] += amp; }
        auto p = make(); allOff (*p); set (*p, "crackle", 50);
        auto o = run (*p, dirty);
        const double before = errDb (dirty, clean, 20000, (size_t) n), after = errDb (o, clean, 20000, (size_t) n);
        check (after < before - (dens < 300 ? 8 : 4), "de-crackle " + juce::String (dens / 4) + " ticks/s: " + juce::String (before, 1) + " dB -> " + juce::String (after, 1) + " dB");
    }
    {   // clean music with sharp drum hits must come through untouched
        auto clean = music (n, 3); juce::Random r (4);
        for (int hit = 0; hit < 16; ++hit)
        {
            const int at = 12000 + hit * 11000;
            for (int i = 0; i < 6000; ++i) { const double env = std::exp (-i / 900.0); clean[(size_t) (at + i)] += (float) (0.6 * env * std::sin (tau * (60 + 120 * std::exp (-i / 300.0)) * i / fs) + 0.25 * std::exp (-i / 150.0) * (r.nextFloat() * 2 - 1)); }
        }
        auto p = make(); allOff (*p); set (*p, "declick", 50); set (*p, "crackle", 30);
        auto o = run (*p, clean);
        const double e = errDb (o, clean, 10000, (size_t) n);
        const int fixes = p->uiClicks.load() + p->uiCrackles.load();
        check (e < -35, "drum hits and music left alone at default settings (change " + juce::String (e, 1) + " dB, " + juce::String (fixes) + " false fixes)");
    }
    {   // hiss: adaptive
        juce::Random r (5); std::vector<float> clean ((size_t) n), dirty ((size_t) n);
        for (int i = 0; i < n; ++i) { clean[(size_t) i] = 0.3f * (float) std::sin (tau * 1000 * i / fs); dirty[(size_t) i] = clean[(size_t) i] + 0.01f * (r.nextFloat() * 2 - 1); }
        auto ref = make(); allOff (*ref); auto a = run (*ref, dirty);
        auto p = make(); allOff (*p); set (*p, "denoise", 20); auto o = run (*p, dirty);
        const double nb = bandDb (a, 3000, 15000, 96000, 90000), na = bandDb (o, 3000, 15000, 96000, 90000);
        const double sb = bandDb (a, 950, 1050, 96000, 90000), sa = bandDb (o, 950, 1050, 96000, 90000);
        check (nb - na > 12 && std::abs (sb - sa) < 1.0, "de-hiss (adaptive): hiss down " + juce::String (nb - na, 1) + " dB, tone changed " + juce::String (sa - sb, 2) + " dB");
    }
    {   // learned noise print from a lead-in, then music
        juce::Random r (6); std::vector<float> in ((size_t) n);
        for (int i = 0; i < n; ++i) { const float hiss = 0.01f * (r.nextFloat() * 2 - 1); in[(size_t) i] = hiss + (i > 48000 ? 0.3f * (float) std::sin (tau * 1000 * i / fs) : 0.0f); }
        auto ref = make(); allOff (*ref); auto a = run (*ref, in);
        auto p = make(); allOff (*p); set (*p, "denoise", 20); set (*p, "noiseMode", 1);
        std::vector<float> lead (in.begin(), in.begin() + 40000); set (*p, "learn", 1); run (*p, lead); set (*p, "learn", 0);
        auto o = run (*p, in);
        const double nb = bandDb (a, 3000, 15000, 96000, 90000), na = bandDb (o, 3000, 15000, 96000, 90000);
        check (nb - na > 12, "de-hiss (learned print): hiss down " + juce::String (nb - na, 1) + " dB");
        juce::MemoryBlock st; p->getStateInformation (st);
        auto q = make(); q->setStateInformation (st.getData(), (int) st.getSize());
        allOff (*q); set (*q, "denoise", 20); set (*q, "noiseMode", 1); auto o2 = run (*q, in);
        check (nb - bandDb (o2, 3000, 15000, 96000, 90000) > 12, "restored session uses the saved noise print");
    }
    {   // hum + rumble
        std::vector<float> in ((size_t) n);
        for (int i = 0; i < n; ++i) in[(size_t) i] = (float) (0.1 * std::sin (tau * 60 * i / fs) + 0.05 * std::sin (tau * 180 * i / fs) + 0.2 * std::sin (tau * 7 * i / fs) + 0.2 * std::sin (tau * 1000 * i / fs));
        auto ref = make(); allOff (*ref); auto a = run (*ref, in);
        auto p = make(); allOff (*p); set (*p, "hum", 2); set (*p, "rumbleOn", 1); auto o = run (*p, in);
        auto d = [&] (double f) { return bandDb (o, f - 3, f + 3, 96000, 90000) - bandDb (a, f - 3, f + 3, 96000, 90000); };
        check (d (60) < -20 && d (180) < -20, "hum 60 Hz " + juce::String (d (60), 1) + " dB, 180 Hz " + juce::String (d (180), 1) + " dB");
        check (d (7) < -25, "rumble 7 Hz " + juce::String (d (7), 1) + " dB");
        check (std::abs (d (1000)) < 0.5, "music at 1 kHz untouched (" + juce::String (d (1000), 2) + " dB)");
    }
    {   // clipped MP3
        std::vector<float> truth ((size_t) n), clipped ((size_t) n);
        for (int i = 0; i < n; ++i) { truth[(size_t) i] = (float) (1.0 * std::sin (tau * 300 * i / fs) + 0.4 * std::sin (tau * 1100 * i / fs)); clipped[(size_t) i] = juce::jlimit (-1.0f, 1.0f, truth[(size_t) i]); }
        auto p = make(); allOff (*p); set (*p, "declip", 1); set (*p, "ceiling", 0);
        auto o = run (*p, clipped);
        const double before = errDb (clipped, truth, 20000, (size_t) n), after = errDb (o, truth, 20000, (size_t) n);
        check (after < before - 10, "de-clip: error " + juce::String (before, 1) + " dB -> " + juce::String (after, 1) + " dB (" + juce::String (p->uiClips.load()) + " clips rebuilt)");
    }
    {   // dull MP3: everything above 11 kHz missing
        juce::Random r (8); std::vector<float> in ((size_t) n);
        juce::dsp::IIR::Filter<float> lp[4]; for (auto& f : lp) f.coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass (fs, 10500, 0.707f);
        for (auto& x : in) { float s = 0.3f * (r.nextFloat() * 2 - 1); for (auto& f : lp) s = f.processSample (s); x = s; }
        auto ref = make(); allOff (*ref); auto a = run (*ref, in);
        auto p = make(); allOff (*p); set (*p, "hfOn", 1); set (*p, "hfFreq", 11000); set (*p, "hfAmt", 60); auto o = run (*p, in);
        const double up = bandDb (o, 12500, 19000, 96000, 90000) - bandDb (a, 12500, 19000, 96000, 90000);
        const double low = bandDb (o, 500, 8000, 96000, 90000) - bandDb (a, 500, 8000, 96000, 90000);
        check (up > 10 && std::abs (low) < 0.5, "MP3 rebuild: 12.5-19 kHz +" + juce::String (up, 1) + " dB, below 8 kHz " + juce::String (low, 2) + " dB");
    }
    {   // everything on, noisy input: finite and stable
        auto p = make(); set (*p, "hum", 1); set (*p, "declip", 1); set (*p, "hfOn", 1); set (*p, "denoise", 30); set (*p, "declick", 100); set (*p, "crackle", 100); set (*p, "mono", 1);
        juce::Random r (2); std::vector<float> in ((size_t) n); for (auto& x : in) x = (r.nextFloat() - 0.5f) * 1.8f;
        auto o = run (*p, in); bool fin = true; float mx = 0; for (float v : o) { fin &= std::isfinite (v); mx = std::max (mx, std::abs (v)); }
        check (fin && mx < 4, "all modules at max on loud noise stays finite (peak " + juce::String (mx, 2) + ")");
    }
    std::cout << (fails == 0 ? "ALL PASSED" : "FAILURES: " + std::to_string (fails)) << "\n";
    return fails == 0 ? 0 : 1;
}
