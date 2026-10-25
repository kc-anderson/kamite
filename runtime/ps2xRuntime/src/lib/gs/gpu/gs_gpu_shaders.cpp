#include "gs_gpu_shaders.h"

namespace GSGpuShaders
{
    const char *const kPresentHash = R"GLSL(
layout(local_size_x = 256) in;
layout(std430, binding = 7) readonly buffer DataBuffer { uint data[]; };
layout(std430, binding = 10) buffer HashBuffer { uint imageHash[2]; };
uniform uint uPixels;
shared uint sums[256];
shared uint xors[256];
uint mixHash(uint v)
{
    v ^= v >> 16u;
    v *= 0x7feb352du;
    v ^= v >> 15u;
    v *= 0x846ca68bu;
    return v ^ (v >> 16u);
}
void main()
{
    uint lane = gl_LocalInvocationID.x;
    uint i = gl_GlobalInvocationID.x;
    uint value = i < uPixels ? mixHash(data[i] ^ mixHash(i + 1u)) : 0u;
    sums[lane] = value;
    xors[lane] = i < uPixels ? mixHash(value ^ 0x9e3779b9u) : 0u;
    barrier();
    for (uint stride = 128u; stride > 0u; stride >>= 1u)
    {
        if (lane < stride)
        {
            sums[lane] += sums[lane + stride];
            xors[lane] ^= xors[lane + stride];
        }
        barrier();
    }
    if (lane == 0u)
    {
        atomicAdd(imageHash[0], sums[0]);
        atomicXor(imageHash[1], xors[0]);
    }
}
)GLSL";
    const char *const kPresentBlit = R"GLSL(
layout(local_size_x = 16, local_size_y = 16) in;
layout(std430, binding = 7) readonly buffer DataBuffer { uint data[]; };
layout(rgba8, binding = 0) writeonly uniform image2D uFrame;
uniform uvec4 uSize;
void main()
{
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= uSize.x || p.y >= uSize.y)
        return;
    imageStore(uFrame, ivec2(p), unpackUnorm4x8(data[p.y * uSize.x + p.x]));
}
)GLSL";
    const char *const kCommon = R"GLSL(

layout(std430, binding = 0) buffer VramBuffer { uint vram[]; };
layout(std430, binding = 1) readonly buffer SwizzleBuffer { uint swz[]; };
layout(std430, binding = 2) buffer ClutBuffer { uint clut[]; };
layout(std430, binding = 9) readonly buffer PageMapBuffer { uint pageMap[]; };

#if HIRES
// Internal resolution (PS2X_GS_SCALE above 1; at 1x the host strips every HIRES block): the VRAM buffer holds
// SCALE*SCALE sample planes of PLANE_WORDS words each. Plane (i + j*SCALE) is the whole GS memory as rendered with the
// sample point moved (i, j)/SCALE of a pixel; plane 0 is the native image, the one the game reads back. Uploads, copies
// and clears write every plane. SCALE is read at run time, so every scale shares the same compiled shaders.
// gsDebug (PS2X_GS_DEBUG_FLAGS, measurements only): 1 no texture reads, 2 shade without writing, 4 coverage only.
layout(std430, binding = 12) readonly buffer ScaleBuffer { uint gsScale; uint gsDebug; };
#define SCALE gsScale
layout(std430, binding = 11) readonly buffer HiresBuffer { uint hiresPages[]; };
uint gPlaneBase = 0u;
int gSubX = 0;
int gSubY = 0;
uint gTexPlane = 0u;
uint gPlane = 0u;
// Sprites set the texture position of the native pixel's corner: a nearest read of drawn content takes the texel the
// native pixel takes, in this sample's plane, so 1:1 copies stay exact whatever half-texel offset the game adds.
bool gCorner = false;
float gCornerU = 0.0;
float gCornerV = 0.0;
#define VRAM_INDEX(i) (gPlaneBase + (i))
#define SUB_X gSubX
#define SUB_Y gSubY
void setPlane(uint plane)
{
    gPlane = plane;
    gPlaneBase = plane * PLANE_WORDS;
    gSubX = int(((plane % SCALE) * 16u + SCALE / 2u) / SCALE);
    gSubY = int(((plane / SCALE) * 16u + SCALE / 2u) / SCALE);
}
#endif
const uint KIND_INVALID = 0u;
const uint KIND_32 = 1u;
const uint KIND_24 = 2u;
const uint KIND_16 = 3u;
const uint KIND_8 = 4u;
const uint KIND_4 = 5u;

uint fmtOf(uint psm)
{
    switch (psm & 0x3Fu)
    {
    case 0x00u: return 0u;
    case 0x01u: return 1u;
    case 0x02u: return 2u;
    case 0x0Au: return 3u;
    case 0x13u: return 4u;
    case 0x14u: return 5u;
    case 0x1Bu: return 6u;
    case 0x24u: return 7u;
    case 0x2Cu: return 8u;
    case 0x30u: return 9u;
    case 0x31u: return 10u;
    case 0x32u: return 11u;
    case 0x3Au: return 12u;
    }
    return 13u;
}

const uint FMT_SX[14] = uint[14](6u, 6u, 6u, 6u, 7u, 7u, 6u, 6u, 6u, 6u, 6u, 6u, 6u, 0u);
const uint FMT_SY[14] = uint[14](5u, 5u, 6u, 6u, 6u, 7u, 5u, 5u, 5u, 5u, 5u, 6u, 6u, 0u);
const uint FMT_US[14] = uint[14](5u, 5u, 4u, 4u, 3u, 2u, 5u, 5u, 5u, 5u, 5u, 4u, 4u, 0u);
const uint FMT_BO[14] = uint[14](0u, 0u, 0u, 0u, 0u, 0u, 24u, 24u, 28u, 0u, 0u, 0u, 0u, 0u);
const uint FMT_MASK[14] = uint[14](0x3FFFFCu, 0x3FFFFCu, 0x3FFFFEu, 0x3FFFFEu, 0x3FFFFFu, 0x3FFFFFu, 0x3FFFFFu, 0x3FFFFFu, 0x3FFFFFu, 0x3FFFFCu, 0x3FFFFCu, 0x3FFFFEu, 0x3FFFFEu, 0u);
const uint FMT_KIND[14] = uint[14](KIND_32, KIND_24, KIND_16, KIND_16, KIND_8, KIND_4, KIND_8, KIND_4, KIND_4, KIND_32, KIND_24, KIND_16, KIND_16, KIND_INVALID);
const uint FMT_TABLE[14] = uint[14](TABLE_C32, TABLE_C32, TABLE_C16, TABLE_C16S, TABLE_P8, TABLE_P4, TABLE_C32, TABLE_C32, TABLE_C32, TABLE_Z32, TABLE_Z32, TABLE_Z16, TABLE_Z16S, 0u);

uint swzAt(uint index)
{
    uint word = swz[index >> 1u];
    return (index & 1u) != 0u ? (word >> 16u) : (word & 0xFFFFu);
}

uvec2 locate(uint f, uint base, uint bw, uint x, uint y)
{
    uint sx = FMT_SX[f];
    uint sy = FMT_SY[f];
    uint pagesPerRow = (bw * 64u) >> sx;
    uint page = (base >> 5u) + (y >> sy) * pagesPerRow + (x >> sx);
    uint entry = ((base & 31u) << (sx + sy)) + ((y & ((1u << sy) - 1u)) << sx) + (x & ((1u << sx) - 1u));
    uint pixel = (page << (sx + sy)) + swzAt(FMT_TABLE[f] + entry);
    uint bits = (pixel << FMT_US[f]) + FMT_BO[f];
    return uvec2((bits >> 3u) & FMT_MASK[f], bits & 7u);
}

uint loadAt(uint f, uvec2 loc)
{
    uint word = vram[VRAM_INDEX(loc.x >> 2u)];
    switch (FMT_KIND[f])
    {
    case KIND_32: return word;
    case KIND_24: return word & 0x00FFFFFFu;
    case KIND_16: return (word >> ((loc.x & 2u) * 8u)) & 0xFFFFu;
    case KIND_8: return (word >> ((loc.x & 3u) * 8u)) & 0xFFu;
    case KIND_4: return (word >> ((loc.x & 3u) * 8u + loc.y)) & 0x0Fu;
    }
    return 0u;
}

void storeAt(uint f, uvec2 loc, uint value)
{
    uint index = VRAM_INDEX(loc.x >> 2u);
    switch (FMT_KIND[f])
    {
    case KIND_32:
        vram[index] = value;
        break;
    case KIND_24:
        vram[index] = (vram[index] & 0xFF000000u) | (value & 0x00FFFFFFu);
        break;
    case KIND_16:
    {
        uint shift = (loc.x & 2u) * 8u;
        atomicAnd(vram[index], ~(0xFFFFu << shift));
        atomicOr(vram[index], (value & 0xFFFFu) << shift);
        break;
    }
    case KIND_8:
    {
        uint shift = (loc.x & 3u) * 8u;
        atomicAnd(vram[index], ~(0xFFu << shift));
        atomicOr(vram[index], (value & 0xFFu) << shift);
        break;
    }
    case KIND_4:
    {
        uint shift = (loc.x & 3u) * 8u + loc.y;
        atomicAnd(vram[index], ~(0x0Fu << shift));
        atomicOr(vram[index], (value & 0x0Fu) << shift);
        break;
    }
    }
}

uint readPixel(uint psm, uint base, uint bw, uint x, uint y)
{
    uint f = fmtOf(psm);
    if (FMT_KIND[f] == KIND_INVALID)
        return 0u;
    return loadAt(f, locate(f, base, bw, x, y));
}

void writePixelRaw(uint psm, uint base, uint bw, uint x, uint y, uint value)
{
    uint f = fmtOf(psm);
    if (FMT_KIND[f] == KIND_INVALID)
        return;
    storeAt(f, locate(f, base, bw, x, y), value);
}

uint readPixelEpoch(uint psm, uint base, uint bw, uint x, uint y, uint epoch, uint finalEpoch)
{
    uint f = fmtOf(psm);
    if (FMT_KIND[f] == KIND_INVALID)
        return 0u;
    uvec2 loc = locate(f, base, bw, x, y);
    if (epoch < finalEpoch)
        loc.x = (pageMap[epoch * 512u + (loc.x >> 13u)] << 13u) | (loc.x & 0x1FFFu);
    return loadAt(f, loc);
}

uint bitsPerPixel(uint psm)
{
    switch (psm)
    {
    case 0x02u: case 0x0Au: case 0x32u: case 0x3Au: return 16u;
    case 0x13u: case 0x1Bu: return 8u;
    case 0x14u: case 0x24u: case 0x2Cu: return 4u;
    }
    return 32u;
}

uint clampU8(int v)
{
    return uint(clamp(v, 0, 255));
}

uint rgba5551To8888(uint c)
{
    uint r = (c & 0x1Fu) << 3u;
    uint g = ((c >> 5u) & 0x1Fu) << 3u;
    uint b = ((c >> 10u) & 0x1Fu) << 3u;
    uint a = ((c >> 15u) & 0x01u) << 7u;
    return r | (g << 8u) | (b << 16u) | (a << 24u);
}

uint rgba8888To5551(uint c)
{
    uint r = (c & 0xFFu) >> 3u;
    uint g = ((c >> 8u) & 0xFFu) >> 3u;
    uint b = ((c >> 16u) & 0xFFu) >> 3u;
    uint a = ((c >> 24u) & 0xFFu) >> 7u;
    return r | (g << 5u) | (b << 10u) | (a << 15u);
}

uint clutEntry(uint version, uint index)
{
    uint word = clut[version * 256u + (index >> 1u)];
    return (index & 1u) != 0u ? (word >> 16u) : (word & 0xFFFFu);
}

void clutStore(uint version, uint index, uint value)
{
    uint word = version * 256u + (index >> 1u);
    uint shift = (index & 1u) * 16u;
    atomicAnd(clut[word], ~(0xFFFFu << shift));
    atomicOr(clut[word], (value & 0xFFFFu) << shift);
}
)GLSL";

    const char *const kRaster = R"GLSL(
layout(local_size_x = 16, local_size_y = 16) in;

layout(std430, binding = 3) readonly buffer PrimBuffer { uint prims[]; };
layout(std430, binding = 4) readonly buffer StateBuffer { uint states[]; };
layout(std430, binding = 5) readonly buffer TileBuffer { uvec4 tiles[]; };
layout(std430, binding = 6) readonly buffer TileListBuffer { uint tileList[]; };
uniform uvec4 uRaster;

const uint PRIM_WORDS = 40u;
const uint STATE_WORDS = 32u;

const uint S_FBP = 0u;
const uint S_FBW = 1u;
const uint S_FPSM = 2u;
const uint S_FBMSK = 3u;
const uint S_ZBP = 4u;
const uint S_ZPSM = 5u;
const uint S_ZMASK = 6u;
const uint S_SCISSOR_X = 7u;
const uint S_SCISSOR_Y = 8u;
const uint S_TBP0 = 9u;
const uint S_TBW = 10u;
const uint S_TPSM = 11u;
const uint S_TEXMODE = 12u;
const uint S_CSA = 13u;
const uint S_TEXSIZE = 14u;
const uint S_CLAMP_LO = 15u;
const uint S_CLAMP_HI = 16u;
const uint S_ALPHA = 17u;
const uint S_ALPHA_FIX = 18u;
const uint S_TEST = 19u;
const uint S_FBA = 20u;
const uint S_FLAGS = 21u;
const uint S_TEXA = 22u;
const uint S_FOGCOL = 23u;
const uint S_CLUT = 24u;
const uint S_EPOCH = 25u;

const uint F_IIP = 1u;
const uint F_TME = 2u;
const uint F_FGE = 4u;
const uint F_ABE = 8u;
const uint F_FST = 16u;
const uint F_PABE = 32u;
const uint F_LINEAR = 64u;
const uint F_WRAP = 128u;

uint st(uint s, uint i) { return states[s * STATE_WORDS + i]; }
uint pw(uint p, uint i) { return prims[p * PRIM_WORDS + i]; }
float pf(uint p, uint i) { return uintBitsToFloat(prims[p * PRIM_WORDS + i]); }
double pd(uint p, uint i) { return packDouble2x32(uvec2(prims[p * PRIM_WORDS + i], prims[p * PRIM_WORDS + i + 1u])); }

bool passesAlphaTest(uint test, uint alpha)
{
    if ((test & 1u) == 0u)
        return true;
    uint atst = (test >> 1u) & 7u;
    uint aref = (test >> 4u) & 0xFFu;
    switch (atst)
    {
    case 0u: return false;
    case 1u: return true;
    case 2u: return alpha < aref;
    case 3u: return alpha <= aref;
    case 4u: return alpha == aref;
    case 5u: return alpha >= aref;
    case 6u: return alpha > aref;
    case 7u: return alpha != aref;
    }
    return true;
}

uvec3 classifyAlphaTest(uint test, uint alpha, uint framePsm)
{
    if (passesAlphaTest(test, alpha))
        return uvec3(1u, 1u, 1u);
    switch ((test >> 12u) & 3u)
    {
    case 1u: return uvec3(1u, 1u, 0u);
    case 2u: return uvec3(0u, 0u, 1u);
    case 3u:
        if (framePsm == 0u)
            return uvec3(1u, 0u, 0u);
        return uvec3(1u, 1u, 0u);
    }
    return uvec3(0u, 0u, 0u);
}

bool passesDestinationAlphaTest(uint test, uint framePsm, uint raw)
{
    if (((test >> 14u) & 1u) == 0u)
        return true;
    bool datm = ((test >> 15u) & 1u) != 0u;
    if (framePsm == 0u)
        return (((raw >> 31u) & 1u) != 0u) == datm;
    if (framePsm == 2u || framePsm == 10u)
        return (((raw >> 15u) & 1u) != 0u) == datm;
    return true;
}

uint applyTexa(uint s, uint psm, uint texel)
{
    if (psm == 0u)
        return texel;
    uint texa = st(s, S_TEXA);
    uint ta0 = texa & 0xFFu;
    bool aem = ((texa >> 8u) & 1u) != 0u;
    uint ta1 = (texa >> 16u) & 0xFFu;
    bool rgbZero = (texel & 0x00FFFFFFu) == 0u;
    uint a = texel >> 24u;
    if (psm == 1u)
        a = (aem && rgbZero) ? 0u : ta0;
    else if (psm == 2u || psm == 10u)
    {
        if ((a & 0x80u) != 0u)
            a = ta1;
        else
            a = (aem && rgbZero) ? 0u : ta0;
    }
    return (texel & 0x00FFFFFFu) | (a << 24u);
}

int wrapCoordinate(int coordinate, int size, uint mode, uint regionMin, uint regionMax)
{
    switch (mode & 3u)
    {
    case 0u: return int(uint(coordinate) & uint(size - 1));
    case 1u: return clamp(coordinate, 0, size - 1);
    case 2u: return min(max(coordinate, int(regionMin)), int(regionMax));
    case 3u: return int((uint(coordinate) & regionMin) | regionMax);
    }
    return coordinate;
}

bool isFourBit(uint psm) { return psm == 0x14u || psm == 0x24u || psm == 0x2Cu; }
bool isEightBit(uint psm) { return psm == 0x13u || psm == 0x1Bu; }

uint lookupClut(uint s, uint index)
{
    uint mode = st(s, S_TEXMODE);
    uint cpsm = (mode >> 16u) & 0xFFu;
    uint csm = (mode >> 24u) & 0xFFu;
    uint csa = st(s, S_CSA);
    uint sourcePsm = st(s, S_TPSM);
    uint version = st(s, S_CLUT);
    bool sixteenBit = cpsm == 2u || cpsm == 10u;
    uint csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    uint clutBase = (csa & csaMask) << 4u;
    uint sourceIndex = isFourBit(sourcePsm) ? (index & 0x0Fu) : index;
    uint clutIndex = (clutBase + sourceIndex) & (sixteenBit ? 0x1FFu : 0x0FFu);
    if (!sixteenBit && csm == 0u && isEightBit(sourcePsm))
    {
        uint block = min((sourceIndex & 0xF0u) + clutBase, 240u);
        clutIndex = block + (sourceIndex & 0x0Fu);
    }
    if (cpsm == 0u)
        return applyTexa(s, cpsm, clutEntry(version, clutIndex) | (clutEntry(version, clutIndex + 256u) << 16u));
    if (cpsm == 1u)
        return applyTexa(s, cpsm, (clutEntry(version, clutIndex) | (clutEntry(version, clutIndex + 256u) << 16u)) & 0x00FFFFFFu);
    if (sixteenBit)
        return applyTexa(s, cpsm, rgba5551To8888(clutEntry(version, clutIndex)));
    return 0xFFFF00FFu;
}

uint samplePoint(uint s, int u, int v)
{
    uint size = st(s, S_TEXSIZE);
    int texW = int(size & 0xFFFFu);
    int texH = int(size >> 16u);
    uint lo = st(s, S_CLAMP_LO);
    uint hi = st(s, S_CLAMP_HI);
    uint wrapU = lo & 3u;
    uint wrapV = (lo >> 2u) & 3u;
    uint minU = (lo >> 4u) & 0x3FFu;
    uint maxU = (lo >> 14u) & 0x3FFu;
    uint minV = (lo >> 24u) | ((hi & 0x3u) << 8u);
    uint maxV = (hi >> 2u) & 0x3FFu;
    u = wrapCoordinate(u, texW, wrapU, minU, maxU);
    v = wrapCoordinate(v, texH, wrapV, minV, maxV);
    uint psm = st(s, S_TPSM);
    uint raw = 0u;
    uint f = fmtOf(psm);
    if (FMT_KIND[f] != KIND_INVALID)
    {
        uvec2 loc = locate(f, st(s, S_TBP0), st(s, S_TBW), uint(u), uint(v));
        uint epoch = st(s, S_EPOCH);
        if (epoch < uRaster.x)
            loc.x = (pageMap[epoch * 512u + (loc.x >> 13u)] << 13u) | (loc.x & 0x1FFFu);
#if HIRES
        // A page drawn at internal resolution is read from the sample plane picked by the caller (gTexPlane);
        // uploaded textures have the same data in every plane and are read from plane 0, as at 1x.
        uint page = loc.x >> 13u;
        uint saved = gPlaneBase;
        gPlaneBase = ((hiresPages[page >> 5u] >> (page & 31u)) & 1u) != 0u ? gTexPlane * PLANE_WORDS : 0u;
        raw = loadAt(f, loc);
        gPlaneBase = saved;
#else
        raw = loadAt(f, loc);
#endif
    }
    switch (psm)
    {
    case 0x00u: case 0x30u: case 0x01u: case 0x31u:
        return applyTexa(s, psm, raw);
    case 0x02u: case 0x0Au: case 0x32u: case 0x3Au:
        return applyTexa(s, psm, rgba5551To8888(raw));
    case 0x13u: case 0x1Bu: case 0x14u: case 0x24u: case 0x2Cu:
        return lookupClut(s, raw & 0xFFu);
    }
    return 0xFFFF00FFu;
}

#if HIRES
bool texturePageHires(uint s, int u, int v)
{
    uint size = st(s, S_TEXSIZE);
    uint lo = st(s, S_CLAMP_LO);
    uint hi = st(s, S_CLAMP_HI);
    u = wrapCoordinate(u, int(size & 0xFFFFu), lo & 3u, (lo >> 4u) & 0x3FFu, (lo >> 14u) & 0x3FFu);
    v = wrapCoordinate(v, int(size >> 16u), (lo >> 2u) & 3u, (lo >> 24u) | ((hi & 0x3u) << 8u), (hi >> 2u) & 0x3FFu);
    uint f = fmtOf(st(s, S_TPSM));
    if (FMT_KIND[f] == KIND_INVALID)
        return false;
    uvec2 loc = locate(f, st(s, S_TBP0), st(s, S_TBW), uint(u), uint(v));
    uint epoch = st(s, S_EPOCH);
    uint page = loc.x >> 13u;
    if (epoch < uRaster.x)
        page = pageMap[epoch * 512u + page];
    return ((hiresPages[page >> 5u] >> (page & 31u)) & 1u) != 0u;
}

uint sampleGrid(uint s, int hu, int hv, int grid)
{
    int u = hu >= 0 ? hu / grid : -((-hu + grid - 1) / grid);
    int v = hv >= 0 ? hv / grid : -((-hv + grid - 1) / grid);
    gTexPlane = uint(hu - u * grid) + uint(hv - v * grid) * SCALE;
    return samplePoint(s, u, v);
}
#define SAMPLE_TAP(s, u, v) sampleGrid(s, u, v, grid)
#endif
uint lerpChannel(uint c00, uint c10, uint c01, uint c11, float fx, float fy)
{
    precise float top = float(c00) + (float(c10) - float(c00)) * fx;
    precise float bottom = float(c01) + (float(c11) - float(c01)) * fx;
    precise float value = top + (bottom - top) * fy;
    return clampU8(int(value >= 0.0 ? floor(value + 0.5) : ceil(value - 0.5)));
}

float fabsQ(float q)
{
    return abs(q) > 1.0e-8 ? q : 1.0;
}

uint sampleTexture(uint s, float ss, float tt, float q, uint u, uint v)
{
#if HIRES
    if ((gsDebug & 1u) != 0u)
        return 0x80808080u;
#endif
    uint size = st(s, S_TEXSIZE);
    float texW = float(size & 0xFFFFu);
    float texH = float(size >> 16u);
    uint flags = st(s, S_FLAGS);
    precise float texUf;
    precise float texVf;
    if ((flags & F_FST) != 0u)
    {
        texUf = float(u) / 16.0;
        texVf = float(v) / 16.0;
    }
    else
    {
        precise float invQ = 1.0 / fabsQ(q);
        texUf = ss * invQ * texW;
        texVf = tt * invQ * texH;
    }
#if HIRES
    if ((flags & F_LINEAR) == 0u)
    {
        if (gCorner && texturePageHires(s, int(gCornerU), int(gCornerV)))
        {
            gTexPlane = gPlane;
            return samplePoint(s, int(gCornerU), int(gCornerV));
        }
        int nu = int(texUf);
        int nv = int(texVf);
        uint su = uint(clamp(int((texUf - float(nu)) * float(SCALE)), 0, int(SCALE) - 1));
        uint sv = uint(clamp(int((texVf - float(nv)) * float(SCALE)), 0, int(SCALE) - 1));
        gTexPlane = su + sv * SCALE;
        return samplePoint(s, nu, nv);
    }
#else
    if ((flags & F_LINEAR) == 0u)
        return samplePoint(s, int(texUf), int(texVf));
#endif

#if HIRES
    // A page drawn at internal resolution is filtered over the high-resolution grid: texel U of that image is
    // native texel floor(U / SCALE) in plane U mod SCALE, and holds the image at U / SCALE. Native texel x (drawn at x)
    // is filtered as if centred at x + 0.5, so texel U is taken as centred at U / SCALE + 0.5: a 1:1 copy with the
    // usual half-texel offset reads each sample exactly, as at 1x.
    int grid = texturePageHires(s, int(floor(texUf)), int(floor(texVf))) ? int(SCALE) : 1;
    precise float sampleU = grid > 1 ? (texUf - 0.5) * float(grid) : texUf - 0.5;
    precise float sampleV = grid > 1 ? (texVf - 0.5) * float(grid) : texVf - 0.5;
#else
    precise float sampleU = texUf - 0.5;
    precise float sampleV = texVf - 0.5;
#endif
    int u0 = int(floor(sampleU));
    int v0 = int(floor(sampleV));
    precise float fx = sampleU - float(u0);
    precise float fy = sampleV - float(v0);
    uint c00 = SAMPLE_TAP(s, u0, v0);
    uint c10 = SAMPLE_TAP(s, u0 + 1, v0);
    uint c01 = SAMPLE_TAP(s, u0, v0 + 1);
    uint c11 = SAMPLE_TAP(s, u0 + 1, v0 + 1);
    uint r = lerpChannel(c00 & 0xFFu, c10 & 0xFFu, c01 & 0xFFu, c11 & 0xFFu, fx, fy);
    uint g = lerpChannel((c00 >> 8u) & 0xFFu, (c10 >> 8u) & 0xFFu, (c01 >> 8u) & 0xFFu, (c11 >> 8u) & 0xFFu, fx, fy);
    uint b = lerpChannel((c00 >> 16u) & 0xFFu, (c10 >> 16u) & 0xFFu, (c01 >> 16u) & 0xFFu, (c11 >> 16u) & 0xFFu, fx, fy);
    uint a = lerpChannel(c00 >> 24u, c10 >> 24u, c01 >> 24u, c11 >> 24u, fx, fy);
    return r | (g << 8u) | (b << 16u) | (a << 24u);
}

uvec4 combineTexture(uint s, uvec4 vc, uint texel)
{
    uint mode = st(s, S_TEXMODE);
    bool tcc = (mode & 0xFFu) != 0u;
    uint tfx = (mode >> 8u) & 0xFFu;
    int tr = int(texel & 0xFFu);
    int tg = int((texel >> 8u) & 0xFFu);
    int tb = int((texel >> 16u) & 0xFFu);
    int ta = int(texel >> 24u);
    int vr = int(vc.r);
    int vg = int(vc.g);
    int vb = int(vc.b);
    int va = int(vc.a);
    switch (tfx)
    {
    case 0u:
        return uvec4(clampU8((tr * vr) >> 7), clampU8((tg * vg) >> 7), clampU8((tb * vb) >> 7), tcc ? clampU8((ta * va) >> 7) : uint(va));
    case 2u:
        return uvec4(clampU8(((tr * vr) >> 7) + va), clampU8(((tg * vg) >> 7) + va), clampU8(((tb * vb) >> 7) + va), tcc ? clampU8(ta + va) : uint(va));
    case 3u:
        return uvec4(clampU8(((tr * vr) >> 7) + va), clampU8(((tg * vg) >> 7) + va), clampU8(((tb * vb) >> 7) + va), tcc ? uint(ta) : uint(va));
    }
    return uvec4(uint(tr), uint(tg), uint(tb), tcc ? uint(ta) : uint(va));
}

int pickRgb(uint sel, int cs, int cd)
{
    if (sel == 0u)
        return cs;
    if (sel == 1u)
        return cd;
    return 0;
}

)GLSL" R"GLSL(
void writePixel(uint s, int x, int y, uint z, uvec4 c, uint fog)
{
    uint sx = st(s, S_SCISSOR_X);
    uint sy = st(s, S_SCISSOR_Y);
    if (x < int(sx & 0xFFFFu) || x > int(sx >> 16u) || y < int(sy & 0xFFFFu) || y > int(sy >> 16u))
        return;
    uint flags = st(s, S_FLAGS);
    int r = int(c.r);
    int g = int(c.g);
    int b = int(c.b);
    int a = int(c.a);
    if ((flags & F_FGE) != 0u)
    {
        uint fogColor = st(s, S_FOGCOL);
        uint inverseFog = 255u - fog;
        r = int(((fog * uint(r)) >> 8u) + ((inverseFog * (fogColor & 0xFFu)) >> 8u)) & 0xFF;
        g = int(((fog * uint(g)) >> 8u) + ((inverseFog * ((fogColor >> 8u) & 0xFFu)) >> 8u)) & 0xFF;
        b = int(((fog * uint(b)) >> 8u) + ((inverseFog * ((fogColor >> 16u) & 0xFFu)) >> 8u)) & 0xFF;
    }

    uint fbp = st(s, S_FBP);
    uint fbw = st(s, S_FBW);
    uint fpsm = st(s, S_FPSM);
    uint fbmsk = st(s, S_FBMSK);
    uint zbp = st(s, S_ZBP);
    uint zpsm = st(s, S_ZPSM);
    uint test = st(s, S_TEST);

    uvec3 mask = classifyAlphaTest(test, uint(a), fpsm);
    bool writeRgb = mask.x != 0u;
    bool writeAlpha = mask.y != 0u;
    bool writeDepth = mask.z != 0u;
    bool writesFramebuffer = writeRgb || writeAlpha;
    if (!writesFramebuffer && !writeDepth)
        return;

    uint ztestMethod = ((test >> 16u) & 1u) != 0u ? ((test >> 17u) & 3u) : 1u; // ZTE=0: depth test off
    bool abe = (flags & F_ABE) != 0u;
    bool preserveDestinationAlpha = writeRgb && !writeAlpha && fpsm == 0u;
    bool destinationAlphaTestNeedsRead = ((test >> 14u) & 1u) != 0u && (fpsm == 0u || fpsm == 2u || fpsm == 10u);
    bool frmw = destinationAlphaTestNeedsRead || (writesFramebuffer && (fbmsk != 0u || abe || preserveDestinationAlpha));

    uint raw = 0u;
    uint fbrgba = 0u;
    if (frmw)
    {
        raw = readPixel(fpsm, fbp, fbw, uint(x), uint(y));
        fbrgba = raw;
        if (bitsPerPixel(fpsm) == 16u)
            fbrgba = rgba5551To8888(fbrgba);
        else if (fpsm == 1u)
            fbrgba |= 0x80000000u;
    }
    if (!passesDestinationAlphaTest(test, fpsm, raw))
        return;

    bool zpass = false;
    if (ztestMethod == 1u)
        zpass = true;
    else if (ztestMethod == 2u)
        zpass = z >= readPixel(zpsm, zbp, fbw, uint(x), uint(y));
    else if (ztestMethod == 3u)
        zpass = z > readPixel(zpsm, zbp, fbw, uint(x), uint(y));
    if (!zpass)
        return;

    if (writesFramebuffer)
    {
        if (abe && !((flags & F_PABE) != 0u && (a & 0x80) == 0))
        {
            int dr = int(fbrgba & 0xFFu);
            int dg = int((fbrgba >> 8u) & 0xFFu);
            int db = int((fbrgba >> 16u) & 0xFFu);
            int da = int(fbrgba >> 24u);
            uint alphaReg = st(s, S_ALPHA);
            uint asel = alphaReg & 3u;
            uint bsel = (alphaReg >> 2u) & 3u;
            uint csel = (alphaReg >> 4u) & 3u;
            uint dsel = (alphaReg >> 6u) & 3u;
            int fix = int(st(s, S_ALPHA_FIX) & 0xFFu);
            int cAlpha = csel == 0u ? a : (csel == 1u ? da : fix);
            int br = ((pickRgb(asel, r, dr) - pickRgb(bsel, r, dr)) * cAlpha >> 7) + pickRgb(dsel, r, dr);
            int bg = ((pickRgb(asel, g, dg) - pickRgb(bsel, g, dg)) * cAlpha >> 7) + pickRgb(dsel, g, dg);
            int bb = ((pickRgb(asel, b, db) - pickRgb(bsel, b, db)) * cAlpha >> 7) + pickRgb(dsel, b, db);
            if ((flags & F_WRAP) != 0u)
            {
                r = br & 0xFF;
                g = bg & 0xFF;
                b = bb & 0xFF;
            }
            else
            {
                r = int(clampU8(br));
                g = int(clampU8(bg));
                b = int(clampU8(bb));
            }
        }
        if (writeAlpha && (st(s, S_FBA) & 1u) != 0u && fpsm != 1u)
            a = a | 0x80;
        uint pixel = uint(r) | (uint(g) << 8u) | (uint(b) << 16u) | (uint(a & 0xFF) << 24u);
        if (fbmsk != 0u)
            pixel = (pixel & ~fbmsk) | (fbrgba & fbmsk);
        if (preserveDestinationAlpha)
            pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
        if (bitsPerPixel(fpsm) == 16u)
            pixel = rgba8888To5551(pixel);
        writePixelRaw(fpsm, fbp, fbw, uint(x), uint(y), pixel);
    }
    if (writeDepth && st(s, S_ZMASK) == 0u)
        writePixelRaw(zpsm, zbp, fbw, uint(x), uint(y), z);
}

uvec4 unpackColor(uint c)
{
    return uvec4(c & 0xFFu, (c >> 8u) & 0xFFu, (c >> 16u) & 0xFFu, c >> 24u);
}

void drawPoint(uint p, uint s, int x, int y)
{
    if (x != int(pw(p, 1u)) || y != int(pw(p, 2u)))
        return;
    writePixel(s, x, y, pw(p, 3u), unpackColor(pw(p, 4u)), pw(p, 5u));
}

void drawSprite(uint p, uint s, int x, int y)
{
    uint xr = pw(p, 1u);
    uint yr = pw(p, 2u);
    int x0 = int(xr & 0xFFFFu);
    int x1 = int(xr >> 16u);
    int y0 = int(yr & 0xFFFFu);
    int y1 = int(yr >> 16u);
    if (x < x0 || x > x1 || y < y0 || y > y1)
        return;
#if HIRES
    // Sprites cover whole pixels, as at 1x; only the texture is read at the sample's position. Games draw full-screen
    // passes as strips that overlap by half a pixel ([0, 64) and [63.5, 127.5)), which at 1x never shows because
    // pixels are sampled at whole positions: per-sample edges blended the samples in the overlap twice (a bright line
    // every 64 pixels in the heliport).
    float px = float(x) + float(gSubX) / 16.0;
    float py = float(y) + float(gSubY) / 16.0;
#endif
    uint flags = st(s, S_FLAGS);
    uint z = pw(p, 13u);
    uvec4 color = unpackColor(pw(p, 14u));
    uint fog = pw(p, 15u);
    if ((flags & F_TME) == 0u)
    {
        writePixel(s, x, y, z, color, fog);
        return;
    }
    float ox = pf(p, 3u);
    float oy = pf(p, 4u);
    float scaleX = pf(p, 5u);
    float scaleY = pf(p, 6u);
    float u0f = pf(p, 7u);
    float v0f = pf(p, 8u);
    float u1f = pf(p, 9u);
    float v1f = pf(p, 10u);
    precise float ty = (py - oy) * scaleY;
    precise float texVf = v0f + (v1f - v0f) * ty;
    precise float tx = (px - ox) * scaleX;
    precise float texUf = u0f + (u1f - u0f) * tx;
    uint texel;
#if HIRES
    gCorner = true;
    gCornerU = u0f + (u1f - u0f) * ((float(x) - ox) * scaleX);
    gCornerV = v0f + (v1f - v0f) * ((float(y) - oy) * scaleY);
    if ((flags & F_FST) != 0u)
    {
        gCornerU = float(clamp(int(gCornerU * 16.0 + 0.5), 0, 0xFFFF)) / 16.0;
        gCornerV = float(clamp(int(gCornerV * 16.0 + 0.5), 0, 0xFFFF)) / 16.0;
    }
#endif
    if ((flags & F_FST) != 0u)
    {
        uint su = uint(clamp(int(texUf * 16.0 + 0.5), 0, 0xFFFF));
        uint sv = uint(clamp(int(texVf * 16.0 + 0.5), 0, 0xFFFF));
        texel = sampleTexture(s, 0.0, 0.0, 1.0, su, sv);
    }
    else
    {
        uint size = st(s, S_TEXSIZE);
        texel = sampleTexture(s, texUf / float(size & 0xFFFFu), texVf / float(size >> 16u), 1.0, 0u, 0u);
    }
#if HIRES
    gCorner = false;
#endif
    writePixel(s, x, y, z, combineTexture(s, color, texel), fog);
}

)GLSL" R"GLSL(
void drawTriangle(uint p, uint s, int x, int y)
{
    uint xr = pw(p, 1u);
    uint yr = pw(p, 2u);
    if (x < int(xr & 0xFFFFu) || x > int(xr >> 16u) || y < int(yr & 0xFFFFu) || y > int(yr >> 16u))
        return;
    int vx[3];
    int vy[3];
    for (uint i = 0u; i < 3u; ++i)
    {
        vx[i] = int(pw(p, 3u + i * 2u));
        vy[i] = int(pw(p, 4u + i * 2u));
    }
    int sgn = int(pw(p, 9u));
    float invArea = pf(p, 10u);
    int sx = x * 16 + SUB_X;
    int sy = y * 16 + SUB_Y;
    float e[3];
    for (uint i = 0u; i < 3u; ++i)
    {
        uint a = (i + 1u) % 3u;
        uint b = (i + 2u) % 3u;
        int ea = -(vy[b] - vy[a]) * sgn;
        int eb = (vx[b] - vx[a]) * sgn;
        int hi1, lo1, hi2, lo2;
        imulExtended(ea, sx - vx[a], hi1, lo1);
        imulExtended(eb, sy - vy[a], hi2, lo2);
        uint carry;
        uint lo = uaddCarry(uint(lo1), uint(lo2), carry);
        int hi = hi1 + hi2 + int(carry);
        bool bias = ea > 0 || (ea == 0 && eb > 0);
        bool positive = hi > 0 || (hi == 0 && lo != 0u);
        bool zero = hi == 0 && lo == 0u;
        if (!(positive || (zero && bias)))
            return;
        e[i] = float(hi) * 4294967296.0 + float(lo);
    }
    float w0 = e[0] * invArea;
    float w1 = e[1] * invArea;
    float w2 = e[2] * invArea;

#if HIRES
    // Z as a whole base plus a float offset (p[33..35]): the double math below runs at 1/32 rate on consumer GPUs.
    precise float dzf = pf(p, 34u) * w1 + pf(p, 35u) * w2;
    float dzr = floor(dzf + 0.5);
    uint zbase = pw(p, 33u);
    uint zout = dzr >= 0.0 ? (dzr >= float(~zbase) ? 0xFFFFFFFFu : zbase + uint(dzr))
                           : (-dzr >= float(zbase) ? 0u : zbase - uint(-dzr));
#else
    double z0 = pd(p, 11u);
    double z1 = pd(p, 13u);
    double z2 = pd(p, 15u);
    precise double z = z0 + (z1 - z0) * double(w1) + (z2 - z0) * double(w2);
#endif

    uint flags = st(s, S_FLAGS);
    uvec4 c0 = unpackColor(pw(p, 17u));
    uvec4 c1 = unpackColor(pw(p, 18u));
    uvec4 c2 = unpackColor(pw(p, 19u));
    uvec4 color;
    if ((flags & F_IIP) != 0u)
    {
        // Relative to vertex 0, like the GS DDA (start value + gradient * distance): a constant attribute stays exact.
        // The weighted sum c0*w0 + c1*w1 + c2*w2 gave 127 for an all-0x80 alpha wherever w0+w1+w2 < 1 (float rounding),
        // which flipped the game's ATST GREATER 0x7F / AFAIL FB_ONLY test and dropped Z writes (speckled skin and cloth).
        precise vec4 mixed = vec4(c0) + (vec4(c1) - vec4(c0)) * w1 + (vec4(c2) - vec4(c0)) * w2;
        color = uvec4(clampU8(int(mixed.r)), clampU8(int(mixed.g)), clampU8(int(mixed.b)), clampU8(int(mixed.a)));
    }
    else
        color = c2;

    if ((flags & F_TME) != 0u)
    {
        uint texel;
        if ((flags & F_FST) != 0u)
        {
            uint uv0 = pw(p, 29u);
            uint uv1 = pw(p, 30u);
            uint uv2 = pw(p, 31u);
            precise float fu = float(uv0 & 0xFFFFu) * w0 + float(uv1 & 0xFFFFu) * w1 + float(uv2 & 0xFFFFu) * w2;
            precise float fv = float(uv0 >> 16u) * w0 + float(uv1 >> 16u) * w1 + float(uv2 >> 16u) * w2;
            texel = sampleTexture(s, 0.0, 0.0, 1.0, uint(int(fu)) & 0xFFFFu, uint(int(fv)) & 0xFFFFu);
        }
        else
        {
            precise float is = pf(p, 20u) * w0 + pf(p, 23u) * w1 + pf(p, 26u) * w2;
            precise float it = pf(p, 21u) * w0 + pf(p, 24u) * w1 + pf(p, 27u) * w2;
            precise float iq = pf(p, 22u) * w0 + pf(p, 25u) * w1 + pf(p, 28u) * w2;
            texel = sampleTexture(s, is, it, iq, 0u, 0u);
        }
        color = combineTexture(s, color, texel);
    }
    uint fogs = pw(p, 32u);
    precise float fog0 = float(fogs & 0xFFu);
    precise float ffog = fog0 + (float((fogs >> 8u) & 0xFFu) - fog0) * w1 + (float((fogs >> 16u) & 0xFFu) - fog0) * w2;
    uint fog = clampU8(int(ffog));
#if HIRES
    writePixel(s, x, y, zout, color, fog);
#else
    double zc = clamp(z + 0.5, 0.0, 4294967295.0);
    writePixel(s, x, y, uint(zc), color, fog);
#endif
}

bool coversPixel(uint p, int x, int y)
{
    uint header = pw(p, 0u);
    uint type = header & 0xFFu;
    uint xr = pw(p, 1u);
    uint yr = pw(p, 2u);
    if (type == 0u)
        return x == int(xr) && y == int(yr);
    if (x < int(xr & 0xFFFFu) || x > int(xr >> 16u) || y < int(yr & 0xFFFFu) || y > int(yr >> 16u))
        return false;
    if (type != 2u)
        return true;
    int sgn = int(pw(p, 9u));
    int sx = x * 16 + SUB_X;
    int sy = y * 16 + SUB_Y;
    for (uint i = 0u; i < 3u; ++i)
    {
        uint a = (i + 1u) % 3u;
        uint b = (i + 2u) % 3u;
        int vxa = int(pw(p, 3u + a * 2u));
        int vya = int(pw(p, 4u + a * 2u));
        int vxb = int(pw(p, 3u + b * 2u));
        int vyb = int(pw(p, 4u + b * 2u));
        int ea = -(vyb - vya) * sgn;
        int eb = (vxb - vxa) * sgn;
        int hi1, lo1, hi2, lo2;
        imulExtended(ea, sx - vxa, hi1, lo1);
        imulExtended(eb, sy - vya, hi2, lo2);
        uint carry;
        uint lo = uaddCarry(uint(lo1), uint(lo2), carry);
        int hi = hi1 + hi2 + int(carry);
        bool bias = ea > 0 || (ea == 0 && eb > 0);
        bool positive = hi > 0 || (hi == 0 && lo != 0u);
        bool zero = hi == 0 && lo == 0u;
        if (!(positive || (zero && bias)))
            return false;
    }
    return true;
}

void drawPrim(uint p, int x, int y)
{
    uint header = pw(p, 0u);
    uint s = header >> 8u;
    switch (header & 0xFFu)
    {
    case 0u: drawPoint(p, s, x, y); break;
    case 1u: drawSprite(p, s, x, y); break;
    case 2u: drawTriangle(p, s, x, y); break;
    }
}

void main()
{
    uvec4 tile = tiles[gl_WorkGroupID.x];
    int x = int(tile.x * 16u + gl_LocalInvocationID.x);
    int y = int(tile.y * 16u + gl_LocalInvocationID.y);
#if HIRES
    setPlane(gl_WorkGroupID.y);
#endif
    for (uint base = 0u; base < tile.w; base += 32u)
    {
        uint count = min(32u, tile.w - base);
        uint mask = 0u;
        for (uint k = 0u; k < count; ++k)
            if (coversPixel(tileList[tile.z + base + k], x, y))
                mask |= 1u << k;
        while (mask != 0u)
        {
            uint k = uint(findLSB(mask));
            mask &= mask - 1u;
            drawPrim(tileList[tile.z + base + k], x, y);
        }
    }
}
)GLSL";

    const char *const kPageCopy = R"GLSL(
layout(local_size_x = 256) in;
layout(std430, binding = 7) readonly buffer DataBuffer { uint data[]; };

void main()
{
    uint pair = data[gl_WorkGroupID.x];
    uint source = (pair & 0xFFFFu) * 2048u;
    uint target = (pair >> 16u) * 2048u;
#if HIRES
    for (uint plane = 0u; plane < SCALE * SCALE; ++plane)
        for (uint i = gl_LocalInvocationID.x; i < 2048u; i += 256u)
            vram[plane * PLANE_WORDS + target + i] = vram[plane * PLANE_WORDS + source + i];
#else
    for (uint i = gl_LocalInvocationID.x; i < 2048u; i += 256u)
        vram[target + i] = vram[source + i];
#endif
}
)GLSL";

    const char *const kClutLoad = R"GLSL(
layout(local_size_x = 256) in;
layout(std430, binding = 8) readonly buffer ClutOpBuffer { uint clutOps[]; };
uniform uvec4 uClutA;
uniform uvec4 uClutB;

uint clutHalf(uint op, uint hw)
{
    uint p = op * 24u;
    bool fourBit = clutOps[p + 1u] != 0u;
    uint cbp = clutOps[p + 2u];
    uint mode = clutOps[p + 3u];
    uint cbw = clutOps[p + 4u];
    uint cou = clutOps[p + 5u];
    uint cov = clutOps[p + 6u];
    uint cpsm = mode & 0xFFu;
    uint csm = (mode >> 8u) & 0xFFu;
    uint csa = (mode >> 16u) & 0xFFu;
    bool sixteenBit = cpsm == 2u || cpsm == 10u;
    uint csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    uint destinationBase = (csa & csaMask) << 4u;
    bool loadCsm1Suffix = csm == 0u && !sixteenBit && !fourBit;
    uint entry;
    uint shift = 0u;
    if (sixteenBit)
        entry = (hw - destinationBase) & 0x1FFu;
    else
    {
        uint destination = hw & 0xFFu;
        shift = (hw >> 8u) * 16u;
        entry = loadCsm1Suffix ? destination : ((destination - destinationBase) & 0xFFu);
    }
    uint sourceX;
    uint sourceY;
    uint sourceWidth = 1u;
    if (csm == 0u)
    {
        uint sourceIndex = (entry & ~0x18u) | ((entry & 0x08u) << 1u) | ((entry & 0x10u) >> 1u);
        sourceX = sourceIndex & 0x0Fu;
        sourceY = sourceIndex >> 4u;
    }
    else
    {
        sourceWidth = cbw != 0u ? cbw : 1u;
        sourceX = (cou << 4u) + entry;
        sourceY = cov;
    }
    uint raw = readPixelEpoch(cpsm, cbp, sourceWidth, sourceX, sourceY, clutOps[p + 7u], uClutA.y);
    return (raw >> shift) & 0xFFFFu;
}

void main()
{
    uint op = gl_WorkGroupID.x;
    uint lid = gl_LocalInvocationID.x;
    uint p = op * 24u;
    uint target = clutOps[p];
    uint word = 0u;
    for (uint part = 0u; part < 2u; ++part)
    {
        uint hw = lid * 2u + part;
        uint block = hw >> 4u;
        uint writer = (clutOps[p + 8u + (block >> 1u)] >> ((block & 1u) * 16u)) & 0xFFFFu;
        uint value = writer == 0xFFFFu ? ((clut[uClutA.x * 256u + lid] >> (part * 16u)) & 0xFFFFu) : clutHalf(writer, hw);
        word |= value << (part * 16u);
    }
    clut[target * 256u + lid] = word;
}
)GLSL";

    const char *const kUpload = R"GLSL(
layout(local_size_x = 256) in;
layout(std430, binding = 7) readonly buffer DataBuffer { uint data[]; };
uniform uvec4 uUpload;

uint dataByte(uint base, uint index)
{
    return (data[base + (index >> 2u)] >> ((index & 3u) * 8u)) & 0xFFu;
}

void main()
{
    uint entry = data[uUpload.x + gl_WorkGroupID.x];
    uint d = (entry & 0xFFFFu) * 9u;
    uint i = (entry >> 16u) * 256u + gl_LocalInvocationID.x;
    if (i >= data[d + 5u])
        return;
    uint dbp = data[d];
    uint dbw = data[d + 1u];
    uint dpsm = data[d + 2u];
    uint rrw = data[d + 3u];
    uint pixel = data[d + 4u] + i;
    uint x = data[d + 6u] + pixel % rrw;
    uint y = data[d + 7u] + pixel / rrw;
    uint base = data[d + 8u];
    uint value;
    switch (dpsm)
    {
    case 0x00u: case 0x30u:
        value = data[base + i];
        break;
    case 0x01u: case 0x31u:
        value = dataByte(base, i * 3u) | (dataByte(base, i * 3u + 1u) << 8u) | (dataByte(base, i * 3u + 2u) << 16u);
        break;
    case 0x02u: case 0x0Au: case 0x32u: case 0x3Au:
        value = dataByte(base, i * 2u) | (dataByte(base, i * 2u + 1u) << 8u);
        break;
    case 0x13u: case 0x1Bu:
        value = dataByte(base, i);
        break;
    default:
        value = (dataByte(base, i >> 1u) >> ((i & 1u) * 4u)) & 0x0Fu;
        break;
    }
#if HIRES
    for (uint plane = 0u; plane < SCALE * SCALE; ++plane)
    {
        gPlaneBase = plane * PLANE_WORDS;
        writePixelRaw(dpsm, dbp, dbw, x, y, value);
    }
#else
    writePixelRaw(dpsm, dbp, dbw, x, y, value);
#endif
}
)GLSL";

    const char *const kCopyRead = R"GLSL(
layout(local_size_x = 256) in;
layout(std430, binding = 7) buffer DataBuffer { uint data[]; };
uniform uvec4 uSrc;
uniform uvec4 uSize;

void main()
{
    uint i = gl_GlobalInvocationID.x;
    uint rrw = uSize.x;
    uint rrh = uSize.y;
    if (i >= rrw * rrh)
        return;
    uint x = i % rrw;
    uint y = i / rrw;
    if ((uSize.z & 2u) != 0u)
        x = rrw - x - 1u;
    if ((uSize.z & 1u) != 0u)
        y = rrh - y - 1u;
#if HIRES
    gPlaneBase = gl_GlobalInvocationID.y * PLANE_WORDS;
    i += gl_GlobalInvocationID.y * rrw * rrh;
#endif
    data[i] = readPixel(uSrc.z, uSrc.x, uSrc.y, x + (uSrc.w & 0xFFFFu), y + (uSrc.w >> 16u));
}
)GLSL";

    const char *const kCopyWrite = R"GLSL(
layout(local_size_x = 256) in;
layout(std430, binding = 7) readonly buffer DataBuffer { uint data[]; };
uniform uvec4 uDst;
uniform uvec4 uSize;

void main()
{
    uint i = gl_GlobalInvocationID.x;
    uint rrw = uSize.x;
    uint rrh = uSize.y;
    if (i >= rrw * rrh)
        return;
    uint x = i % rrw;
    uint y = i / rrw;
    if ((uSize.z & 2u) != 0u)
        x = rrw - x - 1u;
    if ((uSize.z & 1u) != 0u)
        y = rrh - y - 1u;
#if HIRES
    gPlaneBase = gl_GlobalInvocationID.y * PLANE_WORDS;
    i += gl_GlobalInvocationID.y * rrw * rrh;
#endif
    writePixelRaw(uDst.z, uDst.x, uDst.y, x + (uDst.w & 0xFFFFu), y + (uDst.w >> 16u), data[i]);
}
)GLSL";

    const char *const kClear = R"GLSL(
layout(local_size_x = 16, local_size_y = 16) in;
uniform uvec4 uFrame;
uniform uvec4 uRect;

void main()
{
    uint x = uRect.x + gl_GlobalInvocationID.x;
    uint y = uRect.y + gl_GlobalInvocationID.y;
    if (x > uRect.z || y > uRect.w)
        return;
    uint fbp = uFrame.x;
    uint fbw = uFrame.y & 0xFFFFu;
    uint psm = uFrame.y >> 16u;
    uint mask = uFrame.z;
    uint source = uFrame.w;
    uint pixel = source;
#if HIRES
    gPlaneBase = gl_GlobalInvocationID.z * PLANE_WORDS;
#endif
    if (mask != 0u)
        pixel = (pixel & ~mask) | (readPixel(psm, fbp, fbw, x, y) & mask);
    writePixelRaw(psm, fbp, fbw, x, y, pixel);
}
)GLSL";

    const char *const kPoke = R"GLSL(
layout(local_size_x = 1) in;
layout(std430, binding = 7) buffer DataBuffer { uint data[]; };
uniform uvec4 uArgs;
uniform uvec4 uArgs2;

void main()
{
#if HIRES
    if (uArgs2.y != 0u)
    {
        for (uint plane = 0u; plane < SCALE * SCALE; ++plane)
        {
            gPlaneBase = plane * PLANE_WORDS;
            writePixelRaw(uArgs.x, uArgs.y, uArgs.z, uArgs.w & 0xFFFFu, uArgs.w >> 16u, uArgs2.x);
        }
    }
#else
    if (uArgs2.y != 0u)
        writePixelRaw(uArgs.x, uArgs.y, uArgs.z, uArgs.w & 0xFFFFu, uArgs.w >> 16u, uArgs2.x);
#endif
    else
        data[0] = readPixel(uArgs.x, uArgs.y, uArgs.z, uArgs.w & 0xFFFFu, uArgs.w >> 16u);
}
)GLSL";

    const char *const kPresent = R"GLSL(
layout(local_size_x = 16, local_size_y = 16) in;
layout(std430, binding = 7) buffer DataBuffer { uint data[]; };
uniform uvec4 uOut;
uniform uint uC1[12];
uniform uint uC2[12];
uniform uvec4 uMode;

uint displayToRgba(uint psm, uint raw)
{
    switch (psm & 0x0Fu)
    {
    case 0u: return raw;
    case 1u: return (raw & 0x00FFFFFFu) | 0x80000000u;
    case 2u:
    case 10u:
    {
        uint r = raw & 31u;
        uint g = (raw >> 5u) & 31u;
        uint b = (raw >> 10u) & 31u;
        return ((r << 3u) | (r >> 2u)) | (((g << 3u) | (g >> 2u)) << 8u) | (((b << 3u) | (b >> 2u)) << 16u) |
               ((raw & 0x8000u) != 0u ? 0x80000000u : 0u);
    }
    }
    return raw;
}

bool covers(uint c[12], int rx, int ry)
{
    return c[0] != 0u && rx >= int(c[6]) && rx < int(c[6]) + int(c[8]) && ry >= int(c[7]) && ry < int(c[7]) + int(c[9]);
}

#if HIRES
// rx/ry are display positions in units of 1/SCALE of a display clock (pixel) step.
uint readCircuit(uint c[12], int rx, int ry)
{
    uint bxs = uint((rx - int(c[6]) * int(SCALE)) / int(c[10])) + c[4] * SCALE;
    uint bys = uint((ry - int(c[7]) * int(SCALE)) / int(c[11])) + c[5] * SCALE;
    gPlaneBase = ((bxs % SCALE) + (bys % SCALE) * SCALE) * PLANE_WORDS;
    return displayToRgba(c[3], readPixel(c[3], c[1] * 32u, c[2], (bxs / SCALE) & 0x7FFu, (bys / SCALE) & 0x7FFu));
}
#else
uint readCircuit(uint c[12], int rx, int ry)
{
    uint bx = (uint((rx - int(c[6])) / int(c[10])) + c[4]) & 0x7FFu;
    uint by = (uint((ry - int(c[7])) / int(c[11])) + c[5]) & 0x7FFu;
    return displayToRgba(c[3], readPixel(c[3], c[1] * 32u, c[2], bx, by));
}
#endif

uint blendChannel(uint src, uint dst, uint factor)
{
    int delta = int(src) - int(dst);
    return clampU8(int(dst) + ((delta * int(factor)) / 255));
}

void main()
{
    uint ox = gl_GlobalInvocationID.x;
    uint oy = gl_GlobalInvocationID.y;
    uint width = uOut.x;
    uint height = uOut.y;
    if (ox >= width || oy >= height)
        return;
    int x0 = int(uOut.z & 0xFFFFu);
    int y0 = int(uOut.z >> 16u);
    int hstep = int(uOut.w & 0xFFFFu);
    int vstep = int(uOut.w >> 16u);
#if HIRES
    int ry = y0 * int(SCALE) + int(oy) * vstep + vstep / 2;
    int rx = x0 * int(SCALE) + int(ox) * hstep + hstep / 2;
    uint c1[12] = uC1;
    uint c2[12] = uC2;
    bool in1 = covers(c1, rx / int(SCALE), ry / int(SCALE));
    bool in2 = covers(c2, rx / int(SCALE), ry / int(SCALE));
#else
    int ry = y0 + int(oy) * vstep + vstep / 2;
    int rx = x0 + int(ox) * hstep + hstep / 2;
    uint c1[12] = uC1;
    uint c2[12] = uC2;
    bool in1 = covers(c1, rx, ry);
    bool in2 = covers(c2, rx, ry);
#endif
    uint outPixel = 0xFF000000u;
    if (in1 || in2)
    {
        uint bg = uMode.x;
        uint r = bg & 0xFFu;
        uint g = (bg >> 8u) & 0xFFu;
        uint b = (bg >> 16u) & 0xFFu;
        bool slbg = (uMode.y & 1u) != 0u;
        bool mmod = (uMode.y & 2u) != 0u;
        uint alp = uMode.z;
        if (in2 && !slbg)
        {
            uint color = readCircuit(c2, rx, ry);
            r = color & 0xFFu;
            g = (color >> 8u) & 0xFFu;
            b = (color >> 16u) & 0xFFu;
        }
        if (in1)
        {
            uint color = readCircuit(c1, rx, ry);
            uint factor = mmod ? alp : min(255u, (color >> 24u) * 2u);
            r = blendChannel(color & 0xFFu, r, factor);
            g = blendChannel((color >> 8u) & 0xFFu, g, factor);
            b = blendChannel((color >> 16u) & 0xFFu, b, factor);
        }
        outPixel = r | (g << 8u) | (b << 16u) | 0xFF000000u;
    }
    data[oy * width + ox] = outPixel;
}
)GLSL";

    const char *const kHwVertex = R"GLSL(
layout(std430, binding = 3) readonly buffer PrimBuffer { uint prims[]; };
flat out uint vPrim;
uniform uvec4 uHw;

void main()
{
    uint p = uint(gl_VertexID) / 6u;
    uint c = uint(gl_VertexID) % 6u;
    uint header = prims[p * 40u];
    vPrim = p;
    if (uHw.x != 0u && (header & 0xFFu) == 2u)
    {
        vec2 v[3];
        for (uint i = 0u; i < 3u; ++i)
            v[i] = vec2(float(int(prims[p * 40u + 3u + i * 2u])), float(int(prims[p * 40u + 4u + i * 2u]))) / 16.0 + 0.5;
        vec2 e0 = v[1] - v[0];
        vec2 e1 = v[2] - v[0];
        float orient = e0.x * e1.y - e0.y * e1.x >= 0.0 ? 1.0 : -1.0;
        vec2 n[3];
        bool usable = true;
        for (uint i = 0u; i < 3u; ++i)
        {
            vec2 d = v[(i + 1u) % 3u] - v[i];
            float len = length(d);
            usable = usable && len > 1.0e-3;
            n[i] = len > 1.0e-3 ? vec2(d.y, -d.x) * (orient / len) : vec2(0.0);
        }
        vec2 corner[3];
        for (uint i = 0u; i < 3u; ++i)
        {
            vec2 a = n[(i + 2u) % 3u];
            vec2 b = n[i];
            float denom = 1.0 + dot(a, b);
            usable = usable && denom > 1.0e-4;
            corner[i] = v[i] + (a + b) * (0.75 / max(denom, 1.0e-4));
        }
        if (usable)
        {
            vec2 q = c < 3u ? corner[c] : corner[2];
            gl_Position = vec4(q.x / 1024.0 - 1.0, q.y / 1024.0 - 1.0, 0.0, 1.0);
            return;
        }
    }
    uint xr = prims[p * 40u + 1u];
    uint yr = prims[p * 40u + 2u];
    float x0;
    float x1;
    float y0;
    float y1;
    if ((header & 0xFFu) == 0u)
    {
        x0 = float(xr);
        x1 = x0 + 1.0;
        y0 = float(yr);
        y1 = y0 + 1.0;
    }
    else
    {
        x0 = float(xr & 0xFFFFu);
        x1 = float(xr >> 16u) + 1.0;
        y0 = float(yr & 0xFFFFu);
        y1 = float(yr >> 16u) + 1.0;
    }
    bool right = c == 1u || c == 3u || c == 4u;
    bool bottom = c == 2u || c == 4u || c == 5u;
    gl_Position = vec4((right ? x1 : x0) / 1024.0 - 1.0, (bottom ? y1 : y0) / 1024.0 - 1.0, 0.0, 1.0);
    vPrim = p;
}
)GLSL";

    const char *const kHwFragmentMain = R"GLSL(
layout(pixel_interlock_ordered) in;
flat in uint vPrim;

void main()
{
#if HIRES
    // The framebuffer is SCALE times the GS area: each fragment is one sample of native pixel (x, y).
    int hx = int(gl_FragCoord.x);
    int hy = int(gl_FragCoord.y);
    int x = hx / int(SCALE);
    int y = hy / int(SCALE);
    setPlane(uint(hx - x * int(SCALE)) + uint(hy - y * int(SCALE)) * SCALE);
#else
    int x = int(gl_FragCoord.x);
    int y = int(gl_FragCoord.y);
#endif
    uint s;
    uint z;
    uvec4 c;
    uint fog;
#if HIRES
    bool shaded = coversPixel(vPrim, x, y) && (gsDebug & 4u) == 0u && shadePrim(vPrim, x, y, s, z, c, fog);
    beginInvocationInterlockARB();
    if (shaded && (gsDebug & 2u) == 0u)
        writePixel(s, x, y, z, c, fog);
    endInvocationInterlockARB();
#else
    bool shaded = coversPixel(vPrim, x, y) && shadePrim(vPrim, x, y, s, z, c, fog);
    beginInvocationInterlockARB();
    if (shaded)
        writePixel(s, x, y, z, c, fog);
    endInvocationInterlockARB();
#endif
}
)GLSL";
}
