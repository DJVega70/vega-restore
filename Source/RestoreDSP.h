#pragma once
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>

// Restoration DSP building blocks. Everything runs on fixed 512-sample hops.
namespace restore
{
constexpr int H = 512;            // hop
constexpr int N = 2048;           // FFT size (75% overlap)
constexpr int bins = N / 2 + 1;
constexpr int P = 24;             // AR model order for click / clip repair
constexpr int BUF = 4 * H;        // click stage history: [out-pending | older | current | lookahead]

// Levinson-Durbin: autocorrelation r[0..p] -> a[1..p] with x[n] ~ sum a[j] x[n-j]
inline bool levinson (const double* r, double* a, int p)
{
    double tmp[P + 1];
    double err = r[0];
    if (err <= 0) return false;
    for (int i = 0; i <= p; ++i) a[i] = 0;
    for (int i = 1; i <= p; ++i)
    {
        double acc = r[i];
        for (int j = 1; j < i; ++j) acc -= a[j] * r[i - j];
        const double k = acc / err;
        const double ne = err * (1.0 - k * k);
        if (ne <= r[0] * 1.0e-9 || std::abs (k) >= 1.0) return i > 1;   // numerically exhausted: keep the lower-order model
        for (int j = 1; j < i; ++j) tmp[j] = a[j] - k * a[i - j];
        for (int j = 1; j < i; ++j) a[j] = tmp[j];
        a[i] = k;
        err = ne;
    }
    return true;
}

//==============================================================================
// De-click / de-crackle / de-clip. Detects impulses in the AR prediction residual and
// rebuilds damaged samples by forward/backward AR prediction. Latency: 2 hops.
struct ImpulseStage
{
    float buf[BUF] {};
    float e[BUF] {}, d[BUF] {};
    double a[P + 1] {};
    int clicks = 0, crackles = 0, clips = 0;

    void reset() { std::fill (buf, buf + BUF, 0.0f); std::fill (e, e + BUF, 0.0f); }

    void process (const float* in, float* out, float clickSens, float crackSens, bool declip, float ceiling, double sr)
    {
        std::memmove (buf, buf + H, sizeof (float) * 3 * H);
        std::memcpy (buf + 3 * H, in, sizeof (float) * H);
        if (clickSens > 0 || crackSens > 0 || declip)
            analyse (clickSens, crackSens, declip, ceiling, sr);
        std::memcpy (out, buf + H, sizeof (float) * H);
    }

private:
    // fit the AR model on buf[from, from + len) with a Hann window
    bool fitModel (int from, int len)
    {
        float xw[BUF];
        for (int i = 0; i < len; ++i) xw[i] = buf[from + i] * (0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (i + 0.5f) / len));
        double r[P + 1];
        for (int l = 0; l <= P; ++l)
        {
            double sum = 0;
            for (int i = 0; i + l < len; ++i) sum += (double) xw[i] * xw[i + l];
            r[l] = sum;
        }
        if (r[0] < 1.0e-10) return false;
        r[0] *= 1.001;
        return levinson (r, a, P);
    }

    float residual (int n) const
    {
        double s = buf[n];
        for (int j = 1; j <= P; ++j) s -= a[j] * buf[n - j];
        return (float) s;
    }

    // rebuild buf[s..t] from the good samples around it: least-squares AR interpolation
    // (Janssen / Godsill) - the gap is filled with whatever minimises the model's prediction error
    void repair (int s, int t, bool clipConstraint)
    {
        const int L = t - s + 1;
        if (L <= 0 || L > 200) return;
        double c[P + 1]; c[0] = 1.0; for (int j = 1; j <= P; ++j) c[j] = -a[j];
        double rc[P + 1];
        for (int d = 0; d <= P; ++d) { double sum = 0; for (int j = 0; j + d <= P; ++j) sum += c[j] * c[j + d]; rc[d] = sum; }
        // known-only prediction error over every equation touching the gap
        double ek[200 + P + 1];
        for (int n = s; n <= t + P; ++n)
        {
            double sum = 0;
            for (int j = 0; j <= P; ++j) { const int k = n - j; if (k < s || k > t) sum += c[j] * buf[k]; }
            ek[n - s] = sum;
        }
        static thread_local std::vector<double> M; M.assign ((size_t) (L * L), 0.0);
        double y[200];
        for (int i = 0; i < L; ++i)
        {
            for (int k = std::max (0, i - P); k <= std::min (L - 1, i + P); ++k) M[(size_t) (i * L + k)] = rc[std::abs (i - k)];
            double bi = 0; for (int d = 0; d <= P; ++d) bi += c[d] * ek[i + d];
            y[i] = -bi;
        }
        // banded Cholesky (in place, lower triangle)
        for (int j = 0; j < L; ++j)
        {
            double d = M[(size_t) (j * L + j)];
            for (int k = std::max (0, j - P); k < j; ++k) d -= M[(size_t) (j * L + k)] * M[(size_t) (j * L + k)];
            if (d <= 1.0e-12) return;
            d = std::sqrt (d); M[(size_t) (j * L + j)] = d;
            for (int i = j + 1; i <= std::min (L - 1, j + P); ++i)
            {
                double v = M[(size_t) (i * L + j)];
                for (int k = std::max (0, i - P); k < j; ++k) v -= M[(size_t) (i * L + k)] * M[(size_t) (j * L + k)];
                M[(size_t) (i * L + j)] = v / d;
            }
        }
        for (int i = 0; i < L; ++i) { double v = y[i]; for (int k = std::max (0, i - P); k < i; ++k) v -= M[(size_t) (i * L + k)] * y[k]; y[i] = v / M[(size_t) (i * L + i)]; }
        for (int i = L - 1; i >= 0; --i) { double v = y[i]; for (int k = i + 1; k <= std::min (L - 1, i + P); ++k) v -= M[(size_t) (k * L + i)] * y[k]; y[i] = v / M[(size_t) (i * L + i)]; }
        for (int n = s; n <= t; ++n)
        {
            float v = juce::jlimit (-4.0f, 4.0f, (float) y[n - s]);
            if (clipConstraint) { const float o = buf[n]; v = (o >= 0 ? 1.0f : -1.0f) * std::max (std::abs (v), std::abs (o)); }
            buf[n] = v;
        }
    }

    void analyse (float clickSens, float crackSens, bool declip, float ceiling, double sr)
    {
        // model from the already-repaired history, so damage ahead can't teach it to predict clicks
        if (! fitModel (0, 2 * H)) return;
        const int segA = 2 * H, segB = 3 * H;
        const int lastOk = BUF - P - 2;

        if (declip)
        {
            const float thr = ceiling * 0.999f;
            for (int n = segA; n < segB; ++n)
            {
                if (std::abs (buf[n]) < thr || std::abs (buf[n - 1]) >= thr) continue;
                int t = n; while (t + 1 < lastOk && std::abs (buf[t + 1]) >= thr) ++t;
                const int len = t - n + 1;
                if (len >= 3 && len <= 200) { repair (n, t, true); ++clips; }
                n = t;
            }
        }

        if (clickSens <= 0 && crackSens <= 0) return;
        // pass 1: find damage, patch it roughly, refit the model on the patched audio, then rebuild properly
        Region rs[64]; int nr = detect (clickSens, crackSens, sr, rs, 64);
        if (nr == 0) return;
        for (int i = 0; i < nr; ++i)
        {
            const int s = rs[i].s, t = rs[i].t; const float y0 = buf[s - 1], y1 = buf[t + 1];
            for (int k = s; k <= t; ++k) buf[k] = y0 + (y1 - y0) * (float) (k - s + 1) / (float) (t - s + 2);
            if (rs[i].click) ++clicks; else ++crackles;
        }
        if (! fitModel (0, 3 * H)) return;
        for (int i = 0; i < nr; ++i) repair (rs[i].s, rs[i].t, false);
        // pass 2: anything left over
        nr = detect (clickSens, crackSens, sr, rs, 64);
        for (int i = 0; i < nr; ++i) repair (rs[i].s, rs[i].t, false);
    }

    struct Region { int s, t; bool click; };

    int detect (float clickSens, float crackSens, double sr, Region* out, int maxOut)
    {
        const int segA = 2 * H, segB = 3 * H;
        const int lastOk = BUF - P - 2;
        for (int n = H + P; n < BUF; ++n) e[n] = residual (n);
        // backward residual: an impulse disturbs the forward residual from its start onwards and the
        // backward residual up to its end, so start = first forward hit, end = last backward hit
        for (int n = H + P; n < BUF; ++n)
        {
            if (n + P >= BUF) { d[n] = std::abs (e[n]); continue; }
            double sb = buf[n]; for (int j = 1; j <= P; ++j) sb -= a[j] * buf[n + j];
            d[n] = (float) std::abs (sb);
        }
        float mags[3 * H]; int m = 0;
        for (int n = H + P; n < BUF; ++n) mags[m++] = std::abs (e[n]);
        std::nth_element (mags, mags + m / 2, mags + m);
        const float sigma = std::max (mags[m / 2] / 0.6745f, 1.0e-5f);
        const float Tclick = clickSens > 0 ? juce::jmap (clickSens, 40.0f, 9.0f) * sigma : 1.0e30f;
        const float Tcrack = crackSens > 0 ? juce::jmap (crackSens, 22.0f, 6.0f) * sigma : 1.0e30f;
        const float Tlow = std::min (Tclick, Tcrack);
        const int maxClick = (int) (0.0025 * sr) + 8;
        const int maxCrack = P + (int) (0.0004 * sr);
        const int gap = std::max (8, (int) (0.0005 * sr));
        int nr = 0;
        for (int n = segA; n < segB && nr < maxOut; ++n)
        {
            if (std::abs (e[n]) <= Tlow) continue;
            int t = n; float peak = std::abs (e[n]);
            for (int k = n + 1; k < lastOk && k - t <= gap; ++k)
                if (d[k] > Tlow) { t = k; peak = std::max (peak, std::min (std::abs (e[k]), d[k])); }
            // smooth pops fade in and out below the detector: pad wider events more
            const int pad = 2 + (t - n) / 3;
            const int s = std::max (H + P + 1, n - pad);
            int tt = std::min (t + 1 + pad, lastOk - 1);
            // a click is over quickly; a drum hit / new note keeps the residual high afterwards.
            // Grow the region until the residual settles; if it never does, it's music - leave it.
            auto quietAfter = [&] (int end)
            {
                // median, so other ticks nearby don't count as "still busy"
                float w[128]; int cnt = 0;
                for (int k = end + P + 1; k < std::min (BUF, end + P + 129); ++k) w[cnt++] = std::abs (e[k]);
                if (cnt < 32) return true;
                std::nth_element (w, w + cnt / 2, w + cnt);
                return w[cnt / 2] < 2.0f * sigma;
            };
            bool settled = quietAfter (tt);
            while (! settled && tt - s + 1 < maxClick && tt + 8 < lastOk) { tt += 8; settled = quietAfter (tt); }
            if (! settled) { n = tt; continue; }
            const int len = tt - s + 1;
            if (peak > Tclick && len <= maxClick) out[nr++] = { s, tt, true };
            else if (peak > Tcrack && peak <= Tclick && len <= maxCrack) out[nr++] = { s, tt, false };
            n = tt + P;   // the forward residual right after is the event's own echo; pass 2 catches anything real there
        }
        return nr;
    }
};

//==============================================================================
// STFT noise reduction (de-hiss / surface noise) with an adaptive noise tracker
// (minimum statistics) or a learned noise print. Latency: N - H = 3 hops.
struct SpectralStage
{
    static constexpr int U = 6, V = 16;   // minimum-statistics sub-windows
    juce::dsp::FFT fft { 11 };
    float ring[N] {}, acc[N] {}, win[N] {};
    std::vector<float> fb = std::vector<float> (2 * N);
    float prevPow[bins] {}, smooth[bins] {}, curMin[bins] {}, subMin[U][bins] {}, noise[bins] {}, gs[bins] {}, power[bins] {}, outPow[bins] {};
    int frame = 0, subIdx = 0, subFilled = 0;
    float invScale = 1.0f;

    SpectralStage()
    {
        for (int i = 0; i < N; ++i) win[i] = std::sqrt (0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * i / N));
        // calibrate the inverse transform scaling once
        std::vector<float> t (2 * N, 0.0f); t[3] = 1.0f;
        fft.performRealOnlyForwardTransform (t.data());
        fft.performRealOnlyInverseTransform (t.data());
        invScale = std::abs (t[3]) > 1.0e-9f ? 1.0f / t[3] : 1.0f;
        reset();
    }
    void reset()
    {
        std::fill (ring, ring + N, 0.0f); std::fill (acc, acc + N, 0.0f);
        for (int k = 0; k < bins; ++k) { prevPow[k] = 0; smooth[k] = 0; curMin[k] = 1.0e30f; noise[k] = 0; gs[k] = 1; for (auto& s : subMin) s[k] = 1.0e30f; }
        frame = subIdx = subFilled = 0;
    }

    // floorGain: 1 = no reduction. profile: learned noise power per bin (nullptr = adaptive).
    void process (const float* in, float* out, float floorGain, float thrFactor, const float* profile)
    {
        std::memmove (ring, ring + H, sizeof (float) * (N - H));
        std::memcpy (ring + N - H, in, sizeof (float) * H);
        for (int i = 0; i < N; ++i) fb[(size_t) i] = ring[i] * win[i];
        std::fill (fb.begin() + N, fb.end(), 0.0f);
        fft.performRealOnlyForwardTransform (fb.data());
        for (int k = 0; k < bins; ++k) power[k] = fb[(size_t) (2 * k)] * fb[(size_t) (2 * k)] + fb[(size_t) (2 * k + 1)] * fb[(size_t) (2 * k + 1)];
        track();

        if (floorGain < 0.999f)
        {
            float g[bins];
            for (int k = 0; k < bins; ++k)
            {
                const float est = profile != nullptr ? profile[k] : noise[k];
                const float nk = std::max (est * thrFactor * 1.4f, 1.0e-20f);
                const float post = power[k] / nk;
                // decision-directed a-priori SNR (Ephraim-Malah) -> Wiener gain; keeps musical noise down
                const float prio = 0.96f * (gs[k] * gs[k] * prevPow[k] / nk) + 0.04f * std::max (post - 1.0f, 0.0f);
                g[k] = prio / (1.0f + prio);
                prevPow[k] = power[k];
            }
            for (int k = 0; k < bins; ++k)
            {
                gs[k] = 0.25f * g[std::max (0, k - 1)] + 0.5f * g[k] + 0.25f * g[std::min (bins - 1, k + 1)];
                const float G = floorGain + (1.0f - floorGain) * gs[k];
                fb[(size_t) (2 * k)] *= G; fb[(size_t) (2 * k + 1)] *= G;
                outPow[k] = power[k] * G * G;
            }
            for (int k = bins; k < N; ++k) { fb[(size_t) (2 * k)] = fb[(size_t) (2 * (N - k))]; fb[(size_t) (2 * k + 1)] = -fb[(size_t) (2 * (N - k) + 1)]; }
            fft.performRealOnlyInverseTransform (fb.data());
            for (int i = 0; i < N; ++i) acc[i] += fb[(size_t) i] * invScale * win[i] * 0.5f;
        }
        else
        {
            for (int k = 0; k < bins; ++k) { outPow[k] = power[k]; gs[k] = 1.0f; }
            for (int i = 0; i < N; ++i) acc[i] += ring[i] * win[i] * win[i] * 0.5f;   // identical to an untouched STFT round trip
        }
        std::memcpy (out, acc, sizeof (float) * H);
        std::memmove (acc, acc + H, sizeof (float) * (N - H));
        std::fill (acc + N - H, acc + N, 0.0f);
    }

private:
    void track()
    {
        for (int k = 0; k < bins; ++k)
        {
            smooth[k] = frame == 0 ? power[k] : 0.85f * smooth[k] + 0.15f * power[k];
            curMin[k] = std::min (curMin[k], smooth[k]);
        }
        ++frame;
        if (frame % V == 0)
        {
            for (int k = 0; k < bins; ++k) { subMin[subIdx][k] = curMin[k]; curMin[k] = 1.0e30f; }
            subIdx = (subIdx + 1) % U; subFilled = std::min (U, subFilled + 1);
        }
        float raw[bins];
        for (int k = 0; k < bins; ++k)
        {
            float mn = curMin[k];
            for (int u = 0; u < subFilled; ++u) mn = std::min (mn, subMin[u][k]);
            raw[k] = mn * 2.0f;   // minimum of a smoothed periodogram under-reads the mean
        }
        // noise is smooth across frequency, held notes are not: a running median removes
        // sustained tones from the estimate so they are never mistaken for hiss
        constexpr int R = 12;
        float w[2 * R + 1];
        for (int k = 0; k < bins; ++k)
        {
            const int a = std::max (0, k - R), b = std::min (bins - 1, k + R), c = b - a + 1;
            std::copy (raw + a, raw + b + 1, w);
            std::nth_element (w, w + c / 2, w + c);
            noise[k] = std::min (raw[k], w[c / 2]);
        }
    }
};

//==============================================================================
// Rebuilds the top octave that old / low-bitrate MP3s cut off: the octave just below
// the cutoff is rectified (generating its 2nd harmonic) and only the new content above
// the cutoff is added back.
struct HighRebuild
{
    juce::dsp::IIR::Filter<float> srcHp, srcLp, outHp1, outHp2;
    float lastF = -1;
    void prepare (double sr, float fc)
    {
        using C = juce::dsp::IIR::Coefficients<float>;
        fc = std::min (fc, (float) sr * 0.45f);
        srcHp.coefficients = C::makeHighPass (sr, fc * 0.5f, 0.707f);
        srcLp.coefficients = C::makeLowPass (sr, std::min (fc, (float) sr * 0.45f), 0.707f);
        outHp1.coefficients = C::makeHighPass (sr, fc, 0.541f);
        outHp2.coefficients = C::makeHighPass (sr, fc, 1.307f);
        lastF = fc;
    }
    void reset() { srcHp.reset(); srcLp.reset(); outHp1.reset(); outHp2.reset(); }
    float process (float x, float amount)
    {
        const float band = srcLp.processSample (srcHp.processSample (x));
        const float gen = std::abs (band) * 2.0f;
        return x + amount * outHp2.processSample (outHp1.processSample (gen));
    }
};
} // namespace restore
