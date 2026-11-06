#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Cash (bone_tweaks): the women's seat on two spare bones, one per cheek, so it can be shaped at draw time.
//
// The female skeleton (Money's test: bone 29 on the upper spine) has no bone for the buttocks: the seat is skinned to
// the pelvis (17) and the thighs (18 / 22). Bones 27 and 28 are root-level helpers no woman's mesh is skinned to. At
// boot the women's models on the disc are re-skinned (a same-size disc overlay: only weight bytes change): each rear
// seat vertex's pelvis weight moves to its cheek's bone (27 on bone 18's side, 28 on 22's), a vertex on the centre line
// splits it between both when it has a free weight slot; thigh weights stay. At draw time each cheek bone is the
// pelvis times a growth about a pivot near the top of the hip, so at 100% (no growth) every vertex lands where the
// game puts it.
namespace kamite::cash_skin
{
    constexpr int kPelvis = 17, kThighA = 18, kThighB = 22, kCheekA = 27, kCheekB = 28;

    // Measures in the pelvis' bind frame for a woman of the usual build (thighs 0.085 m to the side); scaled by the
    // model's own hip width.
    constexpr float kHipRef = 0.085f;
    constexpr float kPivotX = 0.075f, kPivotY = -0.005f, kPivotZ = -0.09f; // the cheek's pivot (back is -z)
    constexpr float kCentre = 0.012f;                                       // |x| below this: the centre line
    constexpr float kBandBelow = 0.05f, kBandAbove = 0.04f;                 // the seat band: thigh joint - 0.05 .. pelvis + 0.04

    // Re-skins one model (an "scr" block as on the disc, `size` bytes) in place. Returns the vertices moved; first /
    // last changed byte in lo / hi (lo > hi when nothing changed). 0 when the block is not a female model with free 27 / 28.
    size_t reskin(uint8_t *scr, size_t size, size_t &lo, size_t &hi);
}
