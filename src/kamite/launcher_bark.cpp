#include "kamite/launcher_bark.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <random>

// Ported from the Python mock-ups K approved (round 4 of Nov 10): same filters, levels and timings.
namespace kamite::launcher_bark
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;
        using Mono = std::vector<float>;

        // --- the lines -------------------------------------------------------------------------------------------
        enum class Treatment
        {
            NarratorA, // a bit deeper (0.84x), 3 s hall
            NarratorB, // much deeper (0.75x), doubled, 3.6 s hall
            NarratorD, // original pitch, 2 s room
            Cutscene   // full-band cutscene audio: warmth, compression, 2 s room
        };
        struct Line
        {
            const char *name;
            const char *file;     // an AFS of ADX streams on the disc
            int entry;            // its stream
            uint32_t first, end;  // sample range (48 kHz)
            float fadeOut;        // seconds at the cut's end
            Treatment treatment;
        };
        // The narration: "...the title of God Hand" (31.195-32.12 s, from the G's onset through the D's release).
        // The cutscene cuts: K's ten keepers of 23 found by speech recognition (12, 20 and 22 trimmed by hand).
        constexpr Line kLines[] = {
            {"narrator A", "se/r000.afs", 1, 1497360, 1541760, 0.008f, Treatment::NarratorA},
            {"narrator B", "se/r000.afs", 1, 1497360, 1541760, 0.008f, Treatment::NarratorB},
            {"narrator D", "se/r000.afs", 1, 1497360, 1541760, 0.008f, Treatment::NarratorD},
            {"ev04 22.08", "se/ev/ev04.afs", 0, 1059623, 1113188, 0.006f, Treatment::Cutscene},
            {"ev04 42.98", "se/ev/ev04.afs", 0, 2063039, 2134004, 0.006f, Treatment::Cutscene},
            {"ev07 67.81", "se/ev/ev07.afs", 0, 3254830, 3332281, 0.006f, Treatment::Cutscene},
            {"ev17 20.21", "se/ev/ev17.afs", 0, 970189, 1027980, 0.006f, Treatment::Cutscene},
            {"ev19 71.80", "se/ev/ev19.afs", 0, 3446400, 3501120, 0.030f, Treatment::Cutscene},
            {"ev20 68.61", "se/ev/ev20.afs", 0, 3293478, 3352119, 0.006f, Treatment::Cutscene},
            {"ev29 6.56", "se/ev/ev29.afs", 0, 314685, 387241, 0.006f, Treatment::Cutscene},
            {"ev32 48.86", "se/ev/ev32.afs", 0, 2345117, 2398080, 0.006f, Treatment::Cutscene},
            {"ev34 13.05", "se/ev/ev34.afs", 0, 626400, 684960, 0.015f, Treatment::Cutscene},
            {"ev4a 6.60", "se/ev/ev4a.afs", 0, 316800, 408000, 0.080f, Treatment::Cutscene},
        };

        // --- ADX (CRI, v4, type-8 encryption with Clover's key) ----------------------------------------------------
        uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
        uint32_t be32(const uint8_t *p) { return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
        uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

        // Samples [first, end) of an AFS entry's ADX stream, the channels averaged; false when it isn't one.
        bool decodeAdx(const std::vector<uint8_t> &afs, int entry, uint32_t first, uint32_t end, Mono &out)
        {
            if (afs.size() < 16 || std::memcmp(afs.data(), "AFS", 3) != 0 || static_cast<uint32_t>(entry) >= le32(&afs[4]))
                return false;
            const uint32_t offset = le32(&afs[8 + 8 * entry]), size = le32(&afs[12 + 8 * entry]);
            if (size < 0x20 || static_cast<uint64_t>(offset) + size > afs.size())
                return false;
            const uint8_t *b = afs.data() + offset;
            if (b[0] != 0x80)
                return false;
            const int dataAt = be16(b + 2) + 4, blockSize = b[5], bits = b[6], channels = b[7];
            const uint32_t rate = be32(b + 8), total = be32(b + 12);
            const int highpass = be16(b + 16);
            const bool encrypted = b[0x13] == 8;
            if (blockSize < 3 || bits != 4 || channels < 1 || channels > 2 || rate != static_cast<uint32_t>(kRate) || end > total)
                return false;
            const double a = std::sqrt(2.0) - std::cos(2.0 * kPi * highpass / rate), c0 = std::sqrt(2.0) - 1.0;
            const double cc = (a - std::sqrt((a + c0) * (a - c0))) / c0;
            const int coef1 = static_cast<int>(cc * 8192.0), coef2 = static_cast<int>(-cc * cc * 4096.0);
            const int perBlock = (blockSize - 2) * 8 / bits;
            int hist1[2] = {}, hist2[2] = {}, key[2] = {};
            for (int ch = 0; ch < channels; ++ch)
            {
                key[ch] = 0x49E1;
                for (int i = 0; i < ch; ++i)
                    key[ch] = (key[ch] * 0x4A57 + 0x553D) & 0x7FFF;
            }
            out.assign(end - first, 0.0f);
            uint64_t pos = static_cast<uint64_t>(dataAt);
            for (uint32_t done = 0; done < end; done += perBlock)
            {
                for (int ch = 0; ch < channels; ++ch)
                {
                    if (pos + blockSize > size)
                        return false;
                    const uint8_t *block = b + pos;
                    pos += blockSize;
                    int scale = be16(block);
                    if (scale == 0x8001)
                        return false;
                    if (encrypted)
                    {
                        scale = ((scale ^ key[ch]) & 0x1FFF) + 1;
                        for (int i = 0; i < channels; ++i)
                            key[ch] = (key[ch] * 0x4A57 + 0x553D) & 0x7FFF;
                    }
                    for (int i = 0; i < perBlock; ++i)
                    {
                        const int byte = block[2 + i / 2];
                        int nibble = (i & 1) ? (byte & 15) : (byte >> 4);
                        if (nibble >= 8)
                            nibble -= 16;
                        const int s = std::clamp(nibble * scale + ((coef1 * hist1[ch] + coef2 * hist2[ch]) >> 12), -32768, 32767);
                        hist2[ch] = hist1[ch];
                        hist1[ch] = s;
                        const uint32_t at = done + i;
                        if (at >= first && at < end)
                            out[at - first] += static_cast<float>(s) / (32768.0f * channels);
                    }
                }
            }
            return true;
        }

        // --- filters ---------------------------------------------------------------------------------------------
        enum class Kind { LowPass, HighPass, Peak, LowShelf };
        struct Biquad
        {
            double b0, b1, b2, a1, a2, z1 = 0.0, z2 = 0.0;
            Biquad(Kind kind, double f, double gainDb = 0.0, double q = 0.70710678)
            {
                const double A = std::pow(10.0, gainDb / 40.0), w0 = 2.0 * kPi * f / kRate, al = std::sin(w0) / (2.0 * q),
                             c = std::cos(w0);
                double nb[3], na[3];
                switch (kind)
                {
                case Kind::LowPass:
                    nb[0] = (1 - c) / 2; nb[1] = 1 - c; nb[2] = (1 - c) / 2; na[0] = 1 + al; na[1] = -2 * c; na[2] = 1 - al;
                    break;
                case Kind::HighPass:
                    nb[0] = (1 + c) / 2; nb[1] = -(1 + c); nb[2] = (1 + c) / 2; na[0] = 1 + al; na[1] = -2 * c; na[2] = 1 - al;
                    break;
                case Kind::Peak:
                    nb[0] = 1 + al * A; nb[1] = -2 * c; nb[2] = 1 - al * A; na[0] = 1 + al / A; na[1] = -2 * c; na[2] = 1 - al / A;
                    break;
                case Kind::LowShelf:
                {
                    const double s = 2 * std::sqrt(A) * al;
                    nb[0] = A * ((A + 1) - (A - 1) * c + s); nb[1] = 2 * A * ((A - 1) - (A + 1) * c);
                    nb[2] = A * ((A + 1) - (A - 1) * c - s); na[0] = (A + 1) + (A - 1) * c + s;
                    na[1] = -2 * ((A - 1) + (A + 1) * c); na[2] = (A + 1) + (A - 1) * c - s;
                    break;
                }
                }
                b0 = nb[0] / na[0]; b1 = nb[1] / na[0]; b2 = nb[2] / na[0]; a1 = na[1] / na[0]; a2 = na[2] / na[0];
            }
            float operator()(float x)
            {
                const double y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return static_cast<float>(y);
            }
        };
        Mono filter(Mono x, Kind kind, double f, double gainDb = 0.0, double q = 0.70710678)
        {
            Biquad bq(kind, f, gainDb, q);
            for (float &v : x)
                v = bq(v);
            return x;
        }

        float peak(const Mono &x)
        {
            float m = 0.0f;
            for (float v : x)
                m = std::max(m, std::fabs(v));
            return m;
        }
        void normalize(Mono &x)
        {
            const float m = peak(x);
            if (m > 0.0f)
                for (float &v : x)
                    v /= m;
        }
        double rms(const float *x, size_t n)
        {
            double s = 0.0;
            for (size_t i = 0; i < n; ++i)
                s += static_cast<double>(x[i]) * x[i];
            return n ? std::sqrt(s / n) : 0.0;
        }
        void fade(float *x, size_t n, bool in)
        {
            for (size_t i = 0; i < n; ++i)
            {
                const float g = static_cast<float>(i) / n;
                x[in ? i : n - 1 - i] *= g;
            }
        }

        // --- FFT and convolution -----------------------------------------------------------------------------------
        void fft(std::vector<std::complex<double>> &a, bool inverse)
        {
            const size_t n = a.size();
            for (size_t i = 1, j = 0; i < n; ++i)
            {
                size_t bit = n >> 1;
                for (; j & bit; bit >>= 1)
                    j ^= bit;
                j ^= bit;
                if (i < j)
                    std::swap(a[i], a[j]);
            }
            for (size_t len = 2; len <= n; len <<= 1)
            {
                const double ang = 2.0 * kPi / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
                const std::complex<double> wl(std::cos(ang), std::sin(ang));
                for (size_t i = 0; i < n; i += len)
                {
                    std::complex<double> w(1.0, 0.0);
                    for (size_t k = 0; k < len / 2; ++k)
                    {
                        const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                        a[i + k] = u + v;
                        a[i + k + len / 2] = u - v;
                        w *= wl;
                    }
                }
            }
            if (inverse)
                for (auto &v : a)
                    v /= static_cast<double>(n);
        }
        size_t pow2(size_t n)
        {
            size_t p = 1;
            while (p < n)
                p <<= 1;
            return p;
        }
        Mono convolve(const Mono &x, const Mono &h)
        {
            const size_t n = x.size() + h.size() - 1, size = pow2(n);
            std::vector<std::complex<double>> a(size), b(size);
            for (size_t i = 0; i < x.size(); ++i)
                a[i] = x[i];
            for (size_t i = 0; i < h.size(); ++i)
                b[i] = h[i];
            fft(a, false);
            fft(b, false);
            for (size_t i = 0; i < size; ++i)
                a[i] *= b[i];
            fft(a, true);
            Mono y(n);
            for (size_t i = 0; i < n; ++i)
                y[i] = static_cast<float>(a[i].real());
            return y;
        }
        // Energy of x between lo and hi Hz (only ratios are used).
        double bandPower(const Mono &x, double lo, double hi)
        {
            const size_t size = pow2(x.size());
            std::vector<std::complex<double>> a(size);
            for (size_t i = 0; i < x.size(); ++i)
                a[i] = x[i];
            fft(a, false);
            double s = 0.0;
            for (size_t i = 0; i <= size / 2; ++i)
            {
                const double f = static_cast<double>(i) * kRate / size;
                if (f >= lo && f < hi)
                    s += std::norm(a[i]);
            }
            return s;
        }

        // --- resampling (windowed sinc, a table of 512 phases x 64 taps) -------------------------------------------
        // Reads x at step `step` (0.84: slower and deeper, as tape; or another rate's samples: rate / kRate), every
        // `stride`-th float of x.
        Mono resample(const float *x, size_t n, double step, size_t outFrames = 0, size_t stride = 1)
        {
            if (outFrames == 0)
                outFrames = static_cast<size_t>(std::ceil(n / step));
            constexpr int kHalf = 32, kPhases = 512;
            const double cutoff = std::min(1.0, 1.0 / step);
            auto bessel0 = [](double v) {
                double s = 1.0, t = 1.0;
                for (int k = 1; k < 30; ++k)
                {
                    t *= (v / (2.0 * k)) * (v / (2.0 * k));
                    s += t;
                }
                return s;
            };
            const double beta = 8.0, norm = bessel0(beta);
            // table[phase][tap]: the kernel at d = phase / kPhases + (kHalf - 1 - tap), tap 0..2*kHalf-1
            std::vector<float> table(static_cast<size_t>(kPhases + 1) * 2 * kHalf);
            for (int ph = 0; ph <= kPhases; ++ph)
                for (int t = 0; t < 2 * kHalf; ++t)
                {
                    const double d = static_cast<double>(ph) / kPhases + (kHalf - 1 - t), r = d / kHalf;
                    double v = 0.0;
                    if (std::fabs(r) < 1.0)
                    {
                        const double arg = kPi * d * cutoff;
                        v = cutoff * (d == 0.0 ? 1.0 : std::sin(arg) / arg) * bessel0(beta * std::sqrt(1.0 - r * r)) / norm;
                    }
                    table[static_cast<size_t>(ph) * 2 * kHalf + t] = static_cast<float>(v);
                }
            Mono y(outFrames, 0.0f);
            for (size_t i = 0; i < outFrames; ++i)
            {
                const double p = i * step;
                const long c = static_cast<long>(std::floor(p));
                const double frac = (p - c) * kPhases;
                const int ph = static_cast<int>(frac);
                const float mix = static_cast<float>(frac - ph);
                const float *k0 = &table[static_cast<size_t>(ph) * 2 * kHalf], *k1 = k0 + 2 * kHalf;
                float acc = 0.0f;
                const long j0 = c - kHalf + 1;
                for (int t = 0; t < 2 * kHalf; ++t)
                {
                    const long j = j0 + t;
                    if (j < 0 || j >= static_cast<long>(n))
                        continue;
                    acc += x[static_cast<size_t>(j) * stride] * (k0[t] + mix * (k1[t] - k0[t]));
                }
                y[i] = acc;
            }
            return y;
        }
        Mono speed(const Mono &x, double k) { return resample(x.data(), x.size(), k); }

        // --- the voice chain ---------------------------------------------------------------------------------------
        Mono warm(Mono x, float drive)
        {
            const float d = std::tanh(drive);
            for (float &v : x)
                v = std::tanh(drive * v) / d;
            return x;
        }
        Mono compress(Mono x, double thresholdDb, double ratio, double attack = 0.003, double release = 0.12)
        {
            const double ga = std::exp(-1.0 / (attack * kRate)), gr = std::exp(-1.0 / (release * kRate));
            double env = 0.0;
            for (float &v : x)
            {
                const double s = std::fabs(v), g = s > env ? ga : gr;
                env = g * env + (1.0 - g) * s;
                const double db = 20.0 * std::log10(env + 1e-9);
                if (db > thresholdDb)
                    v = static_cast<float>(v * std::pow(10.0, (thresholdDb - db) * (1.0 - 1.0 / ratio) / 20.0));
            }
            return x;
        }
        // The narration is telephone-band (nothing below ~300 Hz or above ~3.5 kHz): undo the mid honk, lift the lows
        // that are left, rebuild some highs.
        Mono restore(const Mono &x)
        {
            Mono y = filter(x, Kind::HighPass, 45);
            y = filter(y, Kind::LowShelf, 260, 14, 0.6);
            y = filter(y, Kind::Peak, 1500, -9, 0.6);
            y = filter(y, Kind::Peak, 700, -2, 1.0);
            Mono ex = filter(filter(x, Kind::HighPass, 1800), Kind::LowPass, 3600);
            for (float &v : ex)
                v = std::tanh(4.0f * v);
            ex = filter(filter(ex, Kind::HighPass, 3500), Kind::HighPass, 3500);
            const float g = 0.35f * peak(x) / std::max(peak(ex), 1e-9f);
            for (size_t i = 0; i < y.size(); ++i)
                y[i] += g * ex[i];
            return y;
        }
        // The low end the radio band removed, rebuilt from the voice itself: the difference tones between neighbouring
        // harmonics fall on the fundamental and its low multiples (squared, divided back to a linear level).
        Mono regenBass(const Mono &x)
        {
            Mono b = filter(filter(filter(filter(x, Kind::HighPass, 280), Kind::HighPass, 280), Kind::LowPass, 1000), Kind::LowPass, 1000);
            const int n = kRate * 15 / 1000;
            Mono nl(b.size());
            double sum = 0.0;
            for (size_t i = 0; i < b.size() + n / 2; ++i)
            {
                if (i < b.size())
                    sum += static_cast<double>(b[i]) * b[i];
                if (i >= static_cast<size_t>(n) && i - n < b.size())
                    sum -= static_cast<double>(b[i - n]) * b[i - n];
                if (i >= static_cast<size_t>(n / 2))
                {
                    const size_t at = i - n / 2;
                    const double env = std::sqrt(std::max(sum, 0.0) / n) + 1e-6;
                    nl[at] = static_cast<float>(b[at] * b[at] / env);
                }
            }
            for (int i = 0; i < 3; ++i)
                nl = filter(nl, Kind::LowPass, 320);
            for (int i = 0; i < 2; ++i)
                nl = filter(nl, Kind::HighPass, 55);
            return nl;
        }
        // Restored voice plus the rebuilt bass, mixed until 70-300 Hz sits targetDb over 1-2 kHz.
        Mono addBass(const Mono &x, double targetDb)
        {
            Mono y = restore(x);
            normalize(y);
            Mono g = filter(regenBass(x), Kind::LowShelf, 150, 3, 0.7);
            normalize(g);
            const double ref = bandPower(y, 1000, 2000);
            Mono z = y;
            for (int step = 0; step <= 60; ++step)
            {
                const float k = 0.05f * step;
                for (size_t i = 0; i < y.size(); ++i)
                    z[i] = y[i] + k * g[i];
                if (10.0 * std::log10(bandPower(z, 70, 300) / std::max(ref, 1e-30)) >= targetDb)
                    break;
            }
            normalize(z);
            return z;
        }

        // A synthetic room: noise in three bands (lows ring longest), early reflections, pre-delay; stereo.
        std::vector<Mono> impulse(double t60, double preDelay, unsigned seed)
        {
            const size_t n = static_cast<size_t>(kRate * (t60 * 1.2 + preDelay));
            std::mt19937 rng(seed);
            std::normal_distribution<float> noise;
            std::uniform_real_distribution<double> jitter(0.0, 0.006);
            std::vector<Mono> out(2, Mono(n));
            constexpr double kBands[3][3] = {{20, 400, 1.25}, {400, 2500, 1.0}, {2500, 12000, 0.5}};
            constexpr double kReflections[5][2] = {{0.011, 0.5}, {0.019, 0.4}, {0.027, 0.35}, {0.041, 0.3}, {0.053, 0.25}};
            float m = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
            {
                Mono nz(n);
                for (float &v : nz)
                    v = noise(rng);
                for (const auto &band : kBands)
                {
                    const Mono f = filter(filter(nz, Kind::HighPass, band[0]), Kind::LowPass, band[1]);
                    for (size_t i = 0; i < n; ++i)
                        out[ch][i] += static_cast<float>(f[i] * std::exp(-6.9 * (static_cast<double>(i) / kRate) / (t60 * band[2])));
                }
                for (size_t i = 0; i < n; ++i)
                    out[ch][i] *= static_cast<float>(std::clamp((static_cast<double>(i) / kRate - preDelay) / 0.03, 0.0, 1.0));
                for (const auto &r : kReflections)
                {
                    const size_t at = static_cast<size_t>((r[0] + jitter(rng)) * kRate);
                    if (at < n)
                        out[ch][at] += static_cast<float>(r[1] * 8.0);
                }
                m = std::max(m, peak(out[ch]));
            }
            for (auto &ch : out)
                for (float &v : ch)
                    v /= m;
            return out;
        }

        // Swell (kVoiceAt s of reversed reverb rising into the voice) + the voice in its room, faded out after it.
        // dry: per channel (two), peak 1.
        void finish(const std::vector<Mono> &dry, size_t bodyFrames, double t60, double preDelay, unsigned seed, float wet, Stereo &out)
        {
            const std::vector<Mono> room = impulse(t60, preDelay, seed);
            const std::vector<Mono> swellRoom = impulse(1.8, 0.0, 31);
            std::vector<Mono> wetSig(2), swell(2);
            double dryEnergy = 0.0, wetEnergy = 0.0;
            size_t dryCount = 0, wetCount = 0;
            for (int ch = 0; ch < 2; ++ch)
            {
                wetSig[ch] = convolve(dry[ch], room[ch]);
                for (float v : dry[ch])
                    dryEnergy += static_cast<double>(v) * v;
                for (float v : wetSig[ch])
                    wetEnergy += static_cast<double>(v) * v;
                dryCount += dry[ch].size();
                wetCount += wetSig[ch].size();
                Mono reversed(dry[ch].rbegin(), dry[ch].rend());
                Mono full = convolve(reversed, swellRoom[ch]);
                std::reverse(full.begin(), full.end());
                const size_t lh = swellRoom[ch].size(), ns = static_cast<size_t>(kVoiceAt * kRate);
                swell[ch].assign(full.begin() + (lh - ns), full.begin() + lh);
            }
            const double dryRms = std::sqrt(dryEnergy / dryCount);
            const float wetGain = static_cast<float>(dryRms / std::max(std::sqrt(wetEnergy / wetCount), 1e-12));
            const size_t ns = swell[0].size(), tail = static_cast<size_t>(0.05 * kRate);
            double swellEnergy = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (size_t i = ns - tail; i < ns; ++i)
                    swellEnergy += static_cast<double>(swell[ch][i]) * swell[ch][i];
            const float swellGain = static_cast<float>(dryRms / std::max(std::sqrt(swellEnergy / (2 * tail)), 1e-12) * 0.5);
            out.lr.assign((ns + bodyFrames) * 2, 0.0f);
            for (int ch = 0; ch < 2; ++ch)
            {
                for (size_t i = 0; i < ns; ++i)
                {
                    const float ramp = static_cast<float>(i) / ns;
                    out.lr[i * 2 + ch] = swell[ch][i] * swellGain * ramp * ramp;
                }
                for (size_t i = 0; i < bodyFrames; ++i)
                {
                    float v = 0.0f;
                    if (i < dry[ch].size())
                        v += (1.0f - wet) * dry[ch][i];
                    if (i < wetSig[ch].size())
                        v += wet * wetSig[ch][i] * wetGain;
                    out.lr[(ns + i) * 2 + ch] = 0.95f * v;
                }
            }
            // Everything fades out over the last 0.45 s (the bark ends right after the last word, K).
            const size_t fo = static_cast<size_t>(0.45 * kRate), frames = out.frames();
            for (size_t i = 0; i < fo && i < frames; ++i)
            {
                const float g = std::pow(static_cast<float>(i) / fo, 1.5f);
                out.lr[(frames - 1 - i) * 2] *= g;
                out.lr[(frames - 1 - i) * 2 + 1] *= g;
            }
            float m = 0.0f;
            for (float v : out.lr)
                m = std::max(m, std::fabs(v));
            if (m > 0.98f)
                for (float &v : out.lr)
                    v *= 0.98f / m;
        }
    }

    int count() { return static_cast<int>(sizeof(kLines) / sizeof(kLines[0])); }
    const char *name(int line) { return line >= 0 && line < count() ? kLines[line].name : ""; }

    bool render(int line, const ReadFile &read, Stereo &out)
    {
        out.lr.clear();
        if (line < 0 || line >= count())
            return false;
        const Line &l = kLines[line];
        std::vector<uint8_t> afs;
        Mono v;
        if (!read(l.file, afs) || !decodeAdx(afs, l.entry, l.first, l.end, v) || v.empty())
            return false;
        fade(v.data(), std::min<size_t>(v.size(), static_cast<size_t>((l.treatment == Treatment::Cutscene ? 0.006 : 0.008) * kRate)), true);
        fade(v.data() + v.size() - std::min<size_t>(v.size(), static_cast<size_t>(l.fadeOut * kRate)),
             std::min<size_t>(v.size(), static_cast<size_t>(l.fadeOut * kRate)), false);
        normalize(v);
        std::vector<Mono> dry;
        double t60 = 2.0, preDelay = 0.04, speedUsed = 1.0;
        float wet = 0.24f;
        switch (l.treatment)
        {
        case Treatment::NarratorA:
        {
            speedUsed = 0.84;
            const Mono a = compress(warm(addBass(speed(v, speedUsed), 3.0), 1.6f), -22, 4);
            dry = {a, a};
            t60 = 3.0;
            wet = 0.32f;
            break;
        }
        case Treatment::NarratorB:
        {
            speedUsed = 0.75;
            const Mono b = compress(warm(addBass(speed(v, speedUsed), 6.0), 2.4f), -20, 5);
            // Doubled: two slightly detuned, slightly late copies, one each side.
            const Mono hi = speed(b, 1.004), lo = speed(b, 0.996);
            const size_t dl = static_cast<size_t>(0.013 * kRate), dr = static_cast<size_t>(0.021 * kRate);
            const size_t n = std::max({b.size(), hi.size() + dl, lo.size() + dr});
            dry.assign(2, Mono(n, 0.0f));
            for (size_t i = 0; i < b.size(); ++i)
                dry[0][i] = dry[1][i] = b[i];
            for (size_t i = 0; i < hi.size(); ++i)
                dry[0][i + dl] += 0.45f * hi[i];
            for (size_t i = 0; i < lo.size(); ++i)
                dry[1][i + dr] += 0.45f * lo[i];
            t60 = 3.6;
            wet = 0.36f;
            break;
        }
        case Treatment::NarratorD:
        {
            const Mono d = compress(warm(addBass(v, 1.0), 1.3f), -22, 3);
            dry = {d, d};
            t60 = 2.0;
            wet = 0.24f;
            break;
        }
        case Treatment::Cutscene:
        {
            Mono c = filter(filter(v, Kind::HighPass, 70), Kind::HighPass, 70);
            normalize(c);
            c = compress(warm(c, 1.3f), -22, 3);
            dry = {c, c};
            t60 = 2.0;
            preDelay = 0.03;
            wet = 0.24f;
            break;
        }
        }
        float m = std::max(peak(dry[0]), peak(dry[1]));
        for (auto &ch : dry)
            for (float &s : ch)
                s /= std::max(m, 1e-9f);
        // The room rings on 0.55 s past the words, then fades (the narrator's length is set by his line at its speed).
        const size_t words = l.treatment == Treatment::Cutscene ? dry[0].size()
                                                                 : static_cast<size_t>(v.size() / speedUsed);
        const unsigned seed = l.treatment == Treatment::Cutscene ? 7u : 11u;
        finish(dry, words + static_cast<size_t>(0.55 * kRate), t60, preDelay, seed, wet, out);
        return true;
    }

    void prepareWash(Wash &wash)
    {
        // A long dark room for the music (4.5 s, low-passed at 3.2 kHz, +3 dB at 200 Hz).
        std::vector<Mono> room = impulse(4.5, 0.03, 21);
        for (auto &ch : room)
            ch = filter(filter(ch, Kind::LowPass, 3200), Kind::LowShelf, 200, 3, 0.7);
        // The tail is never longer than kMaxTail, so the room past it can't reach it: cut there. The send lasts 1.4 s.
        for (auto &ch : room)
            ch.resize(std::min(ch.size(), static_cast<size_t>(kMaxTail * kRate)));
        wash.fftSize = pow2(static_cast<size_t>(1.4 * kRate) + room[0].size());
        wash.spectrum.assign(2, {});
        for (int ch = 0; ch < 2; ++ch)
        {
            std::vector<std::complex<double>> a(wash.fftSize);
            for (size_t i = 0; i < room[ch].size(); ++i)
                a[i] = room[ch][i];
            fft(a, false);
            wash.spectrum[ch].resize(wash.fftSize * 2);
            for (size_t i = 0; i < wash.fftSize; ++i)
            {
                wash.spectrum[ch][i * 2] = a[i].real();
                wash.spectrum[ch][i * 2 + 1] = a[i].imag();
            }
        }
    }

    void musicTail(const int16_t *music, size_t frames, int rate, size_t from, float end, const Wash &wash, Stereo &out)
    {
        out.lr.clear();
        if (music == nullptr || frames == 0 || rate <= 0 || wash.fftSize == 0 || end <= 0.0f)
            return;
        end = std::min(end, kMaxTail);
        const size_t n = static_cast<size_t>(end * kRate);
        const double step = static_cast<double>(rate) / kRate;
        // The music from the press on (3 s at most, the loop wrapping) and the 4 s before it, at 48 kHz.
        const size_t ahead = static_cast<size_t>(end * rate) + 64, before = static_cast<size_t>(4 * rate);
        auto channel = [&](int ch, size_t start, size_t count) {
            std::vector<float> src(count);
            for (size_t i = 0; i < count; ++i)
                src[i] = music[((start + i) % frames) * 2 + ch] / 32768.0f;
            return src;
        };
        const size_t tailFrames = std::min(n, static_cast<size_t>(kMaxTail * kRate));
        const size_t send = static_cast<size_t>(1.4 * kRate), N = wash.fftSize;
        out.lr.assign(n * 2, 0.0f);
        double preEnergy = 0.0;
        // Both channels' sends in one transform: left real, right imaginary.
        std::vector<std::complex<double>> a(N);
        for (int ch = 0; ch < 2; ++ch)
        {
            const std::vector<float> prev = channel(ch, (from + frames * 4 - before) % frames, before);
            preEnergy += rms(prev.data(), prev.size()) * rms(prev.data(), prev.size());
            const std::vector<float> src = channel(ch, from, ahead);
            Mono m = resample(src.data(), src.size(), step, tailFrames);
            // The dry music closes down: a low-pass sweeping 16 kHz -> 400 Hz over 0.9 s.
            Biquad lp(Kind::LowPass, 16000);
            for (size_t i = 0; i < m.size(); ++i)
            {
                if (i % 256 == 0)
                {
                    const double t = static_cast<double>(i) / kRate;
                    const double fc = t >= 0.9 ? 400.0 : 16000.0 * std::pow(400.0 / 16000.0, t / 0.9);
                    const Biquad next(Kind::LowPass, fc);
                    lp.b0 = next.b0; lp.b1 = next.b1; lp.b2 = next.b2; lp.a1 = next.a1; lp.a2 = next.a2;
                }
                m[i] = lp(m[i]);
            }
            // Its send into the room fades over 1.4 s; the dry music is gone by 0.6 s.
            for (size_t i = 0; i < std::min(send, m.size()); ++i)
            {
                const double v = m[i] * (1.0 - static_cast<double>(i) / send);
                a[i] = ch == 0 ? std::complex<double>(v, a[i].imag()) : std::complex<double>(a[i].real(), v);
            }
            for (size_t i = 0; i < m.size() && i < n; ++i)
            {
                const double g = std::max(0.0, 1.0 - static_cast<double>(i) / (0.6 * kRate));
                out.lr[i * 2 + ch] = static_cast<float>(m[i] * std::pow(g, 1.5));
            }
        }
        fft(a, false);
        std::vector<std::complex<double>> y(N);
        const std::complex<double> j(0.0, 1.0);
        for (size_t k = 0; k < N; ++k)
        {
            const std::complex<double> ak = a[k], an = std::conj(a[(N - k) % N]);
            const std::complex<double> left = (ak + an) * 0.5, right = (ak - an) * (-0.5 * j);
            const std::complex<double> h0(wash.spectrum[0][k * 2], wash.spectrum[0][k * 2 + 1]);
            const std::complex<double> h1(wash.spectrum[1][k * 2], wash.spectrum[1][k * 2 + 1]);
            y[k] = left * h0 + j * (right * h1);
        }
        fft(y, true);
        std::vector<Mono> washed(2, Mono(n, 0.0f));
        double washEnergy = 0.0;
        const size_t probe = std::min(n, static_cast<size_t>(1.5 * kRate));
        for (size_t i = 0; i < n && i < N; ++i)
        {
            washed[0][i] = static_cast<float>(y[i].real());
            washed[1][i] = static_cast<float>(y[i].imag());
            if (i < probe)
                washEnergy += y[i].real() * y[i].real() + y[i].imag() * y[i].imag();
        }
        const size_t washCount = probe * 2;
        // The wash sits at 0.75x the music's level before the press.
        const double washGain = std::sqrt(preEnergy / 2.0) / std::max(std::sqrt(washEnergy / std::max<size_t>(washCount, 1)), 1e-12) * 0.75;
        for (int ch = 0; ch < 2; ++ch)
            for (size_t i = 0; i < n; ++i)
                out.lr[i * 2 + ch] += static_cast<float>(washed[ch][i] * washGain);
        const size_t fo = std::min(n, static_cast<size_t>(0.45 * kRate));
        for (size_t i = 0; i < fo; ++i)
        {
            const float g = std::pow(static_cast<float>(i) / fo, 1.5f);
            out.lr[(n - 1 - i) * 2] *= g;
            out.lr[(n - 1 - i) * 2 + 1] *= g;
        }
        for (float &v : out.lr)
            v = std::clamp(v, -1.0f, 1.0f);
    }
}
