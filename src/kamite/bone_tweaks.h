#pragma once

#include <cstdint>
#include <string>

class PS2Runtime;
struct R5900Context;

// Per-frame changes to the player's bone matrices, applied as the model is packed for drawing and undone at the next
// frame's start, so the game's own motion, blending, IK and hit volumes never see them.
//
// The game's skeleton: cModel (decomp include/godhand/cModel.h) heads the player object (GameWork.playerObj); its
// cParts list (+0x144, one per bone in bone order, 32 for Gene) keeps each bone's world matrix at +0x80 (row vectors:
// the rows are the bone's axes, row 3 its position) under its euler rotation (+0x100, order 0: R = Rz Ry Rx) and
// position (+0xE0). cModel_Trans (0x14A3E8) runs the object's calc virtual, then packs the meshes: its first
// cModel_getMeshPtr call (0x14B6F8 from 0x14A4C0) is the moment the matrices are final and not yet packed.
//
// Backward knees ([Mods] BackwardKnees, launcher: Mods; for Xavier): each knee's forward offset from the hip-ankle
// line is flipped (its sideways offset, knees out, is kept: the pelvis's side axis tells which is which), the thigh
// and shin turned by the least rotation that takes them to the new knee, the foot untouched; every motion the game
// plays keeps its feet and hips, the legs bend the other way.
namespace kamite::bone_tweaks
{
    void install(PS2Runtime &runtime);
    void onFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime); // frame start: the original matrices back
    bool backwardKnees();
    void setBackwardKnees(bool on);
    // The player's visible hand meshes (RH_xx / LH_xx node names), for the F2 Debug tab: which hand shape the game
    // shows (the fist / open hand / grab meshes it swaps).
    std::string handNodes(const uint8_t *rdram);
}
