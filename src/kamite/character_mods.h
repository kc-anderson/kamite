#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kamite::characters
{
    // A playable character model in place of Gene: <exe>/mods/characters/<id>/ holds replacement game files under
    // their disc paths (pl/pl00.dat ...), built by 'build.py models' from your disc and the mod's own data, plus
    // character.ini ([Character] Name=, Credit=).
    struct Character
    {
        std::string id;     // folder name; "gene" = the game's own
        std::string name;   // shown in the launcher
        std::string credit; // shown under it
    };

    // Gene first, then every folder under mods/characters that holds files.
    std::vector<Character> list(const std::filesystem::path &exeDir);

    // After PS2Runtime::setIoPaths, before the game runs: [Mods] Character (KAMITE_CHARACTER). Each replacement is
    // written over the game's file in _DATA.AFS through PS2DiscOverlay, zero-padded to its size (sizes and offsets in
    // the archive never change); a file larger than the game's is left out (logged). A textures/ folder (PCSX2
    // replacement images named after the character's in-game textures) becomes an extra replacement folder of the
    // hardware renderer (PS2X_PCSX2_TEXTURE_EXTRA), so the character draws at full resolution.
    void install(const std::filesystem::path &exeDir);

    // character.ini [SnakeJaw] of the chosen character (Xavier): a jaw skinned to a spare bone that the port turns
    // about a hinge in its parent bone's bind space when the fist hand mesh shows (bone_tweaks). bone < 0: none.
    struct HingedBone
    {
        int bone = -1, parent = -1;
        float pivot[3] = {}, axis[3] = {};
        float closed = 0.0f; // radians the bone turns about the axis when the fist shows
    };
    const HingedBone &snakeJaw();

    // character.ini [Hair]: strands on spare head bones that the port swings (bone_tweaks). bone < 0: none.
    struct HairStrand
    {
        int bone = -1;
        float pivot[3] = {}, dir[3] = {}; // relative to the head bone (4), game bind space; dir = root to tip at rest
        float length = 0.2f;
        bool hinge = false;               // true: swings only about the head's side axis (two strands, one bone)
    };
    struct Hair
    {
        HairStrand back, side;
        float stiffness = 40.0f, damping = 5.0f, gravity = 0.6f, wind = 1.2f;
        float backForward = 8.0f, backBack = 40.0f, backSide = 25.0f, sideSwing = 20.0f; // degrees
    };
    const Hair &hair();

    // Diagnostics for the cloth attachment (Gene's coat, object 0x101 parented to his bone 17 by the player's init at
    // 0x108068): the hook at 0x108088 remembers the player and the coat, and onFrame logs their bone matrices once
    // a few seconds later ([cloth] lines), to work out how an attached model's root is placed.
    void installProbe();
    void onFrame(uint8_t *rdram);
}
