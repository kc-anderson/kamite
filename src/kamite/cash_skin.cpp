#include "kamite/cash_skin.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace kamite::cash_skin
{
    namespace
    {
        uint32_t u32(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
        uint16_t u16(const uint8_t *p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
        float f32(const uint8_t *p) { float v; std::memcpy(&v, p, 4); return v; }
    }

    // An scr: "scr\0", kind 3, node count, node offsets at +16; a node record's +0 is its mdb, relative to the record.
    // An mdb: "mdb\0", +4 its bone table (16 bytes a bone: x, y, z, mirror, parent), +8 bone count, +10 vertex buffer
    // count, +30 0 for room geometry, +32 the vertex buffers' offsets. A vertex buffer: offsets of its positions (x, y,
    // z f32 + flag), normals, UVs, colours and weights (8 bytes a vertex: joints 0, j1, j2, j3 as bone x 4, then the
    // weights in percent, filled in order), +20 its vertex count. (tools/models/gamemodel.py)
    size_t reskin(uint8_t *scr, size_t size, size_t &lo, size_t &hi)
    {
        lo = size;
        hi = 0;
        auto inside = [&](size_t at, size_t bytes) { return at <= size && bytes <= size - at; };
        if (size < 16 || std::memcmp(scr, "scr", 4) != 0 || u32(scr + 4) != 3u)
            return 0;
        const uint32_t nodes = u32(scr + 8);
        if (nodes == 0 || nodes > 256 || !inside(16, 4u * nodes))
            return 0;
        auto mdbOf = [&](uint32_t i) -> size_t {
            const uint32_t node = u32(scr + 16 + 4 * i);
            if (!inside(node, 112))
                return 0;
            int32_t rel;
            std::memcpy(&rel, scr + node, 4);
            const int64_t at = static_cast<int64_t>(node) + rel;
            if (at <= 0 || !inside(static_cast<size_t>(at), 32) || std::memcmp(scr + at, "mdb", 4) != 0)
                return 0;
            return static_cast<size_t>(at);
        };

        // The skeleton (the first mesh's copy of the bone table): the female test, and the bind positions.
        const size_t first = mdbOf(0);
        if (first == 0)
            return 0;
        const uint16_t bones = u16(scr + first + 8);
        const size_t table = first + u32(scr + first + 4);
        if (bones < 30 || bones > 64 || !inside(table, 16u * bones))
            return 0;
        float bind[64][3];
        int parent[64];
        for (int b = 0; b < bones; ++b)
        {
            const uint8_t *e = scr + table + 16 * b;
            int16_t p;
            std::memcpy(&p, e + 14, 2);
            parent[b] = p;
            for (int k = 0; k < 3; ++k)
                bind[b][k] = f32(e + 4 * k) + (p >= 0 && p < b ? bind[p][k] : 0.0f);
        }
        if (parent[29] != 2 || parent[kPelvis] != 0 || parent[kThighA] != kPelvis || parent[kThighB] != kPelvis ||
            parent[kCheekA] != -1 || parent[kCheekB] != -1)
            return 0;
        const float hipX = bind[kThighA][0] - bind[kPelvis][0];
        const float h = std::fabs(hipX) / kHipRef;
        if (h < 0.5f || h > 3.0f)
            return 0;
        const float sideA = hipX < 0.0f ? -1.0f : 1.0f; // bone 18's side of the centre line: cheek 27
        const float bandLow = bind[kThighA][1] - bind[kPelvis][1] - kBandBelow * h, bandHigh = kBandAbove * h;

        // 27 and 28 must be free in every mesh before anything moves.
        std::set<size_t> meshes;
        for (uint32_t i = 0; i < nodes; ++i)
            if (const size_t m = mdbOf(i))
                meshes.insert(m);
        struct Buffer { size_t pos, wt; uint32_t count; };
        std::vector<Buffer> buffers;
        for (const size_t m : meshes)
        {
            if (u16(scr + m + 30) == 0) // room geometry: int16 positions, no weights for us
                continue;
            const uint16_t vbs = u16(scr + m + 10);
            if (!inside(m + 32, 4u * vbs))
                return 0;
            for (uint16_t v = 0; v < vbs; ++v)
            {
                const size_t vb = m + u32(scr + m + 32 + 4 * v);
                if (!inside(vb, 32))
                    return 0;
                const uint32_t po = u32(scr + vb), wo = u32(scr + vb + 16), count = u16(scr + vb + 20);
                if (wo == 0)
                    continue;
                if (!inside(vb + po, 16u * count) || !inside(vb + wo, 8u * count))
                    return 0;
                for (uint32_t k = 0; k < count; ++k)
                {
                    const uint8_t *w = scr + vb + wo + 8 * k;
                    for (int s = 0; s < 3; ++s)
                        if (w[4 + s] != 0 && (w[1 + s] == kCheekA * 4 || w[1 + s] == kCheekB * 4))
                            return 0;
                }
                buffers.push_back({vb + po, vb + wo, count});
            }
        }

        size_t moved = 0;
        for (const Buffer &b : buffers)
            for (uint32_t k = 0; k < b.count; ++k)
            {
                uint8_t *w = scr + b.wt + 8 * k;
                int slot = -1, used = 0;
                for (int s = 0; s < 3; ++s)
                    if (w[4 + s] != 0)
                    {
                        ++used;
                        if (w[1 + s] == kPelvis * 4)
                            slot = s;
                    }
                if (slot < 0)
                    continue;
                const uint8_t *p = scr + b.pos + 16 * k;
                const float x = f32(p) - bind[kPelvis][0], y = f32(p + 4) - bind[kPelvis][1], z = f32(p + 8) - bind[kPelvis][2];
                if (y < bandLow || y > bandHigh || z >= kPivotZ * h)
                    continue;
                if (std::fabs(x) < kCentre * h)
                {
                    if (used >= 3) // no slot for the second cheek: stays on the pelvis
                        continue;
                    const uint8_t all = w[4 + slot], half = static_cast<uint8_t>(all / 2);
                    w[1 + slot] = kCheekA * 4;
                    w[4 + slot] = static_cast<uint8_t>(all - half);
                    w[1 + used] = kCheekB * 4;
                    w[4 + used] = half;
                    if (half == 0) // a 1% pelvis weight: all of it to one cheek
                        w[1 + used] = 0;
                }
                else
                    w[1 + slot] = static_cast<uint8_t>((x * sideA > 0.0f ? kCheekA : kCheekB) * 4);
                lo = std::min(lo, static_cast<size_t>(b.wt + 8 * k));
                hi = std::max(hi, static_cast<size_t>(b.wt + 8 * k + 8));
                ++moved;
            }
        return moved;
    }
}
