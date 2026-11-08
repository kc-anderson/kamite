// MOT motions edited in place (mot_edit.h). Moved here from npc_poses.cpp (Nov 8) for punish_fit to share.

#include "kamite/mot_edit.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace kamite::mot
{
    // PlatinumGames' 16-bit float (mot.py half): sign 1, exponent 6 (bias 47), mantissa 9.
    float halfToFloat(uint16_t v)
    {
        const int expo = (v >> 9) & 0x3F, mant = v & 0x1FF;
        if (expo == 0 && mant == 0)
            return 0.0f;
        const float value = std::ldexp(1.0f + mant / 512.0f, expo - 47);
        return (v & 0x8000u) ? -value : value;
    }
    uint16_t floatToHalf(float x)
    {
        if (x == 0.0f || !std::isfinite(x))
            return 0;
        const uint16_t sign = x < 0.0f ? 0x8000u : 0u;
        const float a = std::fabs(x);
        int e = static_cast<int>(std::floor(std::log2(a)));
        int m = static_cast<int>(std::lround((a / std::ldexp(1.0f, e) - 1.0f) * 512.0f));
        if (m >= 512)
        {
            m = 0;
            ++e;
        }
        const int expo = std::clamp(e + 47, 1, 62);
        return static_cast<uint16_t>(sign | (expo << 9) | (m & 0x1FF));
    }

    // The motion's length from its record table (the last curve block's end), 0 if it is not a motion.
    uint32_t motionLength(const uint8_t *m, uint32_t available)
    {
        if (available < 8 || std::memcmp(m, "mtb3", 4) != 0)
            return 0;
        const uint32_t records = m[6];
        uint32_t end = 8 + 12 * records;
        if (end > available)
            return 0;
        for (uint32_t i = 0; i < records; ++i)
        {
            const uint8_t *r = m + 8 + 12 * i;
            uint16_t count;
            uint32_t payload;
            std::memcpy(&count, r + 2, 2);
            std::memcpy(&payload, r + 8, 4);
            if (r[1] != 0xFF && (r[1] & 0xF0) == 0x10)
                end = std::max(end, payload + 12u + 4u * count);
        }
        return end <= available ? end : 0;
    }



    bool parse(const std::vector<uint8_t> &m, std::vector<Rec> &recs)
    {
        if (m.size() < 8)
            return false;
        const uint32_t n = m[6];
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint8_t *r = m.data() + 8 + 12 * i;
            if (r[1] == 0xFF)
                continue;
            Rec rec;
            rec.bone = r[0];
            rec.channel = r[1];
            std::memcpy(&rec.count, r + 2, 2);
            std::memcpy(&rec.flags, r + 4, 4);
            uint32_t payload;
            std::memcpy(&payload, r + 8, 4);
            if ((r[1] & 0xF0) == 0x10)
            {
                const uint32_t size = 12u + 4u * rec.count;
                if (payload + size > m.size())
                    return false;
                rec.block.assign(m.begin() + payload, m.begin() + payload + size);
            }
            else
                std::memcpy(&rec.constant, &payload, 4);
            recs.push_back(std::move(rec));
        }
        return true;
    }

    std::vector<uint8_t> serialize(uint16_t frames, uint8_t loop, const std::vector<Rec> &recs)
    {
        const uint32_t n = static_cast<uint32_t>(recs.size()) + 1;
        std::vector<uint8_t> out(8 + 12 * n);
        std::memcpy(out.data(), "mtb3", 4);
        std::memcpy(out.data() + 4, &frames, 2);
        out[6] = static_cast<uint8_t>(n);
        out[7] = loop;
        for (size_t i = 0; i < recs.size(); ++i)
        {
            uint8_t *r = out.data() + 8 + 12 * i;
            const Rec &rec = recs[i];
            r[0] = rec.bone;
            r[1] = rec.channel;
            std::memcpy(r + 2, &rec.count, 2);
            std::memcpy(r + 4, &rec.flags, 4);
            uint32_t payload;
            if ((rec.channel & 0xF0) == 0x10)
            {
                payload = static_cast<uint32_t>(out.size());
                out.insert(out.end(), rec.block.begin(), rec.block.end());
                std::memcpy(out.data() + 8 + 12 * i + 8, &payload, 4);
            }
            else
            {
                std::memcpy(&payload, &rec.constant, 4);
                std::memcpy(r + 8, &payload, 4);
            }
        }
        uint8_t *end = out.data() + 8 + 12 * recs.size();
        end[0] = 127;
        end[1] = 0xFF;
        end[2] = end[3] = 0xFF;
        return out;
    }

    static float hermite(float p0, float m0, float p1, float m1, float t)
    {
        const float t2 = t * t, t3 = t2 * t;
        return (2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * m1;
    }

    float sample(const Rec &rec, float t)
    {
        if ((rec.channel & 0xF0) != 0x10)
            return rec.constant;
        const uint16_t *h = reinterpret_cast<const uint16_t *>(rec.block.data());
        const float pos = halfToFloat(h[0]), dpos = halfToFloat(h[1]), t0 = halfToFloat(h[2]), dt0 = halfToFloat(h[3]),
                    t1 = halfToFloat(h[4]), dt1 = halfToFloat(h[5]);
        int frame = 0, prevFrame = 0;
        float prevV = 0, prevOut = 0;
        for (uint16_t k = 0; k < rec.count; ++k)
        {
            const uint8_t *key = rec.block.data() + 12 + 4 * k;
            frame += key[0];
            const float v = pos + dpos * key[1], in = t0 + dt0 * key[2], out = t1 + dt1 * key[3];
            if (k == 0 && t < frame)
                return v;
            if (k > 0 && t < frame)
                return frame == prevFrame ? v : hermite(prevV, prevOut, v, in, (t - prevFrame) / (frame - prevFrame));
            prevFrame = frame;
            prevV = v;
            prevOut = out;
        }
        return prevV;
    }

    static uint16_t halfAtMost(float x)
    {
        uint16_t h = floatToHalf(x);
        for (int i = 0; i < 8 && halfToFloat(h) > x; ++i)
            h = floatToHalf(x - std::fabs(x) * 0.004f * (i + 1) - 1e-6f);
        return h;
    }

    // pos + delta * k (k 0..255) over [lo, hi]
    static void range(float lo, float hi, uint16_t &posBits, uint16_t &deltaBits)
    {
        posBits = halfAtMost(lo);
        const float pos = halfToFloat(posBits);
        deltaBits = 0;
        if (hi - pos <= 1e-7f)
            return;
        float d = (hi - pos) / 255.0f;
        for (int i = 0; i < 16; ++i)
        {
            deltaBits = floatToHalf(d);
            if (pos + halfToFloat(deltaBits) * 255.0f >= hi)
                break;
            d *= 1.004f;
        }
    }

    static uint8_t step(float v, uint16_t posBits, uint16_t deltaBits)
    {
        const float pos = halfToFloat(posBits), d = halfToFloat(deltaBits);
        if (d == 0.0f)
            return 0;
        return static_cast<uint8_t>(std::clamp(std::lround((v - pos) / d), 0L, 255L));
    }

    // A looping curve through `v` (one value a frame), keys every 3 frames (more apart past 120) and on the last;
    // `every` > 0 sets the spacing (the pinned arms: denser, so the hands hold still between keys).
    Rec encode(uint8_t bone, uint8_t component, const std::vector<float> &v, int every, bool loop)
    {
        Rec rec;
        rec.bone = bone;
        // Always a curve, never a constant, even when it does not move: the motion's records stay the same
        // whatever the sliders say (edits in place).
        rec.channel = static_cast<uint8_t>(0x10 | component);
        const int n = static_cast<int>(v.size());
        std::vector<int> frames;
        if (every <= 0)
            every = std::max(3, n / 24); // a long motion keeps its slot size in hand
        for (int f = 0; f < n - 1; f += every)
            frames.push_back(f);
        frames.push_back(n - 1);
        std::vector<float> vals, ins, outs;
        for (size_t i = 0; i < frames.size(); ++i)
        {
            const int f = frames[i];
            const float slope = loop ? 0.5f * (v[(f + 1) % n] - v[(f - 1 + n) % n])
                                 : (f == 0 ? v[std::min(1, n - 1)] - v[0]
                                    : f == n - 1 ? v[n - 1] - v[n - 2] : 0.5f * (v[f + 1] - v[f - 1]));
            const int lin = i > 0 ? f - frames[i - 1] : 1, lout = i + 1 < frames.size() ? frames[i + 1] - f : 1;
            vals.push_back(v[f]);
            ins.push_back(slope * lin);
            outs.push_back(slope * lout);
        }
        uint16_t h[6];
        range(*std::min_element(vals.begin(), vals.end()), *std::max_element(vals.begin(), vals.end()), h[0], h[1]);
        range(*std::min_element(ins.begin(), ins.end()), *std::max_element(ins.begin(), ins.end()), h[2], h[3]);
        range(*std::min_element(outs.begin(), outs.end()), *std::max_element(outs.begin(), outs.end()), h[4], h[5]);
        rec.block.resize(12);
        std::memcpy(rec.block.data(), h, 12);
        int prev = 0;
        for (size_t i = 0; i < frames.size(); ++i)
        {
            rec.block.push_back(static_cast<uint8_t>(frames[i] - prev));
            rec.block.push_back(step(vals[i], h[0], h[1]));
            rec.block.push_back(step(ins[i], h[2], h[3]));
            rec.block.push_back(step(outs[i], h[4], h[5]));
            prev = frames[i];
        }
        rec.count = static_cast<uint16_t>(frames.size());
        return rec;
    }

    // delta[f] added to the bone's component every frame; the record becomes a curve (always, so the motion's
    // records stay the same whatever the sliders say).
    void addCurve(std::vector<uint8_t> &m, int bone, int component, const std::vector<float> &delta, int every)
    {
        std::vector<Rec> recs;
        if (!parse(m, recs))
            return;
        uint16_t frames;
        std::memcpy(&frames, m.data() + 4, 2);
        const uint8_t loop = m[7];
        const Rec *own = nullptr;
        size_t where = recs.size();
        for (size_t i = 0; i < recs.size(); ++i)
            if (recs[i].bone == bone)
            {
                if (where == recs.size())
                    where = i;
                if ((recs[i].channel & 0x0F) == component)
                {
                    own = &recs[i];
                    where = i;
                }
            }
        std::vector<float> v(frames);
        for (int f = 0; f < frames; ++f)
            v[f] = (own ? sample(*own, static_cast<float>(f)) : 0.0f) + delta[f % delta.size()];
        std::vector<Rec> out;
        for (size_t i = 0; i < recs.size(); ++i)
        {
            if (i == where)
                out.push_back(encode(static_cast<uint8_t>(bone), static_cast<uint8_t>(component), v, every, loop != 0));
            if (&recs[i] != own)
                out.push_back(recs[i]);
        }
        if (where == recs.size())
            out.push_back(encode(static_cast<uint8_t>(bone), static_cast<uint8_t>(component), v, every, loop != 0));
        if (out.size() < 254)
            m = serialize(frames, loop, out);
    }
    Mat mul(const Mat &a, const Mat &b)
    {
        Mat r{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
        return r;
    }
    Mat rotX(float a) { const float c = std::cos(a), s = std::sin(a); return {1, 0, 0, 0, c, -s, 0, s, c}; }
    Mat rotY(float a) { const float c = std::cos(a), s = std::sin(a); return {c, 0, s, 0, 1, 0, -s, 0, c}; }
    Mat rotZ(float a) { const float c = std::cos(a), s = std::sin(a); return {c, -s, 0, s, c, 0, 0, 0, 1}; }
    Mat euler(float x, float y, float z) { return mul(rotZ(z), mul(rotY(y), rotX(x))); }
    void toEuler(const Mat &r, float &x, float &y, float &z)
    {
        y = std::asin(std::clamp(-r[6], -1.0f, 1.0f));
        x = std::atan2(r[7], r[8]);
        z = std::atan2(r[3], r[0]);
    }
    float nearAngle(float a, float ref)
    {
        constexpr float tau = 6.28318530718f;
        while (a - ref > 3.14159265f)
            a -= tau;
        while (a - ref < -3.14159265f)
            a += tau;
        return a;
    }

    void preRotate(std::vector<uint8_t> &m, int bone, const std::vector<Mat> &pre, int every)
    {
        std::vector<Rec> recs;
        if (pre.empty() || !parse(m, recs))
            return;
        uint16_t frames;
        std::memcpy(&frames, m.data() + 4, 2);
        const uint8_t loop = m[7];
        const Rec *comp[3] = {};
        for (const Rec &r : recs)
            if (r.bone == bone && (r.channel & 0x0F) >= 3 && (r.channel & 0x0F) <= 5)
                comp[(r.channel & 0x0F) - 3] = &r;
        std::vector<float> xs(frames), ys(frames), zs(frames);
        for (int f = 0; f < frames; ++f)
        {
            float a[3];
            for (int c = 0; c < 3; ++c)
                a[c] = comp[c] ? sample(*comp[c], static_cast<float>(f)) : 0.0f;
            // Of the two Euler triples of the new rotation, the one nearest the motion's own at this frame (its winding
            // and branch: a small pre-rotation stays a small change of each curve).
            float x, y, z;
            toEuler(mul(pre[std::min<size_t>(f, pre.size() - 1)], euler(a[0], a[1], a[2])), x, y, z);
            constexpr float pi = 3.14159265358979f;
            const float p[3] = {nearAngle(x, a[0]), nearAngle(y, a[1]), nearAngle(z, a[2])};
            const float q[3] = {nearAngle(x + pi, a[0]), nearAngle(pi - y, a[1]), nearAngle(z + pi, a[2])};
            const float dp = std::fabs(p[0] - a[0]) + std::fabs(p[1] - a[1]) + std::fabs(p[2] - a[2]);
            const float dq = std::fabs(q[0] - a[0]) + std::fabs(q[1] - a[1]) + std::fabs(q[2] - a[2]);
            const float *pick = dq < dp ? q : p;
            xs[f] = pick[0];
            ys[f] = pick[1];
            zs[f] = pick[2];
        }
        // The bone's rotation records replaced where they were (records keep the motion's own order: root motion,
        // bone 255, comes first; a motion whose records go out of that order breaks in the game, Nov 8). A bone
        // without rotation records gets them after its other records, or before the next bone up.
        std::vector<Rec> out;
        bool placed = false;
        const auto place = [&] {
            out.push_back(encode(static_cast<uint8_t>(bone), 3, xs, every, loop != 0));
            out.push_back(encode(static_cast<uint8_t>(bone), 4, ys, every, loop != 0));
            out.push_back(encode(static_cast<uint8_t>(bone), 5, zs, every, loop != 0));
            placed = true;
        };
        const bool hasRotation = comp[0] || comp[1] || comp[2];
        int lastOfBone = -1;
        for (size_t i = 0; i < recs.size(); ++i)
            if (recs[i].bone == bone)
                lastOfBone = static_cast<int>(i);
        for (size_t i = 0; i < recs.size(); ++i)
        {
            const Rec &r = recs[i];
            const int ch = r.channel & 0x0F;
            if (r.bone == bone && ch >= 3 && ch <= 5)
            {
                if (!placed)
                    place();
                continue;
            }
            if (!placed && !hasRotation && lastOfBone < 0 && r.bone > bone && r.bone != 255)
                place();
            out.push_back(r);
            if (!placed && !hasRotation && static_cast<int>(i) == lastOfBone)
                place();
        }
        if (!placed)
            place();
        if (out.size() < 254)
            m = serialize(frames, loop, out);
    }
}
